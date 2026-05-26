/*
 * praia_plugin_c.h — pure-C facade for the Praia plugin ABI.
 *
 * Use this header instead of praia_plugin.h when you want to:
 *   - write plugins in C rather than C++;
 *   - bind to Praia from Rust / Zig / Go / Swift / any language with
 *     a C FFI (the C++ ABI's name mangling and exception model make
 *     it impractical to call directly from non-C++ tooling).
 *
 * The facade is purely additive: it re-exports the engine's
 * existing C++ ABI through C-callable thunks. Same engine binary,
 * same ABI gate, same plugin entry point (`praia_register`),
 * same loadNative diagnostics.
 *
 *   Quick start:
 *     #include "praia_plugin_c.h"
 *
 *     PRAIA_C_DECLARE_ABI();
 *
 *     static PraiaValue cmod_hello(PraiaArgs args, void* ud) {
 *         (void) ud;
 *         if (praia_args_count(args) != 1) {
 *             praia_throw("cmod.hello: expected 1 argument");
 *             return NULL;
 *         }
 *         PraiaValue name = praia_args_get(args, 0);
 *         size_t n = 0;
 *         const char* s = praia_value_as_string(name, &n);
 *         char buf[256];
 *         snprintf(buf, sizeof(buf), "hello, %.*s", (int)n, s);
 *         return praia_value_string(buf);
 *     }
 *
 *     void praia_register(PraiaMapHandle* module) {
 *         PraiaValue fn = praia_make_native("cmod.hello", 1, cmod_hello, NULL);
 *         praia_module_set(module, "hello", fn);
 *         praia_value_release(fn);
 *     }
 *
 *   Build (macOS):
 *     gcc -std=c11 -shared -fPIC -I$(praia --include-path) \
 *         -undefined dynamic_lookup -o cmod.dylib cmod.c
 *
 *   Build (Linux):
 *     gcc -std=c11 -shared -fPIC -I$(praia --include-path) \
 *         -o cmod.so cmod.c
 *
 * Lifetime in one paragraph:
 *   Every PraiaValue you receive *from* the API (constructors,
 *   praia_value_map_get, praia_call, praia_promise_future) is YOURS
 *   to free with praia_value_release. PraiaValues passed *into* a
 *   setter or container op are copied — the engine takes its own
 *   reference, so you still own (and must release) the handle you
 *   passed in. PraiaValues handed to you via praia_args_get inside
 *   a native callback are BORROWED: do not release them; clone
 *   first if you need to outlive the call. Returning a PraiaValue
 *   from a native callback transfers ownership to the engine.
 */
#ifndef PRAIA_PLUGIN_C_H
#define PRAIA_PLUGIN_C_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── ABI version ──────────────────────────────────────────────────
 *
 * The C facade shares the C++ ABI's version counter. A plugin
 * built against praia_plugin_c.h v5 is binary-compatible with the
 * same engine that loads C++ plugins built against praia_plugin.h
 * v5. The engine's loadNative gate inspects `praia_abi_version()`
 * — same symbol both languages export — and refuses mismatches
 * with a "rebuild required" diagnostic.
 */
#define PRAIA_PLUGIN_ABI_VERSION 5

/* Required: place once at file scope. Without it, loadNative
 * refuses to load the plugin. */
#define PRAIA_C_DECLARE_ABI()                                           \
    int praia_abi_version(void) { return PRAIA_PLUGIN_ABI_VERSION; }

/* Optional: declare module name, version, and description. Exposed
 * to user code as `mod._meta = {name, version, description}` after
 * loadNative returns. */
#define PRAIA_C_PLUGIN_METADATA(NAME, VERSION, DESC)                    \
    const char* praia_plugin_name(void)        { return NAME; }         \
    const char* praia_plugin_version(void)     { return VERSION; }      \
    const char* praia_plugin_description(void) { return DESC; }

/* ── Opaque handles ───────────────────────────────────────────── */

/* A boxed Praia Value. NULL is a valid value only as a return-from-
 * native error signal — every other API returning PraiaValue
 * returns a real handle (or sets last error and returns NULL with
 * explicit documentation per function). */
typedef struct praia_value_s*   PraiaValue;

/* Inspector for native callback args. Borrowed from the engine
 * for the call's duration; do not stash. */
