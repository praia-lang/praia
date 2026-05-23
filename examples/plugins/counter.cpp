// Demo plugin: opaque handles via praia::makeExternal.
//
// Wraps a tiny in-process counter as a native handle. Shows the
// lifecycle a real plugin would use for DB connections, file
// handles, etc.: create returns a handle, methods accept the
// handle and operate on the underlying C++ object, and the
// destructor fires automatically when the last reference drops.
//
// Also exposes a `_destructorCount` getter — a side channel used
// by the test suite to verify the GC actually invokes the
// deleter on collection. A real plugin wouldn't ship this.

#include "praia_plugin.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

PRAIA_DECLARE_ABI();
PRAIA_PLUGIN_METADATA("counter", "0.1.0",
                     "Demo: opaque handles via praia::makeExternal");

namespace {

struct Counter {
    int64_t n = 0;
};

// Process-global tally of how many Counter instances have been
// destroyed. Used by tests to observe GC-driven destruction.
std::atomic<int64_t> g_destructorCount{0};

void deleteCounter(Counter* c) {
    g_destructorCount.fetch_add(1, std::memory_order_relaxed);
    delete c;
}

constexpr const char* kTag = "counter.handle";

// Plugin-held callback across native calls. Pinned via
// praia::pinValue so the GC's sweep doesn't hollow out the
// callable's Environment between when setStored stashes it and
// callStored fires it. A real plugin would use this pattern for
// a callback registry (`mod.on("event", handler)`), a cached
// callable, or a singleton. Default-constructed Value is nil;
// we treat nil as "no callback stored" — no separate flag needed.
Value g_storedCallback;

}  // namespace

