// cmod.c — canonical pure-C plugin demonstrating the praia_plugin_c.h
// facade. Five natives that exercise each shape: string ops, integer
// ops with arg validation, error throwing, external handles with a
// deleter, and map construction.
//
// Build (macOS):
//   gcc -std=c11 -shared -fPIC -I$(praia --include-path) \
//       -undefined dynamic_lookup -o cmod.dylib cmod.c
//
// Build (Linux):
//   gcc -std=c11 -shared -fPIC -I$(praia --include-path) \
//       -o cmod.so cmod.c

#include "praia_plugin_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

PRAIA_C_DECLARE_ABI();
PRAIA_C_PLUGIN_METADATA("cmod", "0.1.0",
                       "Pure-C plugin demo for the praia_plugin_c.h facade");

// ── greet(name) → "hello, <name>" ────────────────────────────────

static PraiaValue cmod_greet(PraiaArgs args, void* ud) {
    (void) ud;
    if (praia_args_count(args) != 1) {
        praia_throw("cmod.greet: expected 1 argument");
        return NULL;
    }
    PraiaValue name = praia_args_get(args, 0);
    if (!praia_value_is_string(name)) {
        praia_throw("cmod.greet: expected a string");
        return NULL;
    }
    size_t n = 0;
    const char* s = praia_value_as_string(name, &n);
    // 7 = strlen("hello, "), +1 for NUL. Cap at 256 to keep stack
    // usage bounded; longer names get truncated rather than
    // reallocating.
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "hello, %.*s", (int)n, s);
    if (len < 0) {
        praia_throw("cmod.greet: snprintf failed");
        return NULL;
    }
    if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;
    return praia_value_string_n(buf, (size_t)len);
}

// ── add(a, b) → a+b ──────────────────────────────────────────────

static PraiaValue cmod_add(PraiaArgs args, void* ud) {
    (void) ud;
    if (praia_args_count(args) != 2) {
        praia_throw("cmod.add: expected 2 arguments");
        return NULL;
    }
    PraiaValue a = praia_args_get(args, 0);
    PraiaValue b = praia_args_get(args, 1);
    if (!praia_value_is_int(a) || !praia_value_is_int(b)) {
        praia_throw("cmod.add: both arguments must be ints");
        return NULL;
    }
    int64_t ai = praia_value_as_int(a);
    int64_t bi = praia_value_as_int(b);
    // Signed-overflow check before adding — `int64_t + int64_t` on
    // overflow is undefined behavior in C, and a demo plugin
    // shouldn't leave that landmine for readers to copy. The two
    // bounds checks below are the canonical portable formulation
    // (gcc/clang also offer __builtin_add_overflow if you want the
    // single-call version).
    if ((bi > 0 && ai > INT64_MAX - bi) ||
        (bi < 0 && ai < INT64_MIN - bi)) {
        praia_throw("cmod.add: integer overflow");
        return NULL;
    }
    return praia_value_int(ai + bi);
}

// ── boom() → throws unconditionally ──────────────────────────────

static PraiaValue cmod_boom(PraiaArgs args, void* ud) {
    (void) args; (void) ud;
    praia_throw("cmod.boom: this is a deliberate error");
    return NULL;
}

// ── box() → external handle with a counting deleter ──────────────
//
// Demonstrates the ownership-transfer-to-Value pattern: malloc a C
// resource, wrap with a deleter that fires when the GC sweeps the
// Value. We instrument the deleter with a process-global counter
// so the test suite can verify the deleter fires exactly once.

static const char* CMOD_BOX_TYPE = "cmod.box";

// Module-private counter incremented by cmod_box_free. Exposed via
// cmod.box_freed_count() so Praia code can assert post-GC.
static int cmod_box_free_count = 0;

static void cmod_box_free(void* ptr) {
    cmod_box_free_count++;
    free(ptr);
}

static PraiaValue cmod_box(PraiaArgs args, void* ud) {
    (void) args; (void) ud;
    // Trivial payload: a malloc'd byte that the deleter frees.
    // Real plugins would wrap an OS handle / DB connection / etc.
    int* payload = (int*) malloc(sizeof(int));
    if (!payload) {
        praia_throw("cmod.box: out of memory");
        return NULL;
    }
    *payload = 42;
    return praia_value_new_external(payload, CMOD_BOX_TYPE, cmod_box_free);
}

static PraiaValue cmod_box_unwrap(PraiaArgs args, void* ud) {
    (void) ud;
    if (praia_args_count(args) != 1) {
        praia_throw("cmod.box_unwrap: expected 1 argument");
        return NULL;
    }
    PraiaValue h = praia_args_get(args, 0);
    int* payload = (int*) praia_value_external_ptr(h, CMOD_BOX_TYPE);
    if (!payload) return NULL;   // praia_value_external_ptr already threw
    return praia_value_int((int64_t) *payload);
}

static PraiaValue cmod_box_freed_count(PraiaArgs args, void* ud) {
    (void) args; (void) ud;
    return praia_value_int((int64_t) cmod_box_free_count);
}

// ── dict(k, v) → {k: v} ──────────────────────────────────────────