typedef struct praia_args_s*    PraiaArgs;

/* Plugin-constructed Future. Used by background-thread plugins
 * that want to resolve a Future from a worker. */
typedef struct praia_promise_s* PraiaPromise;

/* Engine's PraiaMap struct, threaded through `praia_register`. The
 * facade exposes a setter — plugin code never dereferences. */
typedef struct PraiaMap         PraiaMapHandle;

/* ── Plugin entry point ───────────────────────────────────────── */

/* Plugins implement this. Called once at loadNative time on the
 * engine thread. Register exports via praia_module_set below. */
extern void praia_register(PraiaMapHandle* module);

/* Optional teardown hook. *You* define this function (just like
 * praia_register); the engine calls it once at process exit, after
 * every async task has drained. Use it to release resources the
 * GC can't clean up on its own — open sockets, child processes,
 * library handles whose deleters block on I/O.
 *
 *   void praia_at_exit(void) {
 *       close_my_sockets();
 *       free_my_caches();
 *   }
 *
 * Optional — plugins without a teardown hook simply don't define
 * the symbol, and the engine skips it. Errors thrown from
 * praia_at_exit are logged to stderr and swallowed; a misbehaving
 * teardown can't crash the process during shutdown. */
extern void praia_at_exit(void);

/* ── Value lifecycle ──────────────────────────────────────────── */

/* Drop ownership of a PraiaValue. Safe on NULL. */
void       praia_value_release(PraiaValue v);

/* Get a new owned handle referring to the same underlying value.
 * For GC-tracked types (map/array/callable/external), this bumps
 * the shared refcount; the underlying object survives until every
 * handle is released AND the engine no longer holds it. */
PraiaValue praia_value_clone(PraiaValue v);

/* ── Value constructors ─────────────────────────────────────────
 *
 * Each returns a freshly owned PraiaValue. Caller must release.
 */
PraiaValue praia_value_nil(void);
PraiaValue praia_value_bool(bool b);
PraiaValue praia_value_int(int64_t i);
PraiaValue praia_value_double(double d);

/* NUL-terminated string. Bytes are copied. */
PraiaValue praia_value_string(const char* s);

/* Byte sequence with explicit length — for strings containing
 * embedded NULs or non-UTF8 binary. */
PraiaValue praia_value_string_n(const char* s, size_t len);

/* GC-tracked empty containers. */
PraiaValue praia_value_new_map(void);
PraiaValue praia_value_new_array(void);

/* ── Type predicates ──────────────────────────────────────────── */

bool praia_value_is_nil(PraiaValue v);
bool praia_value_is_bool(PraiaValue v);
bool praia_value_is_int(PraiaValue v);       /* exactly int (not float) */
bool praia_value_is_double(PraiaValue v);    /* exactly float */
bool praia_value_is_number(PraiaValue v);    /* int OR float */
bool praia_value_is_string(PraiaValue v);
bool praia_value_is_map(PraiaValue v);
bool praia_value_is_array(PraiaValue v);
bool praia_value_is_callable(PraiaValue v);
bool praia_value_is_external(PraiaValue v);

/* ── Value accessors ──────────────────────────────────────────── */

bool        praia_value_as_bool(PraiaValue v);

/* Safe only after praia_value_is_int. as_double is safe whenever
 * praia_value_is_number is true (auto-converts an int). */
int64_t     praia_value_as_int(PraiaValue v);
double      praia_value_as_double(PraiaValue v);

/* Returns a pointer to the string's bytes and (if out_len is
 * non-NULL) writes the byte count.
 *
 * Two contract points that catch real bugs:
 *
 *   (a) The pointer is only valid while the PraiaValue is alive.
 *       Calling praia_value_release(v) (or letting the engine drop
 *       its last reference) invalidates the pointer immediately —
 *       any later read is use-after-free.
 *
 *   (b) The bytes are NOT NUL-terminated. Praia strings are
 *       arbitrary byte sequences and may contain embedded NULs.
 *       Passing this pointer to strlen/strcpy/printf("%s")/etc. is
 *       wrong on both counts: they'd walk past the end of the
 *       string into adjacent Praia heap memory.
 *
 * Immediate use is fine — write your loop or comparison against
 * (s, *out_len), no copy needed:
 *
 *     size_t n = 0;
 *     const char* s = praia_value_as_string(v, &n);
 *     for (size_t i = 0; i < n; ++i) { ... }
 *
 * To outlive the PraiaValue OR to pass to a NUL-terminated API,
 * copy first:
 *
 *     size_t n = 0;
 *     const char* s = praia_value_as_string(v, &n);
 *     char* owned = malloc(n + 1);
 *     memcpy(owned, s, n);
 *     owned[n] = 0;
 *     // ... use `owned` freely, free(owned) when done.
 */