extern "C" void praia_register(PraiaMap* module) {
    // counter.new() -> handle
    module->entries["new"] = Value(makeNative("counter.new", 0,
        [](const std::vector<Value>&) -> Value {
            return praia::makeExternal<Counter>(new Counter,
                                                kTag, deleteCounter);
        }));

    // counter.inc(handle) -> new value
    module->entries["inc"] = Value(makeNative("counter.inc", 1,
        [](const std::vector<Value>& args) -> Value {
            auto* c = praia::getExternal<Counter>(args[0], kTag);
            c->n++;
            return Value(c->n);
        },
        {"handle"}));

    // counter.value(handle) -> current value
    module->entries["value"] = Value(makeNative("counter.value", 1,
        [](const std::vector<Value>& args) -> Value {
            auto* c = praia::getExternal<Counter>(args[0], kTag);
            return Value(c->n);
        },
        {"handle"}));

    // counter.reset(handle) -> nil
    module->entries["reset"] = Value(makeNative("counter.reset", 1,
        [](const std::vector<Value>& args) -> Value {
            auto* c = praia::getExternal<Counter>(args[0], kTag);
            c->n = 0;
            return Value();
        },
        {"handle"}));

    // counter._destructorCount() -> int. Test-only side channel.
    // Exposes how many Counter instances have been destroyed so a
    // test can verify the GC actually ran the deleter.
    module->entries["_destructorCount"] = Value(makeNative(
        "counter._destructorCount", 0,
        [](const std::vector<Value>&) -> Value {
            return Value(static_cast<int64_t>(
                g_destructorCount.load(std::memory_order_relaxed)));
        }));

    // counter.setStored(cb) — stash a callable across calls, pinned
    // as a GC root so the next collection doesn't hollow out its
    // closure environment. Mirrors the pattern any real plugin's
    // event-callback registry would use.
    module->entries["setStored"] = Value(makeNative("counter.setStored", 1,
        [](const std::vector<Value>& args) -> Value {
            // Type-check the argument; the returned shared_ptr is
            // unused — args[0] flows through as a Value.
            praia::requireCallable(args, 0, "counter.setStored");
            // Replacing an existing stored callback releases the old
            // pin first — leaks the pin otherwise.
            if (!g_storedCallback.isNil()) praia::unpinValue(g_storedCallback);
            g_storedCallback = args[0];
            praia::pinValue(g_storedCallback);
            return Value();
        },
        {"cb"}));

    // counter.callStored() — invoke the stashed callable via
    // praia::call. Without the pin, GC pressure between setStored
    // and callStored could clear the callable's environment;
    // calling it would then fail at variable lookup or worse.
    module->entries["callStored"] = Value(makeNative("counter.callStored", 0,
        [](const std::vector<Value>&) -> Value {
            if (g_storedCallback.isNil())
                praia::error("counter.callStored: no callback stored");
            return praia::call(g_storedCallback.asCallable(), {});
        }));

    // counter.clearStored() — release the pin and drop the stash.
    // Idempotent so test teardown can call it unconditionally.
    module->entries["clearStored"] = Value(makeNative("counter.clearStored", 0,
        [](const std::vector<Value>&) -> Value {
            if (!g_storedCallback.isNil()) {
                praia::unpinValue(g_storedCallback);
                g_storedCallback = Value();
            }
            return Value();
        }));

    // counter.scheduleAsync(delayMs, cb) — demo of praia::postToEngine.
    // Spawns a detached worker thread that sleeps for `delayMs` and
    // then marshals `cb()` back to the engine that called us. This is
    // the canonical pattern any plugin doing background I/O would
    // use; the test exercises the round-trip.
    // counter.scheduleAsync(delayMs, cb, ...args) — spawns a detached
    // worker that sleeps `delayMs` then marshals `cb(...args)` back
    // to the engine thread. Any trailing args after `cb` are passed
    // through to the callback verbatim, so the test can verify the
    // queue carries args across the thread hop.
    //
    // GC contract: every Value that has to survive the worker→engine
    // hop is pinned on the engine thread BEFORE the worker spawns
    // (workers can't pin from their own thread — no executor). The
    // matching unpin runs on the engine thread inside a wrapper
    // callable that's what we actually post, so each pinValue has a
    // guaranteed pairing without burdening the user callback or
    // making the test responsible for the cleanup.
    module->entries["scheduleAsync"] = Value(makeNative("counter.scheduleAsync", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                praia::error("counter.scheduleAsync(delayMs, cb, ...args) "
                             "requires at least 2 arguments");
            }
            int delayMs = static_cast<int>(
                praia::requireNumber(args, 0, "counter.scheduleAsync"));
            praia::requireCallable(args, 1, "counter.scheduleAsync");

            // Pin everything that has to survive the worker→engine
            // gap. `pinned` is what the wrapper unpins after the
            // user callback returns. Non-GC values (numbers, strings)
            // no-op inside pinValue, but we keep them in `pinned` so
            // the unpin loop stays uniform.
            std::vector<Value> pinned;
            pinned.reserve(args.size() - 1);
            for (size_t i = 1; i < args.size(); i++) {
                praia::pinValue(args[i]);
                pinned.push_back(args[i]);
            }

            // Trailing args (args[2..]) are what we actually pass to
            // the user callback. Copy the slice so the worker's
            // capture has its own vector.
            std::vector<Value> cbArgs;
            cbArgs.reserve(args.size() - 2);
            for (size_t i = 2; i < args.size(); i++) {
                cbArgs.push_back(args[i]);
            }

            // Build the wrapper that the worker will post. The
            // wrapper runs on the engine thread: invokes the user
            // callback, then unpins every pinned Value, then
            // returns. This way the worker doesn't need to touch
            // pinValue/unpinValue (it can't — wrong thread) and the
            // user callback doesn't need to clean up after itself.
            auto userCb = args[1].asCallable();
            std::shared_ptr<Callable> wrapper = makeNative(
                "counter.scheduleAsync.wrap", -1,
                [userCb, pinned = std::move(pinned)]
                (const std::vector<Value>& wrapArgs) -> Value {
                    Value result;
                    try {
                        result = praia::call(userCb, wrapArgs);
                    } catch (...) {
                        // The wrapper MUST unpin even if the user
                        // callback throws — otherwise an exception
                        // turns this scheduleAsync into a permanent
                        // pin leak. Rethrow after cleanup; the
                        // engine's drainPosted catches it.
                        for (auto& v : pinned) praia::unpinValue(v);
                        throw;
                    }
                    for (auto& v : pinned) praia::unpinValue(v);
                    return result;
                });

            void* engine = praia::currentExecutor();
            std::thread([engine, wrapper, cbArgs = std::move(cbArgs), delayMs]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                praia::postToEngine(engine, wrapper, cbArgs);
            }).detach();
            return Value();
        },
        {"delayMs", "cb"}));

}
