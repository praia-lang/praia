#include "../builtins.h"
#include "../cancellation.h"  // CancellationState + CancelScope (thread-local current-token)
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include "../gc_heap.h"

void registerConcurrencyBuiltins(Interpreter* /*self*/, std::shared_ptr<Environment> globals) {
    // self is no longer needed: native callbacks resolve the calling
    // Interpreter at invocation time via g_currentInterp (set by
    // NativeFunction::call). Parameter kept for API stability.
    globals->define("Lock", Value(makeNative("Lock", 0,
        [](const std::vector<Value>&) -> Value {
            auto mtx = std::make_shared<std::recursive_mutex>();
            auto lock = gcNew<PraiaMap>();

            lock->entries[Value("acquire")] = Value(makeNative("acquire", 0,
                [mtx](const std::vector<Value>&) -> Value {
                    mtx->lock();
                    return Value();
                }));

            lock->entries[Value("release")] = Value(makeNative("release", 0,
                [mtx](const std::vector<Value>&) -> Value {
                    mtx->unlock();
                    return Value();
                }));

            // withLock(fn) — acquire, call fn, release on return *or* throw.
            // The lock_guard's destructor releases on stack unwind, so the
            // mutex is always released even when fn throws. We do NOT roll
            // back any state fn mutated before throwing — Lock has no idea
            // what user state is being guarded. Document the "build new
            // state locally, commit on the last line" pattern for users.
            //
            // Use g_currentInterp (set by NativeFunction::call) rather than
            // the registration-time captured `self`. An async task's user
            // callback would otherwise run on the parent Interpreter and
            // race with its env field — see the comment on g_currentInterp.
            lock->entries[Value("withLock")] = Value(makeNative("withLock", 1,
                [mtx](const std::vector<Value>& args) -> Value {
                    if (!args[0].isCallable())
                        throw RuntimeError("withLock() requires a function", 0);
                    std::lock_guard<std::recursive_mutex> guard(*mtx);
                    return callSafe(*g_currentInterp, args[0].asCallable(), {});
                }));

            return Value(lock);
        })));

    // ── Queue() — thread-safe FIFO queue for async communication ──
    //
    // `Queue()` (no arg) is unbounded — `send` never blocks unless the
    // queue is closed. `Queue(N)` is bounded; `send` blocks while the
    // buffer holds N values, providing backpressure. Both are
    // closeable; receivers see `nil` once the queue is closed and
    // drained. Named to match the semantic (Python's queue.Queue,
    // Java's BlockingQueue) rather than Go's rendezvous channel.
    //
    // The deprecated `Channel` alias below points at the same factory
    // and prints a one-shot stderr warning on first use.
    struct QueueState {
        std::mutex mtx;
        std::condition_variable cv;
        std::queue<Value> buffer;
        bool closed = false;
    };

    auto queueFactory = [](const std::vector<Value>& args) -> Value {
        auto state = std::make_shared<QueueState>();
        int capacity = 0; // 0 = unbounded
        if (!args.empty() && args[0].isNumber())
            capacity = static_cast<int>(args[0].asNumber());

        auto q = gcNew<PraiaMap>();

        q->entries[Value("send")] = Value(makeNative("send", 1,
            [state, capacity](const std::vector<Value>& args) -> Value {
                std::unique_lock<std::mutex> lock(state->mtx);
                if (state->closed)
                    throw RuntimeError("Cannot send on a closed queue", 0);
                if (capacity > 0) {
                    state->cv.wait(lock, [&] {
                        return static_cast<int>(state->buffer.size()) < capacity || state->closed;
                    });
                    if (state->closed)
                        throw RuntimeError("Cannot send on a closed queue", 0);
                }
                state->buffer.push(args[0]);
                state->cv.notify_all();
                return Value();
            }));

        q->entries[Value("recv")] = Value(makeNative("recv", 0,
            [state](const std::vector<Value>&) -> Value {
                std::unique_lock<std::mutex> lock(state->mtx);
                state->cv.wait(lock, [&] {
                    return !state->buffer.empty() || state->closed;
                });
                if (state->buffer.empty()) return Value(); // closed + empty = nil
                Value val = state->buffer.front();
                state->buffer.pop();
                state->cv.notify_all();
                return val;
            }));

        q->entries[Value("tryRecv")] = Value(makeNative("tryRecv", 0,
            [state](const std::vector<Value>&) -> Value {
                std::lock_guard<std::mutex> lock(state->mtx);
                if (state->buffer.empty()) return Value();
                Value val = state->buffer.front();
                state->buffer.pop();
                state->cv.notify_all();
                return val;
            }));

        q->entries[Value("close")] = Value(makeNative("close", 0,
            [state](const std::vector<Value>&) -> Value {
                std::lock_guard<std::mutex> lock(state->mtx);
                state->closed = true;
                state->cv.notify_all();
                return Value();
            }));

        // isClosed() — true once close() has been called, regardless of
        // whether the buffer still has values. This is what a *producer*
        // wants to consult before send(): "can I still send, or will
        // send() throw?". The legacy closed() method confuses producers
        // by returning false until the buffer drains.
        q->entries[Value("isClosed")] = Value(makeNative("isClosed", 0,
            [state](const std::vector<Value>&) -> Value {
                std::lock_guard<std::mutex> lock(state->mtx);
                return Value(state->closed);
            }));

        // isEmpty() — true when the buffer has no pending values. What
        // a *consumer* wants to consult before tryRecv() / a peek-style
        // check. Orthogonal to isClosed().
        q->entries[Value("isEmpty")] = Value(makeNative("isEmpty", 0,
            [state](const std::vector<Value>&) -> Value {
                std::lock_guard<std::mutex> lock(state->mtx);
                return Value(state->buffer.empty());
            }));

        // closed() — "fully drained": close() has been called AND the
        // buffer is empty. Equivalent to isClosed() && isEmpty().
        q->entries[Value("closed")] = Value(makeNative("closed", 0,
            [state](const std::vector<Value>&) -> Value {
                std::lock_guard<std::mutex> lock(state->mtx);
                return Value(state->closed && state->buffer.empty());
            }));

        return Value(q);
    };

    globals->define("Queue", Value(makeNative("Queue", -1, queueFactory)));

    // Channel — deprecated alias for Queue. Prints a one-shot warning
    // to stderr on first use so existing callers see the rename
    // without breaking. Remove at 1.0.
    static std::atomic<bool> channelWarned{false};
    globals->define("Channel", Value(makeNative("Channel", -1,
        [queueFactory](const std::vector<Value>& args) -> Value {
            if (!channelWarned.exchange(true)) {
                std::cerr << "[deprecated] Channel() is now Queue(); rename "
                             "the constructor (methods unchanged)\n";
            }
            return queueFactory(args);
        })));

    // ── CancellationToken() — cross-VM cooperative cancel signal ──
    //
    // A token is a flag that any task can flip to "cancelled". Pass it to
    // long-running async tasks and have them poll `cancelled()` in their
    // loop to bail out cleanly. Same shared-state-via-native-closures trick
    // as Channel/SharedMap, so the token survives async deep-copy.
    //
    // The CancellationState struct itself lives in src/cancellation.h so
    // the plugin runtime shim (praia::shouldCancel) can read the same
    // flag through a thread-local pointer set by withCancel below.
    using praia::detail::CancellationState;

    globals->define("CancellationToken", Value(makeNative("CancellationToken", 0,
        [](const std::vector<Value>&) -> Value {
            auto state = std::make_shared<CancellationState>();
            auto tok = gcNew<PraiaMap>();

            tok->entries[Value("cancel")] = Value(makeNative("cancel", 0,
                [state](const std::vector<Value>&) -> Value {
                    state->cancelled.store(true, std::memory_order_release);
                    return Value();
                }));

            tok->entries[Value("cancelled")] = Value(makeNative("cancelled", 0,
                [state](const std::vector<Value>&) -> Value {
                    return Value(state->cancelled.load(std::memory_order_acquire));
                }));

            // throwIfCancelled() — convenience for "fail fast" loops.
            // Tasks doing `while (...) { token.throwIfCancelled(); ... }`
            // can let the surrounding try/catch handle the bail-out.
            tok->entries[Value("throwIfCancelled")] = Value(makeNative("throwIfCancelled", 0,
                [state](const std::vector<Value>&) -> Value {
                    if (state->cancelled.load(std::memory_order_acquire))
                        throw RuntimeError("cancelled", 0);
                    return Value();
                }));

            // Hidden _state slot: a PraiaExternal whose data points
            // directly at the same CancellationState the closures
            // capture. withCancel reads this to find the address of
            // the atomic flag without going through a callable
            // invocation. Ownership is anchored by the std::function
            // deleter, which holds a shared_ptr<CancellationState> in
            // its capture — that ref keeps the state alive as long as
            // the External is reachable, paralleling the closures'
            // own captures. When the External destructs, the deleter
            // body (a no-op) runs, then the std::function destructor
            // releases the captured shared_ptr.
            auto ext = gcNew<PraiaExternal>();
            ext->data = static_cast<void*>(state.get());
            ext->typeName = "praia.CancellationState";
            ext->deleter = [keep = state](void* /*p*/) { (void)keep; };
            tok->entries[Value("_state")] = Value(ext);

            return Value(tok);
        })));

    // ── withCancel(token, fn, ...args) — bind token to the current scope ──
    //
    // Runs `fn(...args)` with `token`'s cancellation state published
    // through the thread-local `g_currentCancel` pointer. Plugin native
    // code calls `praia::shouldCancel()` to read it. Restores the
    // previous binding on every exit path (including exceptions from
    // fn) via the CancelScope RAII guard.
    //
    // Composes with async: `async(lam { withCancel(tok, () -> heavyWork()) })`
    // — the thread-local is set on the task's worker thread for the
    // duration of fn, and torn down when fn returns. Outside any
    // withCancel scope, shouldCancel() returns nullopt and native code
    // treats that as "no cancellation requested".
    globals->define("withCancel", Value(makeNative("withCancel", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 2)
                throw RuntimeError(
                    "withCancel(token, fn, ...args) requires at least 2 arguments", 0);
            // Extract the CancellationState pointer from the token's
            // hidden _state slot. The error wording distinguishes the
            // two ways this can fail so a confused user can fix it
            // without reading the source.
            if (!args[0].isMap())
                throw RuntimeError(
                    "withCancel(token, ...): first argument must be a CancellationToken", 0);
            auto& tokMap = args[0].asMap()->entries;
            auto it = tokMap.find(Value("_state"));
            if (it == tokMap.end() || !it->second.isExternal() ||
                it->second.asExternal()->typeName != "praia.CancellationState") {
                throw RuntimeError(
                    "withCancel(token, ...): first argument must be a CancellationToken "
                    "produced by CancellationToken() (got a map without a _state slot)", 0);
            }
            auto* state = static_cast<CancellationState*>(
                it->second.asExternal()->data);

            if (!args[1].isCallable())
                throw RuntimeError(
                    "withCancel(token, fn, ...): second argument must be callable", 0);

            std::vector<Value> callArgs(args.begin() + 2, args.end());

            // RAII: restores previous scope even if fn throws.
            praia::detail::CancelScope scope(state);
            return callSafe(*g_currentInterp, args[1].asCallable(), callArgs);
        })));

    // ── SharedMap() — cross-VM key-value store ──
    //
    // The deep-copy that `async` does on globals/args copies the PraiaMap
    // wrapper but each method's std::shared_ptr<SharedMapState> capture is
    // preserved through callable cloning, so all copies of the wrapper
    // address the same C++ state. Same trick as Channel.
    struct SharedMapState {
        std::recursive_mutex mtx;
        std::unordered_map<Value, Value, ValueHash, ValueKeyEqual> entries;
    };

    globals->define("SharedMap", Value(makeNative("SharedMap", 0,
        [](const std::vector<Value>&) -> Value {
            auto state = std::make_shared<SharedMapState>();
            auto m = gcNew<PraiaMap>();

            m->entries[Value("set")] = Value(makeNative("set", 2,
                [state](const std::vector<Value>& args) -> Value {
                    std::lock_guard<std::recursive_mutex> g(state->mtx);
                    state->entries[args[0]] = args[1];
                    return Value();
                }));

            m->entries[Value("get")] = Value(makeNative("get", -1,
                [state](const std::vector<Value>& args) -> Value {
                    if (args.empty())
                        throw RuntimeError("SharedMap.get() requires a key", 0);
                    std::lock_guard<std::recursive_mutex> g(state->mtx);
                    auto it = state->entries.find(args[0]);
                    if (it != state->entries.end()) return it->second;
                    if (args.size() > 1) return args[1];
                    return Value();
                }));

            m->entries[Value("has")] = Value(makeNative("has", 1,
                [state](const std::vector<Value>& args) -> Value {
                    std::lock_guard<std::recursive_mutex> g(state->mtx);
                    return Value(state->entries.find(args[0]) != state->entries.end());
                }));

            m->entries[Value("delete")] = Value(makeNative("delete", 1,
                [state](const std::vector<Value>& args) -> Value {
                    std::lock_guard<std::recursive_mutex> g(state->mtx);
                    auto it = state->entries.find(args[0]);
                    if (it == state->entries.end()) return Value(false);
                    state->entries.erase(it);
                    return Value(true);
                }));

            // update(k, fn) — atomic read-modify-write. fn receives the
            // current value (or nil) and returns the new value to store.
            // The lock is held across fn, so don't do I/O inside it.
            //
            // Throw semantics: the slot write only happens on successful
            // return, so a throw leaves entries[k] untouched. *But* `current`
            // is passed by reference — if fn mutates it via aliases (e.g.
            // s.foo = bar) before throwing, those mutations stick even
            // though the slot wasn't reassigned. Document that gotcha for
            // users; we don't snapshot here (deep-copy would change the
            // semantics of the existing in-place mutation pattern).
            // Use g_currentInterp (set by NativeFunction::call) so that
            // an async task running m.update invokes the user callback on
            // the *task's* Interpreter, not the parent's — see comment on
            // g_currentInterp.
            m->entries[Value("update")] = Value(makeNative("update", 2,
                [state](const std::vector<Value>& args) -> Value {
                    if (!args[1].isCallable())
                        throw RuntimeError("SharedMap.update() requires a function", 0);
                    std::lock_guard<std::recursive_mutex> g(state->mtx);
                    Value current;
                    auto it = state->entries.find(args[0]);
                    if (it != state->entries.end()) current = it->second;
                    Value next = callSafe(*g_currentInterp, args[1].asCallable(), {current});
                    state->entries[args[0]] = next;
                    return next;
                }));

            m->entries[Value("keys")] = Value(makeNative("keys", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::lock_guard<std::recursive_mutex> g(state->mtx);
                    auto arr = gcNew<PraiaArray>();
                    arr->elements.reserve(state->entries.size());
                    for (auto& [k, _] : state->entries) arr->elements.push_back(k);
                    return Value(arr);
                }));

            m->entries[Value("values")] = Value(makeNative("values", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::lock_guard<std::recursive_mutex> g(state->mtx);
                    auto arr = gcNew<PraiaArray>();
                    arr->elements.reserve(state->entries.size());
                    for (auto& [_, v] : state->entries) arr->elements.push_back(v);
                    return Value(arr);
                }));

            m->entries[Value("size")] = Value(makeNative("size", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::lock_guard<std::recursive_mutex> g(state->mtx);
                    return Value(static_cast<int64_t>(state->entries.size()));
                }));

            m->entries[Value("clear")] = Value(makeNative("clear", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::lock_guard<std::recursive_mutex> g(state->mtx);
                    state->entries.clear();
                    return Value();
                }));

            return Value(m);
        })));

    // ── futures namespace ──

    auto asyncMap = gcNew<PraiaMap>();

    asyncMap->entries[Value("all")] = Value(makeNative("futures.all", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("async.all() requires an array of futures", 0);
            auto& futures = args[0].asArray()->elements;
            auto results = gcNew<PraiaArray>();
            for (auto& f : futures) {
                if (!f.isFuture())
                    throw RuntimeError("async.all() array must contain only futures", 0);
                results->elements.push_back(f.asFuture()->future.get());
            }
            return Value(results);
        }));

    asyncMap->entries[Value("race")] = Value(makeNative("futures.race", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("async.race() requires an array of futures", 0);
            auto& futures = args[0].asArray()->elements;
            if (futures.empty())
                throw RuntimeError("async.race() requires at least one future", 0);
            for (auto& f : futures) {
                if (!f.isFuture())
                    throw RuntimeError("async.race() array must contain only futures", 0);
            }

            // Quick check for any future already satisfied — avoids spawning
            // waiter threads when the race is already decided.
            for (size_t i = 0; i < futures.size(); i++) {
                auto& sf = futures[i].asFuture()->future;
                if (sf.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                    return sf.get();
            }

            // Spawn one waiter thread per future; each blocks on f.wait()
            // (using the future's internal notification — no polling) and
            // races to claim the win via compare_exchange. The first to
            // succeed signals the condition variable. Loser threads finish
            // when their own future eventually completes; we detach them.
            auto cv = std::make_shared<std::condition_variable>();
            auto mtx = std::make_shared<std::mutex>();
            auto winner = std::make_shared<std::atomic<int>>(-1);

            std::vector<std::thread> waiters;
            waiters.reserve(futures.size());
            for (size_t i = 0; i < futures.size(); i++) {
                auto sf = futures[i].asFuture()->future;
                int idx = static_cast<int>(i);
                waiters.emplace_back([sf, cv, mtx, winner, idx]() {
                    sf.wait();
                    int expected = -1;
                    if (winner->compare_exchange_strong(expected, idx)) {
                        std::lock_guard<std::mutex> lk(*mtx);
                        cv->notify_one();
                    }
                });
            }

            {
                std::unique_lock<std::mutex> lk(*mtx);
                cv->wait(lk, [&] { return winner->load() != -1; });
            }

            // Detach losers — they'll exit naturally when their future
            // completes (and their compare_exchange will harmlessly fail).
            for (auto& t : waiters) t.detach();

            return futures[winner->load()].asFuture()->future.get();
        }));

    globals->define("futures", Value(asyncMap));
}
