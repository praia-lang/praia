// Praia Native Plugin API
//
// Include this single header in your plugin source file.
// Every plugin must:
//   1. Declare its ABI version via PRAIA_DECLARE_ABI()
//   2. Export `extern "C" void praia_register(PraiaMap* module)`
//
// Example:
//
//   #include "praia_plugin.h"
//
//   PRAIA_DECLARE_ABI();
//
//   extern "C" void praia_register(PraiaMap* module) {
//       module->entries["double"] = Value(makeNative("mymod.double", 1,
//           [](const std::vector<Value>& args) -> Value {
//               double x = praia::requireNumber(args, 0, "mymod.double");
//               return Value(x * 2);
//           }));
//   }
//
// Build:
//   make plugin SRC=myplugin.cpp OUT=myplugin.dylib   # macOS
//   make plugin SRC=myplugin.cpp OUT=myplugin.so      # Linux
// (or use the `praia --include-path` manual snippet from PLUGINS.md
//  if you're not building from a Praia source checkout)
//
// Use in Praia:
//   let mymod = loadNative("./myplugin")  // extension auto-detected
//   print(mymod.double(21))  // 42

#pragma once

#include "value.h"          // Value, PraiaArray, PraiaMap, RuntimeError
#include "gc_heap.h"        // gcNew<T>()
#include "builtins.h"       // makeNative()
#include "praia_runtime.h"  // praia::currentExecutor, praia::invokeExecutor
#include <cstdio>           // std::fprintf in PinnedValue cross-thread abort
#include <cstdlib>          // std::abort  in PinnedValue cross-thread abort

// ── Plugin ABI version ─────────────────────────────────────────
//
// Bumped whenever the C++ surface this header re-exports changes
// in a way that would silently misbehave for plugins built against
// the old surface (Value variant layout, PraiaMap key type, the
// praia_runtime.h ABI). Plugins MUST invoke PRAIA_DECLARE_ABI()
// once at file scope so loadNative() can verify the plugin was
// built against this praia version before invoking
// praia_register. A mismatch (or a missing declaration) refuses
// the load with a clear "rebuild required" message rather than
// crashing later from a layout mismatch.
//
// History:
//   1 — initial versioned ABI (Praia 0.4+)
//   2 — adds praia::postToEngine for cross-thread call marshalling
//       (praia_runtime.h). Plugins that link the new symbol won't
//       resolve against a v1 engine; bumping the ABI surfaces that
//       as a clean "rebuild required" diagnostic at loadNative time
//       rather than a raw dlerror about an unresolved symbol.
#define PRAIA_PLUGIN_ABI_VERSION 2

#define PRAIA_DECLARE_ABI()                                             \
    extern "C" int praia_abi_version() {                                \
        return PRAIA_PLUGIN_ABI_VERSION;                                \
    }

// ── Optional plugin metadata ────────────────────────────────────
//
// Declares name/version/description as exported C symbols
// loadNative() reads at load time. When present they surface as a
// `_meta` sub-key on the returned module map, so user code can do:
//
//   let mod = loadNative("./mymod")
//   print(mod._meta.version)
//
// Entirely optional — plugins that don't invoke this macro get no
// `_meta` key on the module. Pairs with the ABI version system
// above for diagnostic surfacing in `sand list` and similar tools.
//
// Place once at file scope alongside PRAIA_DECLARE_ABI():
//
//   PRAIA_PLUGIN_METADATA("mymod", "1.2.0", "Does the thing");
#define PRAIA_PLUGIN_METADATA(NAME, VERSION, DESCRIPTION)                    \
    extern "C" const char* praia_plugin_name()        { return NAME; }       \
    extern "C" const char* praia_plugin_version()     { return VERSION; }    \
    extern "C" const char* praia_plugin_description() { return DESCRIPTION; }

