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
}
