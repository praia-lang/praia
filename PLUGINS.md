# Native Plugins

Praia loads native modules at runtime via `loadNative()`. There are two SDKs, picked by the `#include` you use:

| SDK | Header | Stability | When to reach for it |
|-----|--------|-----------|---------------------|
| C++ source SDK | `praia_plugin.h` | **Source-coupled to the engine's C++ toolchain.** Re-exports `Value` (`std::variant`), `std::shared_ptr` containers, `std::function`, templates, `RuntimeError` exceptions. Variant/shared_ptr/function layouts differ across compilers and minor stdlib releases. | You control the build (same toolchain compiles engine + plugin). Canonical for source builds, the `make plugin` target, and the in-tree `examples/plugins/*.cpp`. |
| C stable ABI | `praia_plugin_c.h` | **Stable across compilers and languages.** Opaque pointers + plain function-pointer ABI; works from C, Rust, Zig, Go (cgo), Swift, anything with a C FFI. | Distributing prebuilt binaries via the `prebuilt:` block in `grain.yaml`. Writing the plugin in any language other than C++. Building portable artifacts that will load against engines compiled by a different toolchain than yours. |

Both share the same `PRAIA_PLUGIN_ABI_VERSION` gate — a single engine binary loads plugins built with either header. The C++ SDK is documented first below; jump to [Writing plugins in pure C](#writing-plugins-in-pure-c) for the C facade.

## Quick start

**1. Write a plugin** (`mymodule.cpp`):

```cpp
#include "praia_plugin.h"

PRAIA_DECLARE_ABI();

extern "C" void praia_register(PraiaMap* module) {
    module->entries["double"] = Value(makeNative("mymodule.double", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("expected a number", 0);
            return Value(args[0].asNumber() * 2);
        }));
}
```

`PRAIA_DECLARE_ABI()` is required — see [ABI versioning](#abi-versioning) below.

**2. Build it:**

```sh
make plugin SRC=mymodule.cpp OUT=mymodule.dylib   # macOS
make plugin SRC=mymodule.cpp OUT=mymodule.so      # Linux
```

**3. Use it in Praia:**

```
let mod = loadNative("./mymodule")
print(mod.double(21))  // 42
```

## Writing plugins in pure C

If you prefer C — or you're binding to Praia from Rust, Zig, Go, Swift, or any other language with a C FFI — `#include "praia_plugin_c.h"` instead. It exposes the same engine functionality as `praia_plugin.h`, but through pure-C signatures: opaque handles (`PraiaValue`, `PraiaPromise`), function pointers + `void* userdata` for callbacks, and explicit `release`/`pin`/`unpin` lifetime calls. Same engine binary, same ABI version, same `loadNative` gate.

**Build (macOS):**

```sh
gcc -std=c11 -shared -fPIC -I$(praia --include-path) \
    -undefined dynamic_lookup -o mymodule.dylib mymodule.c
```

**Build (Linux):**

```sh
gcc -std=c11 -shared -fPIC -I$(praia --include-path) \
    -o mymodule.so mymodule.c
```

**Minimal plugin** (`mymodule.c`):

```c
#include "praia_plugin_c.h"
#include <stdio.h>

PRAIA_C_DECLARE_ABI();
PRAIA_C_PLUGIN_METADATA("mymod", "0.1.0", "Does the thing");

static PraiaValue greet(PraiaArgs args, void* ud) {
    (void) ud;
    if (praia_args_count(args) != 1) {
        praia_throw("greet: expected 1 argument");
        return NULL;
    }
    PraiaValue name = praia_args_get(args, 0);
    size_t n = 0;
    const char* s = praia_value_as_string(name, &n);
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "hello, %.*s", (int)n, s);
    return praia_value_string_n(buf, (size_t)len);
}

void praia_register(PraiaMapHandle* module) {
    PraiaValue fn = praia_make_native("mymod.greet", 1, greet, NULL);
    praia_module_set(module, "greet", fn);
    praia_value_release(fn);
}
```

### Lifetime contract

Three rules cover the whole API:

- **Returned handles are owned.** Every `PraiaValue` you receive *from* the facade (constructors, `praia_value_map_get`, `praia_call`, `praia_promise_future`) must be released with `praia_value_release` when you're done.
- **Setter args are copied.** When you hand a `PraiaValue` to `praia_value_map_set`, `praia_value_array_push`, `praia_module_set`, etc., the engine takes its own reference. You still own (and must release) the handle you passed in.
- **Callback args are borrowed.** Inside a `PraiaNativeFn`, the handles returned by `praia_args_get` belong to the engine for the duration of the call. Don't release them. If you need them to outlive the call, `praia_value_clone` first.

A native callback's *return value* transfers ownership to the engine — return the `PraiaValue` directly without releasing it.

### Error model

C has no exceptions, so the facade uses a two-step convention:

1. The native calls `praia_throw("message")` to stage an error.
2. The native returns `NULL`.

The facade thunk picks up the staged message and throws a `RuntimeError` on the C++ side, which propagates to user code identically to a C++ plugin throw.

A `NULL` return *without* a prior `praia_throw` is also caught, but the surfaced message is the literal placeholder `<C native returned NULL without calling praia_throw>`. Seeing that in your stack trace means your native bailed without staging a diagnostic — a plugin bug to fix in the C code, not a runtime condition for user code to handle. Always pair every NULL return with a matching `praia_throw` so the user sees a real message.

### Working examples

- [`examples/plugins/cmod.c`](examples/plugins/cmod.c) — pure-C plugin demonstrating string/int ops, error throwing, external handles with deleters, and map construction. The accompanying tests live at [`tests/test_loadnative_c.praia`](tests/test_loadnative_c.praia).
- [`examples/plugins/rust-cmod/`](examples/plugins/rust-cmod/) — the same shape in pure Rust (hand-rolled `extern "C"` bindings against `praia_plugin_c.h`). Same approach works for Zig, Go via cgo, Swift, and any other language with a C FFI.

For C++ instead, see [Quick start](#quick-start) above.

## ABI versioning

The plugin API is versioned. Every plugin must invoke `PRAIA_DECLARE_ABI()` once at file scope:

```cpp
#include "praia_plugin.h"

PRAIA_DECLARE_ABI();

// ... your praia_register etc.
```

`loadNative()` checks the declared version against the running praia binary's compiled-in version (`PRAIA_PLUGIN_ABI_VERSION` in `praia_plugin.h`) and refuses plugins that don't match — with a clear "rebuild the plugin against the current headers" error rather than crashing later from a layout mismatch. Plugins that omit `PRAIA_DECLARE_ABI()` are refused for the same reason.

When the ABI changes (e.g. `Value`'s variant layout or `PraiaMap`'s key type), the version bumps. Existing plugins continue working at the older praia release; rebuilding picks up the new version automatically.

## Plugin metadata

Optional. Declare a `name`, `version`, and `description` once at file scope:

```cpp
#include "praia_plugin.h"

PRAIA_DECLARE_ABI();
PRAIA_PLUGIN_METADATA("mymod", "1.2.0", "Does the thing");
```

`loadNative()` exposes the values under a `_meta` key on the returned module map, so user code can do:

```praia
let mod = loadNative("./mymod")
print(mod._meta.name, mod._meta.version)
```

Plugins that don't declare metadata get no `_meta` key — backward-compatible and opt-in. The underscore prefix keeps it from colliding with normal module functions.

## Opaque handles

For plugins wrapping a C/C++ resource — a DB connection, a file handle, an OS object — return a `praia::makeExternal<T>(ptr, typeName, deleter)` Value instead of squeezing the pointer into a map or string. The Value:

- Holds the opaque pointer + a destructor that fires when the last reference drops (during the GC sweep).
- Carries a `typeName` tag so the plugin can type-check on unwrap (`praia::getExternal<T>(value, typeName)`).
- Prints as `<external:typeName>` and compares by pointer identity.
- Can be passed through arrays, maps, function calls — but isn't hashable (can't be a map key).

### Example: wrapping a SQLite connection

The handle wraps `sqlite3*` in a small struct alongside a `closed` flag, so explicit `db.close(conn)` and the GC-time deleter are both idempotent — running close twice (or close-then-GC) doesn't double-free.

```cpp
#include "praia_plugin.h"
#include <sqlite3.h>

PRAIA_DECLARE_ABI();

namespace {
struct sqlite3_wrapper {
    sqlite3* conn = nullptr;
    bool closed = false;
};
}

extern "C" void praia_register(PraiaMap* module) {
    module->entries["open"] = Value(makeNative("db.open", 1,
        [](const std::vector<Value>& args) -> Value {
            auto& path = praia::requireString(args, 0, "db.open");
            auto* w = new sqlite3_wrapper;
            if (sqlite3_open(path.c_str(), &w->conn) != SQLITE_OK) {
                // sqlite3_open can leave conn as NULL on OOM in
                // older SQLite builds — guard before reading the
                // error message or calling close on it.
                std::string msg = w->conn ? sqlite3_errmsg(w->conn)
                                          : "out of memory";
                if (w->conn) sqlite3_close_v2(w->conn);
                delete w;
                praia::error("db.open: " + msg);
            }
            return praia::makeExternal<sqlite3_wrapper>(w, "sqlite3.connection",
                [](sqlite3_wrapper* p) {
                    // GC-time cleanup: skip the close if `db.close`
                    // already ran. Either way, free the wrapper.
                    if (!p->closed) sqlite3_close_v2(p->conn);
                    delete p;
                });
        }));

    module->entries["close"] = Value(makeNative("db.close", 1,
        [](const std::vector<Value>& args) -> Value {
            auto* w = praia::getExternal<sqlite3_wrapper>(args[0], "sqlite3.connection");
            if (w->closed) return Value();   // idempotent
            // sqlite3_close_v2 (vs the v1 close) handles the busy
            // case by deferring actual cleanup until any outstanding
            // prepared statements and blob handles are finalized,
            // rather than returning SQLITE_BUSY. So we can mark
            // `closed` unconditionally — the GC deleter's
            // `if (!p->closed)` guard will skip a second close,
            // and SQLite handles the rest.
            sqlite3_close_v2(w->conn);
            w->closed = true;
            return Value();
        }));
}
```

User code:

```praia
let db = loadNative("./mydb")
let conn = db.open(":memory:")
// ... use conn ...
// when `conn` goes out of scope (or is reassigned), sqlite3_close runs.
```

### TypeName convention

Use `"module.type"` — e.g. `"sqlite.connection"`, `"prscan.scanner"`. The string isn't accessible via any Praia-side API or field, but it IS visible in string form: `str(handle)` and `print(handle)` show `<external:typeName>`. Treat it as a diagnostic-only label from Praia's perspective, and as the plugin's internal type tag from C++. Two different plugins using the same `typeName` for unrelated handles will accept each other's handles in `getExternal<T>` — choose a prefix that's unlikely to collide.

### Lifetime

The deleter runs in `PraiaExternal::~PraiaExternal()` when the last `shared_ptr<PraiaExternal>` drops:

- Reassigning the last user-code variable: deleter runs immediately (refcount drop).
- Going out of scope: same.
- Held in a map / closed-over by a callable: stays alive until the container is itself dropped or GC-swept.

If your wrapped resource holds back-references into Praia (e.g. a callback registered with a C library), don't let the C side outlive the handle — the deleter is your last chance to unregister.

### What user code sees

A Value that prints as `<external:typeName>`, passes through assignments / function calls / arrays / maps unchanged, and can't be used as a map key (the underlying pointer isn't hashable). The typeName isn't accessible via any API or field — but it IS visible in string form (`str(h)`, `print(h)`, REPL output) as part of the `<external:...>` rendering. Treat that string output as a diagnostic label, not a stable contract; for type checks, use `praia::getExternal<T>` on the C++ side.

## GC roots: stashing Values across native calls

If your plugin stashes a Praia `Value` in C++ static / global storage that outlives the native function call — a callback table, a cache, a singleton — you **must** pin it as a GC root. Otherwise the next garbage collection treats the value as unreachable and the sweep hollows out the underlying object's internal references. The `shared_ptr` you held stays valid, but the object is empty: a stashed `Callable` finds an empty closure environment, a cached `PraiaMap` finds its entries cleared.

`praia::pinValue` registers the value with the current thread's `GcHeap` so the mark phase visits it on every collection. Reference-counted by multiplicity: pin N times means N unpins to release. The matching uses last-in-first-out, so nested RAII pin/unpin pairs work without bookkeeping.

```cpp
#include "praia_plugin.h"
#include <unordered_map>

namespace {
std::unordered_map<std::string, Value> g_callbacks;
}

extern "C" void praia_register(PraiaMap* module) {
    module->entries["on"] = Value(makeNative("mod.on", 2,
        [](const std::vector<Value>& args) -> Value {
            auto& name = praia::requireString(args, 0, "mod.on");
            praia::requireCallable(args, 1, "mod.on");   // validate type
            // Release the old pin BEFORE overwriting the map slot —
            // otherwise the old value leaks a pin forever.
            auto it = g_callbacks.find(name);
            if (it != g_callbacks.end()) praia::unpinValue(it->second);
            praia::pinValue(args[1]);
            g_callbacks[name] = args[1];
            return Value();
        }));

    module->entries["fire"] = Value(makeNative("mod.fire", 1,
        [](const std::vector<Value>& args) -> Value {
            auto& name = praia::requireString(args, 0, "mod.fire");
            auto it = g_callbacks.find(name);
            if (it == g_callbacks.end()) return Value();
            // Safe to call: the pin kept the callable's closure
            // environment alive across however many GC sweeps fired
            // between `mod.on(...)` and now.
            return praia::call(it->second.asCallable(), {});
        }));
}
```

### When you need this

| Storage shape | Pin needed? |
|---|---|
| Local `Value` / `Value` argument in a non-reentrant native call | No. The C++ stack frame keeps the `shared_ptr` alive, and nothing else runs between native entry and return to trigger a sweep. |
| Local `Value` / argument across a `praia::call` callback, async yield, or any other reentrant path | **Yes.** Re-entering the interpreter can run a GC sweep on this thread. The C++ stack pins the `shared_ptr` (so the object's address stays valid) but the sweep can still clear the object's *internal* references — see [Scope guard (RAII)](#scope-guard-raii) below. |
| `Value` stashed in C++ static / global between calls | **Yes.** |
| `Value` inside a `PraiaExternal`'s opaque data (e.g. an SQLite connection wrapper) | **Yes, if the stored Value is GC-tracked.** The external itself is GC-tracked, but its `data` field is just `void*` — the GC doesn't know to walk inside. Pin any Praia Values you store there. |

The rule in one sentence: **a local `Value` is safe without a pin only while no Praia code can run on this thread**. The moment you call back into the interpreter — directly via `praia::call`, indirectly via anything that allocates Praia objects, or implicitly across an async yield — a sweep can fire and the value's internals are exposed.

### When you don't

Values that aren't GC-tracked (numbers, strings, bools, nil) accept `pinValue` harmlessly — the registry adds the entry, the mark phase no-ops on the unwrappable type, and the sweep can't break anything anyway. There's no need to type-check before pinning.

### Scope guard (RAII)

For short-lived holds inside a native function — saving an argument across a `praia::call` that recursively triggers GC, or buffering a value across any other re-entrant path — use `PinnedValue`. Move-only; pin on construction, unpin on destruction; bound to the heap it pinned against so a moved guard releases against the correct registry.

```cpp
module->entries["recurseSafely"] = Value(makeNative("mod.recurseSafely", 2,
    [](const std::vector<Value>& args) -> Value {
        praia::PinnedValue keepAlive(args[0]);   // pin until scope exit
        // The Praia callback might internally trigger GC pressure.
        // Without `keepAlive`, args[0]'s captured env could be cleared
        // mid-call — even though args[0] is still on the C++ stack.
        praia::call(args[1].asCallable(), {});
        return keepAlive.get();
    }));
```

This is the same situation the table's second row points at: a local/argument value that crosses a re-entrant boundary. Reach for `PinnedValue` (or the lower-level `pinValue` / `unpinValue` pair) whenever a value has to survive a call back into the interpreter.

### Common bugs

- **Replace-without-unpin.** Storing a new value in a slot you already pinned without unpinning the old one leaks a pin every time. The old value stays a root forever; eventually the registry walks slow as it grows.
- **Unpin-without-pin.** `praia::unpinValue(v)` is silent on a missing match — won't crash, but also won't tell you the bookkeeping is off.
- **Pinning the wrong thread's heap.** Each thread has its own `GcHeap`. Pin in the thread that holds the C++ storage. If your plugin spans threads, pin per-thread or pin only on the owning thread.

## Plugin API

### Entry point

Every plugin must export a single C function:

```cpp
extern "C" void praia_register(PraiaMap* module);
```

This function receives a pointer to an empty `PraiaMap`. Populate its `entries` with native functions. The map is returned to the Praia caller.

### Creating functions

Use `makeNative(name, arity, fn)` to create native functions:

```cpp
module->entries["add"] = Value(makeNative("mymod.add", 2,
    [](const std::vector<Value>& args) -> Value {
        return Value(args[0].asNumber() + args[1].asNumber());
    }));
```

- `name` — display name for error messages
- `arity` — number of parameters, or `-1` for variadic
- `fn` — `std::function<Value(const std::vector<Value>&)>`
- `paramNames` (optional) — vector of parameter names. Pass it to let user code call your function with named arguments:

  ```cpp
  module->entries["add"] = Value(makeNative("mymod.add", 2,
      [](const std::vector<Value>& args) -> Value {
          return Value(args[0].asNumber() + args[1].asNumber());
      },
      {"x", "y"}));
  ```

  Praia code can then write `mod.add(x: 1, y: 2)` or `mod.add(y: 2, x: 1)`. Without `paramNames`, named-arg calls throw the standard "Named arguments not supported" error — same behavior as before, no impact on plain positional calls.

### Argument validation

Type and arity checks are repetitive and the wording matters for diagnostic uniformity. `praia_plugin.h` exports a family of inline helpers in the `praia` namespace that throw a `RuntimeError` with the canonical wording the rest of the engine uses:

```cpp
module->entries["substring"] = Value(makeNative("mymod.substring", 3,
    [](const std::vector<Value>& args) -> Value {
        auto& s = praia::requireString(args, 0, "mymod.substring");
        int64_t start = praia::requireInt(args, 1, "mymod.substring");
        int64_t end   = praia::requireInt(args, 2, "mymod.substring");
        return Value(s.substr(start, end - start));
    }));
```

The error messages match Praia's stdlib conventions exactly:

- `requireString(args, i, "fn")` → `"fn() argument N must be a string"` (N is 1-based)
- `requireInt`, `requireNumber`, `requireBool`, `requireArray`, `requireMap`, `requireCallable` — same shape, different type word.
- `requireArity(args, n, "fn")` → `"fn() expected N argument(s) but got M"`
- `requireArityRange(args, lo, hi, "fn")` → `"fn() expected LO-HI arguments but got M"`
- `praia::error("msg")` — bare shorthand for `throw RuntimeError("msg", 0);` for cases the helpers don't cover.

`requireInt` is strict — it rejects float values that happen to be whole numbers; use `requireNumber` (which returns a double, converting ints) when either form is acceptable. This matches the `asInt`/`asNumber` distinction in the [Value type](#the-value-type) pitfall callout below.

### The Value type

`Value` is a variant that holds any Praia value:

| Constructor | Praia type |
|------------|------------|
| `Value()` or `Value(nullptr)` | nil |
| `Value(true)` | bool |
| `Value(42)` | int |
| `Value(3.14)` | float |
| `Value(std::string("hi"))` or `Value("hi")` | string |
| `Value(shared_ptr<PraiaArray>)` | array |
| `Value(shared_ptr<PraiaMap>)` | map |
| `Value(shared_ptr<PraiaSet>)` | set |
| `Value(shared_ptr<Callable>)` | function |

Type checking and accessors:

```cpp
args[0].isString()    // type check
args[0].asString()    // returns const std::string&
args[0].isNumber()    // true for int or float
args[0].asNumber()    // returns double (converts int)
args[0].isInt()       // true only for int
args[0].asInt()       // returns int64_t — only safe after isInt()
args[0].isArray()     // true for array
args[0].asArray()     // returns shared_ptr<PraiaArray>
args[0].isMap()       // true for map
args[0].asMap()       // returns shared_ptr<PraiaMap>
args[0].isCallable()  // true for function/lambda
args[0].asCallable()  // returns shared_ptr<Callable>
```

> **Pitfall.** `asInt()` is *only* safe after `isInt()` (`std::get` throws `bad_variant_access` on a double-holding Value). If you guard with `isNumber()` instead, use `asNumber()` — it returns a double and converts ints transparently.

> **`std::string` doubles as Praia's bytes type.** Praia uses a single string type for both text and binary data, like Python's bytes objects. Plugins wrapping binary protocols can put raw bytes (including embedded NULs) into `Value(std::string)`; they round-trip through `asString()` byte-for-byte. Don't reach for `std::vector<uint8_t>` or invent a Bytes wrapper — the existing string accessors handle it.

### Creating arrays and maps

Use `gcNew<T>()` to create GC-tracked containers:

```cpp
auto arr = gcNew<PraiaArray>();
arr->elements.push_back(Value(1));
arr->elements.push_back(Value(2));
return Value(arr);

auto map = gcNew<PraiaMap>();
map->entries["key"] = Value("value");
return Value(map);
```

Always use `gcNew` instead of `std::make_shared` — it registers the object with Praia's garbage collector.

### Calling Praia code back

When your plugin accepts a `Callable` argument (a user-provided lambda or function), invoke it through `praia::call`:

```cpp
#include "praia_plugin.h"

extern "C" void praia_register(PraiaMap* module) {
    // filter(array, predicate) — keep elements where predicate(x) is truthy
    module->entries["filter"] = Value(makeNative("mymod.filter", 2,
        [](const std::vector<Value>& args) -> Value {
            auto pred = args[1].asCallable();
            auto out = gcNew<PraiaArray>();
            for (auto& elem : args[0].asArray()->elements) {
                Value keep = praia::call(pred, {elem});
                if (keep.isTruthy()) out->elements.push_back(elem);
            }
            return Value(out);
        }));
}
```

`praia::call` works for both Praia engines (the tree-walker and the bytecode VM) — your plugin doesn't need to know which one is running. See `examples/plugins/mathext.cpp::filter` for the full example.

**Pitfall — deferred invocation.** Callbacks must be invoked synchronously from the same plugin call that received them. If you stash a callable and invoke it later from a worker thread, a libuv-style scheduler, or any other off-engine context, `praia::call` will throw with "no active Praia executor on this thread". Stack-based VMs and tracing GCs can't safely have user code called on threads they didn't sanction. Use `praia::postToEngine` (next section) to marshal the call back to the engine.

### Calling back from background threads — `praia::postToEngine`

The previous section's pitfall — "callbacks must be invoked synchronously" — leaves a real gap: plugins that genuinely do background work (libuv-style I/O, `std::thread` workers, native event loops) need a way to fire Praia callbacks when the background work finishes. `praia::postToEngine` is the supported path.

The shape:

```cpp
void praia::postToEngine(void* engine,
                         std::shared_ptr<Callable> fn,
                         std::vector<Value> args);
```

The `engine` token is captured on the engine thread via `praia::currentExecutor()` (the same token `praia::call` uses internally). From any thread — even ones that have no Praia executor of their own — call `postToEngine` to enqueue `fn(args)` for execution on the engine's next yield point.

Canonical pattern (the engine schedules a callback to fire after a delay computed by a worker):

```cpp
#include "praia_plugin.h"
#include <chrono>
#include <thread>

extern "C" void praia_register(PraiaMap* module) {
    module->entries["scheduleAfter"] = Value(makeNative("mod.scheduleAfter", 2,
        [](const std::vector<Value>& args) -> Value {
            int  ms = (int)praia::requireNumber(args, 0, "mod.scheduleAfter");
            auto cb = praia::requireCallable(args, 1, "mod.scheduleAfter");

            // Pin the callable so the engine's GC doesn't collect its
            // closure environment between now and the post firing.
            // Pin runs on the engine thread (we're inside a native
            // call); the worker can't pin from its own thread.
            praia::pinValue(args[1]);

            void* engine = praia::currentExecutor();   // capture HERE

            // Detach a worker. `engine`, `cb` and `cbValue` are
            // captured by value; nothing dereferences user-Praia
            // state from the worker thread itself.
            Value cbValue = args[1];
            std::thread([engine, cb, cbValue, ms]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                praia::postToEngine(engine, cb, {});
                // The matching unpin must run on the engine thread
                // because praia::unpinValue throws on threads with
                // no executor. Either:
                //   1. Have the callback unpin itself at the end of
                //      its body (the pattern used in the counter
                //      plugin's test harness), or
                //   2. Post a separate "release" callable after the
                //      first one fires.
                (void)cbValue;
            }).detach();

            return Value();
        }));
}
```

#### When the post actually fires

The engine drains its post queue at every "yield point":

- **Tree-walker:** before every statement and expression (via `checkInterrupt`).
- **Bytecode VM:** every ~1024 bytecode ops, at the same boundary as GC + SIGINT checks.

For interactive scripts and request handlers, the latency between `postToEngine` returning on the worker and the callback firing on the engine is microseconds. For tight engine loops that don't allocate (extremely rare in real code), it's bounded by 1024 ops — still negligible.

#### Lifetime and shutdown

The plugin is responsible for ensuring the engine outlives any pending posts. A plugin that spawns workers should join or otherwise drain them in its teardown path; posting to an engine that has already been destroyed is undefined behavior. The detached-thread example above is fine for short-lived workers (sleep, single async I/O); production plugins doing long-running background work should hold a `std::thread` handle and `.join()` on plugin teardown.

#### Fire-and-forget semantics

`postToEngine` has no return value. The deferred call's exceptions are caught by the drain loop and logged to stderr ("`[praia::postToEngine] callback raised: <message>`"); one bad callback can't poison the queue, but the caller can't observe success vs failure directly. For result-passing, capture a Praia `Queue` (from the `concurrency` builtin) or a shared map in the callback's closure and have the worker read it back.

#### What can go wrong synchronously

`postToEngine` itself only fails in one way: passing a null `engine` token throws `RuntimeError` ("called with a null executor token"). On a detached worker thread there's no enclosing handler, so an uncaught throw aborts via `std::terminate`. Make sure to capture the token *before* spawning the worker (`praia::currentExecutor()` on the engine thread can return null if the plugin is loaded from an unusual context), and consider guarding against `engine == nullptr` if your plugin's call graph allows it.

#### Why workers can't call praia::call or praia::pinValue

Both helpers begin with `if (!currentExecutor()) throw`. On a worker thread, `currentExecutor()` is null because no Praia engine has bound its thread-local for that thread. `postToEngine` is the only API documented to be safe to call from such threads — it carries the engine token explicitly so the queue knows which engine to wake.

### Error handling

Throw `RuntimeError` to report errors back to Praia:

```cpp
if (!args[0].isString())
    throw RuntimeError("myFunc() requires a string", 0);
```

The second argument is a line number hint (use `0` from plugins).

## Header

Include a single header:

```cpp
#include "praia_plugin.h"
```

This re-exports:
- `value.h` — `Value`, `PraiaArray`, `PraiaMap`, `PraiaSet`, `Callable`, `RuntimeError`
- `gc_heap.h` — `gcNew<T>()`
- `builtins.h` — `makeNative()`
- `praia_runtime.h` — the `praia::call` callback helper

## Building

The Makefile provides a convenience target:

```sh
make plugin SRC=path/to/plugin.cpp OUT=path/to/plugin.dylib
```

Or build manually using `praia --include-path` to find the headers. Match the flags from the Makefile target — `_XOPEN_SOURCE=600` is required on macOS (otherwise `fiber.h`'s transitive `<ucontext.h>` include errors), and the deprecated-declarations warning silences a noisy ucontext deprecation Apple ships:

```sh
# macOS
g++ -std=c++20 -Wno-deprecated-declarations \
    -D_XOPEN_SOURCE=600 -D_DARWIN_C_SOURCE \
    -shared -fPIC -I$(praia --include-path) \
    -undefined dynamic_lookup \
    -o myplugin.dylib myplugin.cpp

# Linux
g++ -std=c++20 -Wno-deprecated-declarations \
    -D_XOPEN_SOURCE=600 \
    -shared -fPIC -I$(praia --include-path) \
    -o myplugin.so myplugin.cpp
```

Plugins must be compiled with a C++ compiler (the plugin API uses C++ types). They can freely call C functions and wrap C libraries.

## Wrapping C libraries

Plugins are `.cpp` files, but can wrap any C library. For example, wrapping C's `strtoll` for base conversion:

```cpp
#include "praia_plugin.h"
#include <cstdlib>

PRAIA_DECLARE_ABI();

extern "C" void praia_register(PraiaMap* module) {
    module->entries["parseBase"] = Value(makeNative("mymod.parseBase", 2,
        [](const std::vector<Value>& args) -> Value {
            auto& str = args[0].asString();
            int base = static_cast<int>(args[1].asInt());
            return Value(static_cast<int64_t>(strtoll(str.c_str(), nullptr, base)));
        }));
}
```

(`PRAIA_DECLARE_ABI();` is required in every plugin source file — `loadNative()` refuses any plugin that omits it. See [ABI versioning](#abi-versioning).)

See [`examples/plugins/strutil.cpp`](examples/plugins/strutil.cpp) for a full example wrapping C standard library functions.

## Behavior

- **Extension auto-detection** — `loadNative("./mymod")` tries `.dylib` on macOS, `.so` on Linux
- **Caching** — loading the same path twice returns the cached module
- **Lifetime** — plugins are never unloaded; function pointers remain valid for the process lifetime
- **GC integration** — containers created with `gcNew` participate in Praia's garbage collector

### Thread safety

Plugin code can be called from one of three contexts. Pick the one that matches what your plugin does:

- **Synchronous calls.** Most plugins. Praia invokes your native function on its own thread; `gcNew` works as expected, `praia::call` works for callbacks, the GC sees your containers normally.
- **Inside an `async` task.** Praia ships your function to a worker thread for the task's lifetime, with the GC disabled while the task runs. You can still use `gcNew` for short-lived intermediate containers, but **don't let a `gcNew`-built object escape the task scope** — it isn't tracked by the parent's GC. Either return primitives, deep-copy outside the plugin, or marshal the work back to the parent. See the [async/Lock guide](https://praia.sh/docs/advanced/async) for the matching pattern on the Praia side.
- **Off-engine threads (e.g. libuv callback, your own `std::thread`).** Both `gcNew` and `praia::call` are unsafe. Use [`praia::postToEngine`](#calling-back-from-background-threads--praiaposttoengine) to schedule the work back on the engine thread.

## Distributing prebuilt binaries

Native plugins normally require the end user to have a C++ toolchain — `sand install` clones the grain, but you (or the user) still have to run `make`. That gates every plugin behind "has g++ and the library headers installed," which is a real wall for casual users.

`sand` supports a `prebuilt:` block in `grain.yaml` that points at per-platform binaries you publish to a GitHub Release (or any HTTPS host). On install, sand picks the entry matching the user's `<platform>-<arch>`, downloads it, verifies the sha256, and drops the binary into the grain's `plugins/` directory — so the existing platform-aware loader in `main.praia` finds it unchanged. On a miss (unsupported platform, no block), sand falls back to today's clone-and-build path.

### Schema

```yaml
name: curses
version: 1.1.1
main: main.praia
prebuilt:
  darwin-arm64:
    url: https://github.com/praia-lang/curses/releases/download/v1.1.1/curses.dylib
    integrity: sha256-1a2b3c4d…
  darwin-x86_64:
    url: https://github.com/praia-lang/curses/releases/download/v1.1.1/curses-x86_64.dylib
    integrity: sha256-4d5e6f7a…
  linux-x86_64:
    url: https://github.com/praia-lang/curses/releases/download/v1.1.1/curses-linux-x86_64.so
    integrity: sha256-7a8b9c0d…
  linux-aarch64:
    url: https://github.com/praia-lang/curses/releases/download/v1.1.1/curses-linux-aarch64.so
    integrity: sha256-d0e1f2a3…
```

**Platform key.** `<platform>-<arch>` where platform is `darwin` or `linux` and arch is what `uname -m` returns on the target (`arm64`, `x86_64`, `aarch64`). On macOS you can also publish a single `darwin-universal` entry (a `lipo`-merged Mach-O) — sand consults that as a fallback when no exact `darwin-<arch>` entry matches.

**URL.** Must be HTTPS. http:// and file:// are rejected — the sha256 check provides content integrity, https provides transport integrity, together they make a tampered binary observable before it runs.

**Integrity.** Subresource-integrity-style: `sha256-<64 lowercase hex chars>`. Sand recomputes the hash on the downloaded body and refuses to install on mismatch.

**Destination.** Sand writes the downloaded file to `plugins/<basename(url)>`. Keep your URLs ending in the same filename your `main.praia` loader expects (`<name>.dylib` on darwin, `<name>-linux-<arch>.so` on linux — the convention every existing plugin grain already uses).

### Authoring workflow

```sh
# 1. Build for each platform you publish.
make                                                    # macOS host build
./build-linux.sh                                        # cross-compile via Docker

# 2. Compute hashes — prepend "sha256-" to the digest.
shasum -a 256 plugins/curses.dylib                      # darwin-arm64
shasum -a 256 plugins/curses-linux-x86_64.so            # etc.

# 3. Tag and create a release with the binaries attached.
git tag v1.1.1 && git push --tags
gh release create v1.1.1 plugins/curses*.{dylib,so} --generate-notes

# 4. Paste the URLs (`gh release view v1.1.1` shows them) and the
#    sha256 lines into grain.yaml under prebuilt:, commit, push.
```

End users on a published platform now run `sand install -g github.com/you/your-grain` with no toolchain at all.

### Override flags

- `sand install --build-from-source <grain>` — skips the `prebuilt:` path entirely. Used when you want to audit or patch the native code locally.
- `sand install --prebuilt-only <grain>` — refuses to install if no `prebuilt:` entry matches your platform, instead of letting you discover the missing toolchain later. Useful for users without `g++`.

### Mismatch behaviour

If a download fails (HTTP error) or the sha256 doesn't match the recorded integrity, sand throws with both expected and actual values and rolls back the install — it never installs an unverified binary. To fix:

- **Download failure** — almost always a wrong URL in `grain.yaml`; double-check the release name and asset filename.
- **Hash mismatch** — either the upstream release was re-uploaded (force-pushed tag) or someone is MITMing your fetch. Recompute the hash from the canonical release and update `grain.yaml`.

## Example

- [`examples/plugins/mathext.cpp`](examples/plugins/mathext.cpp) — math functions (gcd, lcm, fibonacci, hypot, sum) plus `filter` showing the `praia::call` callback pattern
- [`examples/plugins/strutil.cpp`](examples/plugins/strutil.cpp) — wrapping C stdlib functions (isAlpha, isDigit, base conversion)