namespace praia {

// ── Argument validation helpers ─────────────────────────────────
//
// Compress the "check type, throw RuntimeError" boilerplate that
// every native function would otherwise repeat. Each `require*`
// throws RuntimeError on type/arity mismatch and returns the
// unwrapped value on success, so a plugin can write:
//
//   auto& s = praia::requireString(args, 0, "mymod.foo");
//   int64_t n = praia::requireInt(args, 1, "mymod.foo");
//
// Error wording matches the canonical pattern used by the engine's
// own builtins ("<funcname>() argument N must be a <type>") so the
// surface is consistent whether a call fails inside a plugin or a
// stdlib native. Line is left as 0 — the engine overwrites it with
// the call-site line in the user's Praia code.

// Throw a RuntimeError with `msg`. The engine fills in the
// call-site line; plugins should never try to compute one.
[[noreturn]] inline void error(const std::string& msg) {
    throw RuntimeError(msg, 0);
}

inline void requireArity(const std::vector<Value>& args, int n,
                         const std::string& fn) {
    int got = static_cast<int>(args.size());
    if (got != n) {
        error(fn + "() expected " + std::to_string(n) +
              " argument(s) but got " + std::to_string(got));
    }
}

inline void requireArityRange(const std::vector<Value>& args,
                              int lo, int hi,
                              const std::string& fn) {
    int got = static_cast<int>(args.size());
    if (got < lo || got > hi) {
        error(fn + "() expected " + std::to_string(lo) + "-" +
              std::to_string(hi) + " arguments but got " +
              std::to_string(got));
    }
}

namespace detail {
[[noreturn]] inline void argTypeError(const std::string& fn, size_t i,
                                      const char* type) {
    error(fn + "() argument " + std::to_string(i + 1) +
          " must be a " + type);
}
}

inline const std::string& requireString(const std::vector<Value>& args,
                                        size_t i, const std::string& fn) {
    if (i >= args.size() || !args[i].isString())
        detail::argTypeError(fn, i, "string");
    return args[i].asString();
}

// Strict int — rejects floats. Use requireNumber when either is OK.
inline int64_t requireInt(const std::vector<Value>& args,
                          size_t i, const std::string& fn) {
    if (i >= args.size() || !args[i].isInt())
        detail::argTypeError(fn, i, "int");
    return args[i].asInt();
}

// Accepts int OR float and returns a double (converts int).
inline double requireNumber(const std::vector<Value>& args,
                            size_t i, const std::string& fn) {
    if (i >= args.size() || !args[i].isNumber())
        detail::argTypeError(fn, i, "number");
    return args[i].asNumber();
}

inline bool requireBool(const std::vector<Value>& args,
                        size_t i, const std::string& fn) {
    if (i >= args.size() || !args[i].isBool())
        detail::argTypeError(fn, i, "bool");
    return args[i].asBool();
}

inline std::shared_ptr<PraiaArray> requireArray(
        const std::vector<Value>& args, size_t i, const std::string& fn) {
    if (i >= args.size() || !args[i].isArray())
        detail::argTypeError(fn, i, "array");
    return args[i].asArray();
}

inline std::shared_ptr<PraiaMap> requireMap(
        const std::vector<Value>& args, size_t i, const std::string& fn) {
    if (i >= args.size() || !args[i].isMap())
        detail::argTypeError(fn, i, "map");
    return args[i].asMap();
}

inline std::shared_ptr<Callable> requireCallable(
        const std::vector<Value>& args, size_t i, const std::string& fn) {
    if (i >= args.size() || !args[i].isCallable())
        detail::argTypeError(fn, i, "function");
    return args[i].asCallable();
}

// ── Callback into Praia code ────────────────────────────────────

// Invoke a Praia Callable received as a plugin argument. Use this
// to call back into user code (e.g. when the plugin accepts a
// lambda for a filter/sort/iterator-style API).
//
//   module->entries["forEach"] = Value(makeNative("mymod.forEach", 2,
//       [](const std::vector<Value>& args) -> Value {
//           auto& arr = args[0].asArray()->elements;
//           auto fn   = args[1].asCallable();
//           for (auto& v : arr) praia::call(fn, {v});
//           return Value();
//       }));
//
// Pitfall — deferred invocation. Both engines stash their current
// pointer in a thread-local that's only valid for the duration of
// the active call frame. If a plugin stashes a Callable and
// invokes it later from a worker thread or libuv-style callback,
// `praia::call` throws because no engine is bound to that thread.
// Callbacks must be invoked synchronously from the same plugin
// thread that received them — or marshalled back via
// `praia::postToEngine` (below), which is the supported path for
// any std::thread / libuv / async-I/O scenario.
inline Value call(const std::shared_ptr<Callable>& fn,
                  const std::vector<Value>& args = {}) {
    void* exec = currentExecutor();
    if (!exec) {
        // Distinct from invokeExecutor's "null executor token" so a
        // stack trace makes clear that the wrapper detected the
        // missing context (the common case), not a caller that
        // bypassed the wrapper and passed nullptr to invokeExecutor.
        throw RuntimeError(
            "praia::call invoked with no active Praia executor on this thread", 0);
    }
    return invokeExecutor(exec, fn, args);
}

// Marshal a Praia callback back to the engine thread —
// praia::postToEngine — is declared in praia_runtime.h next to
// currentExecutor() / invokeExecutor() because the implementation
// is out-of-line (no thread-local fast-path to inline). See the
// declaration for the full contract; the canonical pattern is:
//
//   module->entries["scheduleAfter"] = Value(makeNative("mod.scheduleAfter", 2,
//       [](const std::vector<Value>& args) -> Value {
//           int ms = (int)praia::requireNumber(args, 0, "mod.scheduleAfter");
//           auto cb = praia::requireCallable(args, 1, "mod.scheduleAfter");
//           // Pin the callback so the engine thread's GC doesn't
//           // collect its closure between now and the post firing.
//           praia::pinValue(args[1]);
//           void* engine = praia::currentExecutor();   // capture here
//           std::thread([engine, cb, ms]() {
//               std::this_thread::sleep_for(std::chrono::milliseconds(ms));
//               // Different thread — no executor of its own. The post
//               // crosses the boundary. The pin keeps `cb` alive
//               // until the engine drains.
//               praia::postToEngine(engine, cb, {});
//               // The matching unpin must run on the engine thread —
//               // typically the callback itself does it at the end.
//           }).detach();
//           return Value();
//       }));
//
// Pitfalls:
//   * Worker threads MUST NOT call praia::pinValue / unpinValue —
//     those throw on a thread with no active executor. Pin BEFORE
//     spawning the worker; have the callback unpin itself when it
//     runs on the engine thread.
//   * postToEngine is fire-and-forget: no return value, no exception
//     propagation. The drain loop logs callback exceptions to stderr
//     and continues; a bad callback can't poison the queue.

// ── Opaque handles ──────────────────────────────────────────────
//
// Wrap a C/C++ pointer as a Praia Value with a destructor that
// fires when the last reference drops (during a GC sweep). Use
// this for DB connections, file handles, OS resources — anything
// with non-Praia state whose lifetime should track the Value's.
//
// `typeName` is a stable identifier the plugin uses to recognize
// its own handles on the unwrap side. Convention: "module.type"
// (e.g. "sqlite.connection", "myplugin.session"). The string is
// stored verbatim and `getExternal<T>` compares for exact match.
//
// Lifetime ownership transfers to the resulting Value: don't
// `delete ptr` yourself; the registered deleter does it. Pass
// nullptr for `deleter` if `ptr` is statically allocated or
// otherwise externally managed.
//
// Example (DB connection):
//
//   module->entries["open"] = Value(makeNative("db.open", 1,
//       [](const std::vector<Value>& args) -> Value {
//           sqlite3* conn = nullptr;
//           sqlite3_open(praia::requireString(args, 0, "db.open").c_str(), &conn);
//           return praia::makeExternal<sqlite3>(conn, "sqlite3.connection",
//                                                [](sqlite3* c) { sqlite3_close(c); });
//       }));
//   module->entries["query"] = Value(makeNative("db.query", 2,
//       [](const std::vector<Value>& args) -> Value {
//           auto* conn = praia::getExternal<sqlite3>(args[0], "sqlite3.connection");
//           // ... use conn ...
//       }));
template<typename T>
inline Value makeExternal(T* ptr, const char* typeName,
                          void (*deleter)(T*)) {
    auto ext = gcNew<PraiaExternal>();
    ext->data = static_cast<void*>(ptr);
    ext->typeName = typeName ? typeName : "";
    if (deleter) {
        ext->deleter = [deleter](void* p) {
            deleter(static_cast<T*>(p));
        };
    }
    return Value(ext);
}

// Unwrap an external Value back to its T*. Throws RuntimeError
// if the Value isn't an external, or if its typeName doesn't
// match `typeName` — guards against type confusion when a plugin
// receives a Value from user code claiming to be a handle but
// isn't, or is a handle from a different plugin/type.
template<typename T>
inline T* getExternal(const Value& v, const char* typeName) {
    const char* expected = typeName ? typeName : "";
    if (!v.isExternal())
        error(std::string("expected an external handle of type '") +
              expected + "'");
    auto ext = v.asExternal();
    if (ext->typeName != expected)
        error(std::string("external handle type mismatch: expected '") +
              expected + "' but got '" + ext->typeName + "'");
    return static_cast<T*>(ext->data);
}

// ── GC root pinning ────────────────────────────────────────────
//
// A native plugin that stashes a Praia Value in C++ static / global
// storage between calls (callback registries, caches, singletons)
// MUST pin it for the lifetime it's reachable from C++. Without a
// pin, the next garbage collection treats the value as unreachable
// (it's not in the interpreter's roots) and *clears the underlying
// object's internal references during the sweep*. The shared_ptr
// you held stays valid, but the object is hollow — calling a
// stashed Callable finds an empty Environment, reading a cached
// PraiaMap finds its entries cleared, etc.
//
// `pinValue` / `unpinValue` are reference-counted by multiplicity:
// pin N times → unpin N times to actually release. Last-in-first-
// out matching keeps nested RAII pin/unpin pairs unambiguous.
//
// Non-GC-tracked values (numbers, strings, bools, nil) accept pins
// harmlessly — they have nothing for the sweep to clear, so the
// pin is a no-op on the mark side.
//
// Example: a callback table.
//
//   static std::unordered_map<std::string, Value> g_callbacks;
//
//   module->entries["on"] = Value(makeNative("mod.on", 2,
//       [](const std::vector<Value>& args) -> Value {
//           auto& name = praia::requireString(args, 0, "mod.on");
//           auto  cb   = praia::requireCallable(args, 1, "mod.on");
//           auto it = g_callbacks.find(name);
//           if (it != g_callbacks.end())
//               praia::unpinValue(it->second);   // release old
//           praia::pinValue(args[1]);            // hold new
//           g_callbacks[name] = args[1];
//           return Value();
//       }));
//
// For short-lived holds (inside a single native call), use the
// PinnedValue scope guard below instead.
inline void pinValue(const Value& v) {
    // Pinning on a thread without an active interpreter would silently
    // register the value in an orphan thread-local GcHeap that nothing
    // ever marks — the pin would do nothing, and the user would have no
    // hint that they're mis-wired. Catch it loudly. Matches the same
    // check in praia::call for callable invocation.
    if (!currentExecutor()) {
        throw RuntimeError(
            "praia::pinValue invoked with no active Praia executor on this thread", 0);
    }
    GcHeap::current().pinValue(v);
}

inline void unpinValue(const Value& v) {
    if (!currentExecutor()) {
        throw RuntimeError(
            "praia::unpinValue invoked with no active Praia executor on this thread", 0);
    }
    GcHeap::current().unpinValue(v);
}

// RAII scope guard: pin on construction, unpin on destruction.
// Move-only — copy would double-unpin. Lifetime is whatever C++
// scope holds the guard; storing it in a static container extends
// the pin to the container's lifetime (useful for "pin forever"
// patterns like an init-time callback registration that never
// goes back through Praia).
//
// Thread binding: each GcHeap is thread-local. The guard records
// the heap it pinned against at construction and unpins against
// the SAME heap on destruction. That way moving the guard
// (intentionally or via container reallocation) inside one thread
// keeps working, but using a guard cross-thread fails loudly:
// the constructor throws if there's no active executor, and
// move-assign carries the bound heap across so the eventual
// unpin lands in the right registry.
//
// Example:
//
//   {
//       praia::PinnedValue pin(someValue);
//       // someValue protected from GC while `pin` is in scope.
//       expensiveOperation(someValue);
//   }   // unpinned here on the same heap that pinned.
class PinnedValue {
public:
    explicit PinnedValue(const Value& v)
        : v_(v), heap_(nullptr), pinned_(false) {
        // Refuse to register against an orphan heap (no executor =>
        // this thread isn't running Praia code; any pin would just
        // leak into a GcHeap nothing ever sweeps).
        if (!currentExecutor()) {
            throw RuntimeError(
                "praia::PinnedValue constructed with no active Praia executor on this thread", 0);
        }
        heap_ = &GcHeap::current();
        heap_->pinValue(v_);
        pinned_ = true;
    }

