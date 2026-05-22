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
#define PRAIA_PLUGIN_ABI_VERSION 1

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
// thread that received them, or the plugin must marshal back to
// the engine thread first.
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
    GcHeap::current().pinValue(v);
}

inline void unpinValue(const Value& v) {
    GcHeap::current().unpinValue(v);
}

// RAII scope guard: pin on construction, unpin on destruction.
// Move-only — copy would double-unpin. Lifetime is whatever C++
// scope holds the guard; storing it in a static container extends
// the pin to the container's lifetime (useful for "pin forever"
// patterns like an init-time callback registration that never
// goes back through Praia).
//
// Example:
//
//   {
//       praia::PinnedValue pin(someValue);
//       // someValue protected from GC while `pin` is in scope.
//       expensiveOperation(someValue);
//   }   // unpinned here.
class PinnedValue {
public:
    explicit PinnedValue(const Value& v) : v_(v), pinned_(true) {
        pinValue(v_);
    }
    ~PinnedValue() { if (pinned_) unpinValue(v_); }

    PinnedValue(PinnedValue&& other) noexcept
        : v_(std::move(other.v_)), pinned_(other.pinned_) {
        other.pinned_ = false;
    }
    PinnedValue& operator=(PinnedValue&& other) noexcept {
        if (this != &other) {
            if (pinned_) unpinValue(v_);
            v_ = std::move(other.v_);
            pinned_ = other.pinned_;
            other.pinned_ = false;
        }
        return *this;
    }
    PinnedValue(const PinnedValue&) = delete;
    PinnedValue& operator=(const PinnedValue&) = delete;

    const Value& get() const { return v_; }

private:
    Value v_;
    bool  pinned_;
};

}  // namespace praia
