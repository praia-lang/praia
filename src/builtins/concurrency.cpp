#include "../builtins.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include "../gc_heap.h"

void registerConcurrencyBuiltins(Interpreter* self, std::shared_ptr<Environment> globals) {
    globals->define("Lock", Value(makeNative("Lock", 0,
        [self](const std::vector<Value>&) -> Value {
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

            lock->entries[Value("withLock")] = Value(makeNative("withLock", 1,
                [mtx, self](const std::vector<Value>& args) -> Value {
                    if (!args[0].isCallable())
                        throw RuntimeError("withLock() requires a function", 0);
                    std::lock_guard<std::recursive_mutex> guard(*mtx);
                    return callSafe(*self, args[0].asCallable(), {});
                }));

            return Value(lock);
        })));

    // ── Channel() — thread-safe queue for async communication ──

    struct ChannelState {
        std::mutex mtx;
        std::condition_variable cv;
        std::queue<Value> buffer;
        bool closed = false;
    };

    globals->define("Channel", Value(makeNative("Channel", -1,
        [](const std::vector<Value>& args) -> Value {
            auto state = std::make_shared<ChannelState>();
            int capacity = 0; // unbuffered by default
            if (!args.empty() && args[0].isNumber())
                capacity = static_cast<int>(args[0].asNumber());

            auto ch = gcNew<PraiaMap>();

            ch->entries[Value("send")] = Value(makeNative("send", 1,
                [state, capacity](const std::vector<Value>& args) -> Value {
                    std::unique_lock<std::mutex> lock(state->mtx);
                    if (state->closed)
                        throw RuntimeError("Cannot send on a closed channel", 0);
                    if (capacity > 0) {
                        state->cv.wait(lock, [&] {
                            return static_cast<int>(state->buffer.size()) < capacity || state->closed;
                        });
                        if (state->closed)
                            throw RuntimeError("Cannot send on a closed channel", 0);
                    }
                    state->buffer.push(args[0]);
                    state->cv.notify_all();
                    return Value();
                }));

            ch->entries[Value("recv")] = Value(makeNative("recv", 0,
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

            ch->entries[Value("tryRecv")] = Value(makeNative("tryRecv", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    if (state->buffer.empty()) return Value(); // nil
                    Value val = state->buffer.front();
                    state->buffer.pop();
                    state->cv.notify_all();
                    return val;
                }));

            ch->entries[Value("close")] = Value(makeNative("close", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    state->closed = true;
                    state->cv.notify_all();
                    return Value();
                }));

            ch->entries[Value("closed")] = Value(makeNative("closed", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    return Value(state->closed && state->buffer.empty());
                }));

            return Value(ch);
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
