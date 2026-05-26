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
    return praia_value_int(praia_value_as_int(a) + praia_value_as_int(b));
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
    praia_value_map_set(m, k, v);
    return m;
}

// ── Registration ─────────────────────────────────────────────────

// Tiny helper to compress the "make + set + release" triad each
// native registration needs. Saves enough boilerplate per entry
// to be worth it once you're past two natives.
static void reg(PraiaMapHandle* module, const char* name, int arity,
                PraiaNativeFn fn) {
    PraiaValue v = praia_make_native(name, arity, fn, NULL);
    praia_module_set(module, name + strlen("cmod."), v);  // drop "cmod." prefix on the exported key
    praia_value_release(v);
}

void praia_register(PraiaMapHandle* module) {
    reg(module, "cmod.greet",            1,  cmod_greet);
    reg(module, "cmod.add",              2,  cmod_add);
    reg(module, "cmod.boom",             0,  cmod_boom);
    reg(module, "cmod.box",              0,  cmod_box);
    reg(module, "cmod.box_unwrap",       1,  cmod_box_unwrap);
    reg(module, "cmod.box_freed_count",  0,  cmod_box_freed_count);
    reg(module, "cmod.dict",             2,  cmod_dict);
}
