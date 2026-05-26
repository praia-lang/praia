// Implementation of the C facade declared in praia_plugin_c.h.
// Each function is a thin thunk over the existing C++ ABI in
// praia_plugin.h / praia_runtime.h. The header is the canonical
// API contract; cross-reference there for lifetime semantics.

// fiber.h transitively pulls in <ucontext.h>, which on glibc/clang
// requires _XOPEN_SOURCE to be defined before include (the macros
// are marked deprecated otherwise and clang errors out). The engine
// Makefile sets this for its own translation units; declare it
// here too so the file's standalone analysis (LSP, ad-hoc builds)
// also compiles cleanly.
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "praia_plugin_c.h"
#include "praia_plugin.h"
#include "praia_runtime.h"

#include <string>
#include <vector>
#include <cstring>

// Every PraiaValue handed across the C boundary is a `new Value(...)`
// on the heap. PraiaValue == Value*. Casts are encapsulated in the
// helpers below so the boxing convention is in one spot.
static inline Value*       unbox(PraiaValue v) { return reinterpret_cast<Value*>(v); }
static inline PraiaValue   box(Value* v)       { return reinterpret_cast<PraiaValue>(v); }

namespace {

// Thread-local error message staged by praia_throw, consumed by
// the next return-NULL-from-native check. A second `set` flag
// distinguishes "no error" from "empty string error" (rare but
// the C user might pass ""), and lets the thunk tell whether the
// native bailed via praia_throw or just returned NULL silently.
thread_local std::string g_last_error;
thread_local bool        g_last_error_set = false;

void clearLastError() {
    g_last_error.clear();
    g_last_error_set = false;
}

std::string takeLastError() {
    std::string out = std::move(g_last_error);
    g_last_error.clear();
    bool was_set = g_last_error_set;
    g_last_error_set = false;
    if (!was_set && out.empty()) {
        return "<C native returned NULL without calling praia_throw>";
    }
    return out;
}

// PraiaArgs is opaque to C; the implementation holds borrowed Value
// pointers (one per actual arg, pre-boxed) so praia_args_get can
// hand back the same pointer on repeated calls. Boxes are freed
// when the args object goes out of scope at the end of the thunk.
//
// The const_cast below is a deliberate C-ABI boundary cast, not a
// hack. The C facade's public `PraiaValue` type is
// `praia_value_s*` — non-const by definition, because pure C has
// no clean way to express "borrowed but read-only" without
// inventing a parallel `const_PraiaValue` typedef and forcing
// every accessor to come in matched const/non-const pairs (which
// would double the API surface for no real safety win — the
// borrowed-vs-owned contract is documented at the API level and
// the bridge in praia_make_native already enforces "don't return
// a borrowed arg" before any delete can corrupt these addresses).
// Treat the cast as the explicit translation point between C++'s
// const-correctness and C's pointer-only type system.
struct ArgsImpl {
    std::vector<Value*> boxes;
    explicit ArgsImpl(const std::vector<Value>& args) {
        boxes.reserve(args.size());
        for (const auto& v : args) {
            boxes.push_back(const_cast<Value*>(&v));
        }
    }
};

}  // namespace

// Letting a C++ exception escape an `extern "C"` boundary is
// undefined behavior — the C caller has no unwind tables, and the
// runtime tends to terminate (or worse). Every thunk that can
// throw (allocations via `new`/`gcNew`, container ops that may
// rehash, variant accessors like `asInt()` that throw
// `std::bad_variant_access` if the value isn't an int) is wrapped
// in `try { ... } PRAIA_FACADE_CATCH(sentinel)`. The catch stages
// the exception's message in the thread-local last-error slot so
// the next native return surfaces it as a Praia RuntimeError, and
// returns a sentinel of the function's natural return type.
//
// Predicates and trivial accessors (praia_value_is_*,
// praia_args_count, praia_args_get) skip the wrapper — they only
// read a variant discriminator or a vector position and don't
// allocate, so they can't throw.
#define PRAIA_FACADE_CATCH(ret_on_fail)                                  \
    catch (const RuntimeError& e) {                                      \
        g_last_error = e.what(); g_last_error_set = true;                \
        return ret_on_fail;                                              \
    } catch (const std::exception& e) {                                  \
        g_last_error = e.what(); g_last_error_set = true;                \
        return ret_on_fail;                                              \
    } catch (...) {                                                      \
        g_last_error = "<unknown C++ exception in plugin facade>";       \
        g_last_error_set = true;                                         \
        return ret_on_fail;                                              \
    }

