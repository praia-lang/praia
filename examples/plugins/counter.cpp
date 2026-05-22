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
#include <string>

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
// callable, or a singleton.
Value g_storedCallback;
bool  g_haveStored = false;

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
            auto cb = praia::requireCallable(args, 0, "counter.setStored");
            // Replacing an existing stored callback releases the old
            // pin first — leaks the pin otherwise.
            if (g_haveStored) praia::unpinValue(g_storedCallback);
            g_storedCallback = args[0];
            g_haveStored = true;
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
            if (!g_haveStored)
                praia::error("counter.callStored: no callback stored");
            return praia::call(g_storedCallback.asCallable(), {});
        }));

    // counter.clearStored() — release the pin and drop the stash.
    // Idempotent so test teardown can call it unconditionally.
    module->entries["clearStored"] = Value(makeNative("counter.clearStored", 0,
        [](const std::vector<Value>&) -> Value {
            if (g_haveStored) {
                praia::unpinValue(g_storedCallback);
                g_storedCallback = Value();
                g_haveStored = false;
            }
            return Value();
        }));
}