    ~PinnedValue() {
        // Release against the heap we pinned to — but only if we're
        // still on that thread. Each `GcHeap` is thread-local, so a
        // destruct on a different thread would either race on the
        // origin thread's pinned_ vector (if that thread is alive) or
        // dereference a dead thread's storage. Both are UB. Abort
        // loudly instead — cross-thread destruction is always a
        // plugin bug, and a clean process death is strictly better
        // than the silent corruption the alternative invites. We
        // can't throw from a destructor.
        if (pinned_ && heap_) releaseOrAbort();
    }

    PinnedValue(PinnedValue&& other) noexcept
        : v_(std::move(other.v_)), heap_(other.heap_), pinned_(other.pinned_) {
        other.heap_   = nullptr;
        other.pinned_ = false;
    }
    PinnedValue& operator=(PinnedValue&& other) noexcept {
        if (this != &other) {
            // Same cross-thread concern as the destructor. Move-assign
            // is noexcept (vector resize requires it), so abort
            // rather than throw on a mismatch.
            if (pinned_ && heap_) releaseOrAbort();
            v_      = std::move(other.v_);
            heap_   = other.heap_;
            pinned_ = other.pinned_;
            other.heap_   = nullptr;
            other.pinned_ = false;
        }
        return *this;
    }
    PinnedValue(const PinnedValue&) = delete;
    PinnedValue& operator=(const PinnedValue&) = delete;

    const Value& get() const { return v_; }

private:
    // Release this guard's pin against the heap it was pinned on.
    // Aborts if we're not on that heap's thread — caller must already
    // have verified `pinned_ && heap_` before invoking. Keeping the
    // abort path out-of-line shrinks the destructor/move-assign
    // bodies and lets the message live in one place.
    void releaseOrAbort() noexcept {
        if (heap_ != &GcHeap::current()) {
            std::fprintf(stderr,
                "praia::PinnedValue: cross-thread teardown detected. "
                "The guard was pinned on a different thread's GcHeap; "
                "releasing it from here would race on (or dereference) "
                "that heap. Pin/unpin must happen on the same thread.\n");
            std::abort();
        }
        heap_->unpinValue(v_);
    }

    Value    v_;
    GcHeap*  heap_;
    bool     pinned_;
};

}  // namespace praia