static PraiaValue cmod_dict(PraiaArgs args, void* ud) {
    (void) ud;
    if (praia_args_count(args) != 2) {
        praia_throw("cmod.dict: expected 2 arguments");
        return NULL;
    }
    PraiaValue k = praia_args_get(args, 0);
    PraiaValue v = praia_args_get(args, 1);
    PraiaValue m = praia_value_new_map();
    // Propagate any staged error from map_set (e.g. non-hashable
    // key) — without returning NULL we'd silently return an empty
    // map and the user would never see the diagnostic.
    if (praia_value_map_set(m, k, v) != 0) {
        praia_value_release(m);
        return NULL;
    }
    return m;
}

// ── error_intercept(msg) → "intercepted: <msg>" ─────────────────
//
// Demonstrates praia_last_error / praia_take_error: the native
// stages an error with praia_throw, then immediately reads it
// back, drains the slot via praia_take_error, and returns the
// drained text as a normal success value. The user sees a
// returned string, never an exception — proving that an
// intercepted/handled error doesn't leak to the next call.

static PraiaValue cmod_error_intercept(PraiaArgs args, void* ud) {
    (void) ud;
    if (praia_args_count(args) != 1) {
        praia_throw("cmod.error_intercept: expected 1 argument");
        return NULL;
    }
    PraiaValue arg = praia_args_get(args, 0);
    if (!praia_value_is_string(arg)) {
        praia_throw("cmod.error_intercept: expected a string");
        return NULL;
    }
    size_t n = 0;
    const char* s = praia_value_as_string(arg, &n);
    // Stage an error, then read it back to prove the slot works
    // both for reading (praia_last_error) and draining
    // (praia_take_error). After the drain the slot is empty, so
    // we can return non-NULL without the facade surfacing a stale
    // error to the caller.
    char staged[256];
    snprintf(staged, sizeof(staged), "intercepted: %.*s", (int)n, s);
    praia_throw(staged);

    // Peek without consuming — pointer is borrowed from the
    // facade's thread-local buffer, valid until the next stage/drain.
    const char* peek = praia_last_error();
    (void) peek;   // not used in the demo; just exercising the call

    char drained[256];
    int had = praia_take_error(drained, sizeof(drained));
    if (!had) {
        // Defensive: praia_throw above should have set the slot.
        return praia_value_string("(no error was staged)");
    }
    return praia_value_string(drained);
}

// ── makeset(a, b, c) → #{a, b, c} ────────────────────────────────
//
// Demonstrates the new_set + set_add + set_values API. Variadic
// arity (-1) so the caller can pass any number of members.

static PraiaValue cmod_makeset(PraiaArgs args, void* ud) {
    (void) ud;
    PraiaValue s = praia_value_new_set();
    int n = praia_args_count(args);
    for (int i = 0; i < n; i++) {
        if (praia_value_set_add(s, praia_args_get(args, i)) != 0) {
            praia_value_release(s);
            return NULL;
        }
    }
    return s;
}

// ── Registration ─────────────────────────────────────────────────

// Tiny helper to compress the "make + set + release" triad each
// native registration needs. `name` is the fully-qualified label
// used in tracebacks; `key` is the bare attribute the user sees
// on the module map. We pass both rather than deriving one from
// the other so a stray missing-prefix typo doesn't quietly read
// past the start of the name string.
//
// praia_make_native returns NULL on OOM (and stages a message).
// We check the return + read the staged error via
// praia_last_error so the user sees *why* a native is missing
// from the module instead of "praia_register succeeded but
// cmod.greet doesn't exist." Returns 0 on success, -1 on
// failure — praia_register itself can't fail the load, but the
// caller can decide to skip the rest of registration.
static int reg(PraiaMapHandle* module, const char* name, const char* key,
               int arity, PraiaNativeFn fn) {
    PraiaValue v = praia_make_native(name, arity, fn, NULL);
    if (!v) {
        fprintf(stderr, "cmod: failed to register %s: %s\n",
                name, praia_last_error());
        return -1;
    }
    praia_module_set(module, key, v);
    praia_value_release(v);
    return 0;
}

// Bail on the first reg() failure rather than continuing with a
// partially-registered module. praia_register itself is void —
// the engine can't be told "the load failed" — but stopping
// early means the user sees a single "failed to register X"
// line on stderr and consistently-missing later entries, not a
// scattergun of partially-present functions whose absence is
// surprising. Use the `||` short-circuit chain so the failure
// message comes out at the *first* missing native, not the
// last.
void praia_register(PraiaMapHandle* module) {
    if (reg(module, "cmod.greet",            "greet",            1,  cmod_greet))           return;
    if (reg(module, "cmod.add",              "add",              2,  cmod_add))             return;
    if (reg(module, "cmod.boom",             "boom",             0,  cmod_boom))            return;
    if (reg(module, "cmod.box",              "box",              0,  cmod_box))             return;
    if (reg(module, "cmod.box_unwrap",       "box_unwrap",       1,  cmod_box_unwrap))      return;
    if (reg(module, "cmod.box_freed_count",  "box_freed_count",  0,  cmod_box_freed_count)) return;
    if (reg(module, "cmod.dict",             "dict",             2,  cmod_dict))            return;
    if (reg(module, "cmod.makeset",          "makeset",         -1,  cmod_makeset))         return;
    if (reg(module, "cmod.error_intercept",  "error_intercept",  1,  cmod_error_intercept)) return;
}