const char* praia_value_as_string(PraiaValue v, size_t* out_len);

/* ── Map ops ──────────────────────────────────────────────────── */

/* Insert. `key` and `value` are copied — caller still owns and
 * must release. Common idiom: praia_value_map_set(m, k=praia_value_string("foo"), v=…),
 * then release k and v immediately.
 *
 * Returns 0 on success, -1 on error (NULL args, map isn't a map,
 * or key isn't hashable — Praia maps only accept nil / bool /
 * int / float / string keys). On -1, the staged error message
 * names the cause; map is unchanged. Callers that want the error
 * to propagate as a Praia RuntimeError should return NULL after
 * a -1, the same way they would after a praia_throw. */
int        praia_value_map_set(PraiaValue map, PraiaValue key, PraiaValue value);

/* Lookup. Returns a new owned handle to the value, or NULL if the
 * key isn't present. Caller releases. */
PraiaValue praia_value_map_get(PraiaValue map, PraiaValue key);

bool       praia_value_map_has(PraiaValue map, PraiaValue key);
size_t     praia_value_map_size(PraiaValue map);

/* Returns a new owned array Value containing every key. Order is
 * implementation-defined (hash order). */
PraiaValue praia_value_map_keys(PraiaValue map);

/* ── Array ops ────────────────────────────────────────────────── */

void       praia_value_array_push(PraiaValue arr, PraiaValue v);

/* Returns a new owned handle, or NULL if i is out of range. */
PraiaValue praia_value_array_get(PraiaValue arr, size_t i);

/* Sets arr[i] = v. Returns 0 on success, -1 on error
 * (out-of-range i, NULL args, or arr isn't an array). On error
 * the staged error message names the cause; arr is unchanged.
 * Callers that want the error to propagate as a Praia
 * RuntimeError should return NULL after a -1, the same way they
 * would after a praia_throw. */
int        praia_value_array_set(PraiaValue arr, size_t i, PraiaValue v);

size_t     praia_value_array_len(PraiaValue arr);

/* ── Module registration ──────────────────────────────────────── */

/* Set module["key"] = value inside praia_register. Both the key
 * string and the value are copied — caller releases its handle. */
void praia_module_set(PraiaMapHandle* module, const char* key, PraiaValue value);

/* ── Native callbacks ─────────────────────────────────────────── */

/* Signature for a C native. Receives a borrowed PraiaArgs, returns
 * a freshly owned PraiaValue (engine takes ownership) — OR returns
 * NULL after calling praia_throw to signal an error. */
typedef PraiaValue (*PraiaNativeFn)(PraiaArgs args, void* userdata);

/* Register a native function. `arity` is the expected argc, or
 * -1 for variadic. The engine validates argc and throws if it
 * doesn't match before your callback runs.
 *
 * `userdata` is opaque; passed back unchanged on every invocation.
 * NULL is fine for stateless callbacks. */
PraiaValue praia_make_native(const char* name, int arity,
                             PraiaNativeFn fn, void* userdata);

/* Inspect args from within a native callback. */
int        praia_args_count(PraiaArgs args);

/* Returns a borrowed handle. Do NOT release. Clone if you need to
 * outlive the call. Returns NULL if i is out of range (the engine
 * would have caught this before calling you when arity != -1,
 * so this only matters for variadic natives). */
PraiaValue praia_args_get(PraiaArgs args, int i);

/* ── Error reporting ──────────────────────────────────────────── */

/* Stage an error message. The next NULL return from a native
 * callback (or other *_throw-aware op) translates this into a
 * RuntimeError on the engine side — same surface to user code as
 * if a C++ plugin had thrown.
 *
 * Calling praia_throw does NOT itself unwind. The function must
 * still return (NULL by convention). Stale messages are cleared
 * automatically between native calls so an earlier error can't
 * pollute a later one. */