#define PRAIA_FACADE_CATCH_VOID                                          \
    catch (const RuntimeError& e) {                                      \
        g_last_error = e.what(); g_last_error_set = true;                \
    } catch (const std::exception& e) {                                  \
        g_last_error = e.what(); g_last_error_set = true;                \
    } catch (...) {                                                      \
        g_last_error = "<unknown C++ exception in plugin facade>";       \
        g_last_error_set = true;                                         \
    }

extern "C" {

// ─── Value lifecycle ─────────────────────────────────────────────

void praia_value_release(PraiaValue v) {
    try {
        if (!v) return;
        delete unbox(v);
    } PRAIA_FACADE_CATCH_VOID
}

PraiaValue praia_value_clone(PraiaValue v) {
    try {
        if (!v) return nullptr;
        return box(new Value(*unbox(v)));
    } PRAIA_FACADE_CATCH(nullptr)
}

// ─── Constructors ────────────────────────────────────────────────

PraiaValue praia_value_nil(void)        { try { return box(new Value());  } PRAIA_FACADE_CATCH(nullptr) }
PraiaValue praia_value_bool(bool b)     { try { return box(new Value(b)); } PRAIA_FACADE_CATCH(nullptr) }
PraiaValue praia_value_int(int64_t i)   { try { return box(new Value(i)); } PRAIA_FACADE_CATCH(nullptr) }
PraiaValue praia_value_double(double d) { try { return box(new Value(d)); } PRAIA_FACADE_CATCH(nullptr) }
PraiaValue praia_value_string(const char* s) {
    try { return box(new Value(std::string(s ? s : ""))); } PRAIA_FACADE_CATCH(nullptr)
}
PraiaValue praia_value_string_n(const char* s, size_t n) {
    try { return box(new Value(std::string(s ? s : "", s ? n : 0))); } PRAIA_FACADE_CATCH(nullptr)
}
PraiaValue praia_value_new_map(void) {
    try { return box(new Value(gcNew<PraiaMap>())); } PRAIA_FACADE_CATCH(nullptr)
}
PraiaValue praia_value_new_array(void) {
    try { return box(new Value(gcNew<PraiaArray>())); } PRAIA_FACADE_CATCH(nullptr)
}

// ─── Predicates ──────────────────────────────────────────────────

bool praia_value_is_nil(PraiaValue v)      { return v && unbox(v)->isNil(); }
bool praia_value_is_bool(PraiaValue v)     { return v && unbox(v)->isBool(); }
bool praia_value_is_int(PraiaValue v)      { return v && unbox(v)->isInt(); }
bool praia_value_is_double(PraiaValue v)   { return v && unbox(v)->isDouble(); }
bool praia_value_is_number(PraiaValue v)   { return v && unbox(v)->isNumber(); }
bool praia_value_is_string(PraiaValue v)   { return v && unbox(v)->isString(); }
bool praia_value_is_map(PraiaValue v)      { return v && unbox(v)->isMap(); }
bool praia_value_is_array(PraiaValue v)    { return v && unbox(v)->isArray(); }
bool praia_value_is_callable(PraiaValue v) { return v && unbox(v)->isCallable(); }
bool praia_value_is_external(PraiaValue v) { return v && unbox(v)->isExternal(); }

// ─── Accessors ───────────────────────────────────────────────────
//
// The variant `as*` calls below throw `std::bad_variant_access` if
// the held type doesn't match — wrap so a misbehaving plugin
// returning `praia_value_as_int(v)` on a non-int value gets a
// clean error rather than UB across the C ABI.

bool praia_value_as_bool(PraiaValue v) {
    try { return v && unbox(v)->asBool(); } PRAIA_FACADE_CATCH(false)
}
int64_t praia_value_as_int(PraiaValue v) {
    try { return v ? unbox(v)->asInt() : 0; } PRAIA_FACADE_CATCH(0)
}
double praia_value_as_double(PraiaValue v) {
    try { return v ? unbox(v)->asNumber() : 0.0; } PRAIA_FACADE_CATCH(0.0)
}

const char* praia_value_as_string(PraiaValue v, size_t* out_len) {
    try {
        if (!v || !unbox(v)->isString()) {
            if (out_len) *out_len = 0;
            return nullptr;
        }
        const std::string& s = unbox(v)->asString();
        if (out_len) *out_len = s.size();
        return s.data();
    } PRAIA_FACADE_CATCH(nullptr)
}

// ─── Map ops ─────────────────────────────────────────────────────

int praia_value_map_set(PraiaValue mv, PraiaValue kv, PraiaValue vv) {
    try {
        if (!mv || !kv || !vv) {
            praia_throw("praia_value_map_set: NULL argument");
            return -1;
        }
        if (!unbox(mv)->isMap()) {
            praia_throw("praia_value_map_set: target is not a map");
            return -1;
        }
        // Praia maps require hashable keys (nil/bool/int/float/string).
        // The variant-side ValueHash throws bad_variant_access on
        // unhashable types and the macro would catch that — but the
        // resulting message wouldn't mention what actually went wrong.
        // Check up front so the plugin author sees a diagnostic that
        // names the constraint directly.
        const Value& key = *unbox(kv);
        if (!isHashable(key)) {
            praia_throw("praia_value_map_set: map keys must be hashable "
                        "(nil, bool, int, float, or string)");
            return -1;
        }
        unbox(mv)->asMap()->entries[key] = *unbox(vv);
        return 0;
    } PRAIA_FACADE_CATCH(-1)
}

PraiaValue praia_value_map_get(PraiaValue mv, PraiaValue kv) {
    try {
        if (!mv || !kv || !unbox(mv)->isMap()) return nullptr;
        auto& entries = unbox(mv)->asMap()->entries;
        auto it = entries.find(*unbox(kv));
        if (it == entries.end()) return nullptr;
        return box(new Value(it->second));
    } PRAIA_FACADE_CATCH(nullptr)
}

bool praia_value_map_has(PraiaValue mv, PraiaValue kv) {
    try {
        if (!mv || !kv || !unbox(mv)->isMap()) return false;
        auto& entries = unbox(mv)->asMap()->entries;
        return entries.find(*unbox(kv)) != entries.end();
    } PRAIA_FACADE_CATCH(false)
}

size_t praia_value_map_size(PraiaValue mv) {
    try {
        if (!mv || !unbox(mv)->isMap()) return 0;
        return unbox(mv)->asMap()->entries.size();
    } PRAIA_FACADE_CATCH(0)
}

PraiaValue praia_value_map_keys(PraiaValue mv) {
    try {
        if (!mv || !unbox(mv)->isMap()) return nullptr;
        auto arr = gcNew<PraiaArray>();
        for (const auto& kv : unbox(mv)->asMap()->entries) {
            arr->elements.push_back(kv.first);
        }
        return box(new Value(arr));
    } PRAIA_FACADE_CATCH(nullptr)
}

// ─── Array ops ───────────────────────────────────────────────────

void praia_value_array_push(PraiaValue av, PraiaValue vv) {
    try {
        if (!av || !vv || !unbox(av)->isArray()) return;
        unbox(av)->asArray()->elements.push_back(*unbox(vv));
    } PRAIA_FACADE_CATCH_VOID
}

PraiaValue praia_value_array_get(PraiaValue av, size_t i) {
    try {
        if (!av || !unbox(av)->isArray()) return nullptr;
        auto& es = unbox(av)->asArray()->elements;
        if (i >= es.size()) return nullptr;
        return box(new Value(es[i]));
    } PRAIA_FACADE_CATCH(nullptr)
}

int praia_value_array_set(PraiaValue av, size_t i, PraiaValue vv) {
    try {
        if (!av || !vv) {
            praia_throw("praia_value_array_set: NULL argument");
            return -1;
        }
        if (!unbox(av)->isArray()) {
            praia_throw("praia_value_array_set: target is not an array");
            return -1;
        }
        auto& es = unbox(av)->asArray()->elements;
        if (i >= es.size()) {
            praia_throw("praia_value_array_set: index out of range");
            return -1;
        }
        es[i] = *unbox(vv);
        return 0;
    } PRAIA_FACADE_CATCH(-1)
}

size_t praia_value_array_len(PraiaValue av) {
    try {
        if (!av || !unbox(av)->isArray()) return 0;
        return unbox(av)->asArray()->elements.size();
    } PRAIA_FACADE_CATCH(0)
}

// ─── Module registration ─────────────────────────────────────────

void praia_module_set(PraiaMapHandle* module, const char* key, PraiaValue value) {
    try {
        if (!module || !key || !value) return;
        module->entries[Value(std::string(key))] = *unbox(value);
    } PRAIA_FACADE_CATCH_VOID
}

// ─── Native callbacks ────────────────────────────────────────────

PraiaValue praia_make_native(const char* name, int arity,
                             PraiaNativeFn fn, void* userdata) {
    try {
        if (!fn) return nullptr;
        std::string sname = name ? name : "<c_native>";
        auto cppFn = [fn, userdata, sname](const std::vector<Value>& args) -> Value {
            // Fresh thread-local error slate per call. A stale error
            // from a *previous* native (if the C user forgot to return
            // NULL after praia_throw) would otherwise hijack this
            // call's first NULL return.
            clearLastError();
            ArgsImpl ctx(args);
            PraiaValue raw = fn(reinterpret_cast<PraiaArgs>(&ctx), userdata);
            if (!raw) {
                // C side bailed. takeLastError() consumes the message;
                // RuntimeError(msg, 0) lets the engine fill in the
                // call-site line number.
                throw RuntimeError(takeLastError(), 0);
            }
            // Guard against the most catastrophic plugin bug we can
            // detect cheaply: returning a borrowed PraiaArgs entry
            // (i.e. `return praia_args_get(args, i)`) directly. The
            // boxes in ctx point at addresses on the engine's call
            // stack; deleting one corrupts the args vector and any
            // subsequent reads. Compare against each box address up
            // front and throw a clear error instead of UB. O(argc),
            // negligible for the typical 0-3 args.
            for (Value* argBox : ctx.boxes) {
                if (raw == reinterpret_cast<PraiaValue>(argBox)) {
                    throw RuntimeError(
                        "C native returned a borrowed argument Value "
                        "(use praia_value_clone before returning an arg)", 0);
                }
            }
            Value out = *unbox(raw);
            delete unbox(raw);   // ownership transfers to the engine
            return out;
        };
        return box(new Value(makeNative(sname, arity, std::move(cppFn))));
    } PRAIA_FACADE_CATCH(nullptr)
}

// Trivial — only reads a vector size. Can't throw.
int praia_args_count(PraiaArgs args) {
    if (!args) return 0;
    return static_cast<int>(reinterpret_cast<ArgsImpl*>(args)->boxes.size());
}

// Trivial — pointer math + bounds check. Can't throw.
PraiaValue praia_args_get(PraiaArgs args, int i) {
    if (!args) return nullptr;
    auto& boxes = reinterpret_cast<ArgsImpl*>(args)->boxes;
    if (i < 0 || static_cast<size_t>(i) >= boxes.size()) return nullptr;
    return box(boxes[static_cast<size_t>(i)]);
}

// ─── Error reporting ─────────────────────────────────────────────

// std::string assignment can throw std::bad_alloc on a very long
// message + low-memory condition. Absorb so the act of staging an
// error can never itself escape the C ABI as an exception.
void praia_throw(const char* msg) {
    try {
        g_last_error = msg ? msg : "";
        g_last_error_set = true;
    } PRAIA_FACADE_CATCH_VOID
}

// ─── External handles ────────────────────────────────────────────

PraiaValue praia_value_new_external(void* ptr, const char* type_name,
                                    PraiaDeleter deleter) {
    try {
        auto ext = gcNew<PraiaExternal>();
        ext->data = ptr;
        ext->typeName = type_name ? type_name : "";
        if (deleter) {
            // Wrap the C deleter in the std::function the PraiaExternal
            // destructor invokes during a GC sweep.
            ext->deleter = [deleter](void* p) { deleter(p); };
        }
        return box(new Value(ext));
    } PRAIA_FACADE_CATCH(nullptr)
}

void* praia_value_external_ptr(PraiaValue v, const char* expected_type_name) {
    try {
        if (!v || !unbox(v)->isExternal()) {
            praia_throw("praia_value_external_ptr: not an external handle");
            return nullptr;
        }
        auto ext = unbox(v)->asExternal();
        if (expected_type_name && ext->typeName != expected_type_name) {
            // Type-confusion guard: a plugin that receives a Value
            // claiming to be its handle but actually wrapping someone
            // else's pointer would crash on first dereference. Refuse
            // here so the error stays inside the plugin boundary.
            std::string msg = "praia_value_external_ptr: type mismatch (expected '";
            msg += expected_type_name;
            msg += "', got '";
            msg += ext->typeName;
            msg += "')";
            praia_throw(msg.c_str());
            return nullptr;
        }
        return ext->data;
    } PRAIA_FACADE_CATCH(nullptr)
}

const char* praia_value_external_type(PraiaValue v) {
    try {
        if (!v || !unbox(v)->isExternal()) return nullptr;
        return unbox(v)->asExternal()->typeName.c_str();
    } PRAIA_FACADE_CATCH(nullptr)
}

// ─── GC pin/unpin ────────────────────────────────────────────────

int praia_pin_value(PraiaValue v) {
    try {
        if (!v) return -1;
        praia::pinValue(*unbox(v));
        return 0;
    } PRAIA_FACADE_CATCH(-1)
}

int praia_unpin_value(PraiaValue v) {
    try {
        if (!v) return -1;
        praia::unpinValue(*unbox(v));
        return 0;
    } PRAIA_FACADE_CATCH(-1)
}

// ─── Calling back into Praia ─────────────────────────────────────

PraiaValue praia_call(PraiaValue callable, const PraiaValue* args, size_t argc) {
    try {
        if (!callable || !unbox(callable)->isCallable()) {
            praia_throw("praia_call: target is not callable");
            return nullptr;
        }
        std::vector<Value> cppArgs;
        cppArgs.reserve(argc);
        for (size_t i = 0; i < argc; ++i) {
            if (!args || !args[i]) cppArgs.emplace_back();
            else                   cppArgs.push_back(*unbox(args[i]));
        }
        Value result = praia::call(unbox(callable)->asCallable(), cppArgs);
        return box(new Value(std::move(result)));
    } PRAIA_FACADE_CATCH(nullptr)
}

// ─── Cross-thread helpers ────────────────────────────────────────

int praia_post_to_engine(void (*fn)(void* userdata), void* userdata) {
    try {
        if (!fn) {
            praia_throw("praia_post_to_engine: NULL function pointer");
            return -1;
        }
        void* exec = praia::currentExecutor();
        if (!exec) {
            // Calling from a thread with no executor is a programming
            // error — the post needs an engine to schedule against.
            // Stage an error AND surface it via the return code so a
            // worker thread (which has no native-return point to drain
            // the lastError slot) can see the failure.
            praia_throw("praia_post_to_engine: no executor on this thread "
                        "(must be called from engine thread)");
            return -1;
        }
        // Wrap the C function pointer + userdata as a zero-arity
        // Praia callable. Name "" is fine for tracebacks; this
        // callable is never user-visible.
        auto callable = makeNative("", 0,
            [fn, userdata](const std::vector<Value>&) -> Value {
                fn(userdata);
                return Value();
            });
        praia::postToEngine(exec, callable, {});
        return 0;
    } PRAIA_FACADE_CATCH(-1)
}

int praia_should_cancel(void) {
    try {
        auto opt = praia::shouldCancel();
        if (!opt.has_value()) return -1;
        return *opt ? 1 : 0;
    } PRAIA_FACADE_CATCH(-1)
}

// ─── Promise ─────────────────────────────────────────────────────

PraiaPromise praia_promise_new(void) {
    try {
        return reinterpret_cast<PraiaPromise>(new praia::Promise());
    } PRAIA_FACADE_CATCH(nullptr)
}

PraiaValue praia_promise_future(PraiaPromise p) {
    try {
        if (!p) return nullptr;
        return box(new Value(reinterpret_cast<praia::Promise*>(p)->future()));
    } PRAIA_FACADE_CATCH(nullptr)
}

void praia_promise_resolve(PraiaPromise p, PraiaValue result) {
    try {
        if (!p) return;
        Value v = result ? *unbox(result) : Value();
        reinterpret_cast<praia::Promise*>(p)->resolve(std::move(v));
    } PRAIA_FACADE_CATCH_VOID
}

void praia_promise_reject(PraiaPromise p, const char* message) {
    try {
        if (!p) return;
        reinterpret_cast<praia::Promise*>(p)->reject(message ? message : "");
    } PRAIA_FACADE_CATCH_VOID
}

void praia_promise_release(PraiaPromise p) {
    try {
        if (!p) return;
        delete reinterpret_cast<praia::Promise*>(p);
    } PRAIA_FACADE_CATCH_VOID
}

}  // extern "C"

#undef PRAIA_FACADE_CATCH
#undef PRAIA_FACADE_CATCH_VOID