void praia_throw(const char* msg);

/* ── External handles (opaque resources) ──────────────────────── */

typedef void (*PraiaDeleter)(void* ptr);

/* Wrap a C pointer as an opaque Praia value. `type_name` is a
 * stable identifier you use to recognize your own handles on the
 * unwrap side ("mymod.connection", "mymod.session"). `deleter` is
 * called exactly once when the last reference drops (during a GC
 * sweep); pass NULL if the pointer's lifetime is managed elsewhere
 * (static storage, leaked-by-design, etc.). */
PraiaValue praia_value_new_external(void* ptr, const char* type_name,
                                    PraiaDeleter deleter);

/* Unwrap. If the PraiaValue isn't an external of the expected
 * type, sets the last error and returns NULL — guards type
 * confusion when user code passes the wrong handle in. Pass NULL
 * for type_name to skip the type check. */
void* praia_value_external_ptr(PraiaValue v, const char* expected_type_name);

const char* praia_value_external_type(PraiaValue v);

/* ── GC root pinning ──────────────────────────────────────────── */

/* Pin a GC-tracked Value so the next garbage collection won't
 * clear its interior references while you're still using it. Use
 * when stashing a Value in C static storage between calls.
 *
 * Returns 0 on success, -1 if no Praia executor is bound to this
 * thread (which means you called from a worker thread — pin must
 * always run on the engine thread before the worker spawns).
 *
 * Reference-counted: pin N times, unpin N times to release. Pins
 * on non-GC values (numbers, strings, bools, nil) are no-ops. */
int praia_pin_value(PraiaValue v);
int praia_unpin_value(PraiaValue v);

/* ── Calling back into Praia ──────────────────────────────────── */

/* Invoke a Praia callable. `args` is a contiguous array of
 * borrowed PraiaValue handles. Returns a new owned result, or
 * NULL on a thrown error (last error set). */
PraiaValue praia_call(PraiaValue callable, const PraiaValue* args, size_t argc);

/* ── Cross-thread helpers ─────────────────────────────────────── */

/* Schedule `fn(userdata)` to run on the engine thread. Returns
 * 0 on success, -1 on error. Most error cases are programming
 * mistakes:
 *   • NULL fn — nothing to schedule
 *   • No executor on this thread — you called from a worker but
 *     forgot to capture praia::currentExecutor() (in the C++ ABI)
 *     on the engine thread before spawning the worker. The C
 *     facade currently looks up the executor itself on the
 *     calling thread, so worker calls only succeed via the
 *     dedicated engine-thread wrapper around them.
 *
 * Returning a value rather than void so the worker can observe
 * the failure — the staged last-error message is consumed by
 * the next native return on the engine thread, but a worker has
 * no such return point and would otherwise lose the diagnostic. */
int praia_post_to_engine(void (*fn)(void* userdata), void* userdata);

/* Cooperative cancellation. Returns:
 *   -1  no cancel token bound to this thread (user code didn't
 *       wrap this call in withCancel — most cases)
 *    0  token bound, not cancelled
 *    1  token bound and cancelled — bail your loop ASAP */
int praia_should_cancel(void);

/* Promise — for plugins that fulfil a Future from a worker thread.
 * The Praia side does `await plugin.fetch(…)`; you return the
 * future on the engine thread, then resolve from the worker. */
PraiaPromise praia_promise_new(void);

/* Returns a new owned PraiaValue (a Future) — hand back to the
 * engine as your native's return value. */
PraiaValue praia_promise_future(PraiaPromise p);

/* Fulfil. First call wins; later resolve/reject calls are no-ops.
 * Safe from any thread, including workers without an executor.
 * `result` is copied (the engine takes its own reference); the
 * caller still owns the handle they passed in and must release
 * it. NULL is accepted and stored as a nil value. */
void praia_promise_resolve(PraiaPromise p, PraiaValue result);
void praia_promise_reject(PraiaPromise p, const char* message);

/* Drop the Promise handle. The underlying state survives if the
 * Future is still pending; the engine surfaces a "dropped before
 * resolve or reject" error to await callers when nobody fulfils
 * before the last handle goes away. */
void praia_promise_release(PraiaPromise p);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* PRAIA_PLUGIN_C_H */
