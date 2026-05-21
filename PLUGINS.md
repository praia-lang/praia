# Native C++ Plugins

Praia supports loading native C++ modules at runtime via `loadNative()`. This lets you write performance-critical code or wrap C/C++ libraries without modifying Praia itself.

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

**Pitfall — deferred invocation.** Callbacks must be invoked synchronously from the same plugin call that received them. If you stash a callable and invoke it later from a worker thread, a libuv-style scheduler, or any other off-engine context, `praia::call` will throw with "no active Praia executor on this thread". Stack-based VMs and tracing GCs can't safely have user code called on threads they didn't sanction.

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
- **Off-engine threads (e.g. libuv callback, your own `std::thread`).** Both `gcNew` and `praia::call` are unsafe. Marshal back to a Praia-driven thread before touching either.

## Example

- [`examples/plugins/mathext.cpp`](examples/plugins/mathext.cpp) — math functions (gcd, lcm, fibonacci, hypot, sum) plus `filter` showing the `praia::call` callback pattern
- [`examples/plugins/strutil.cpp`](examples/plugins/strutil.cpp) — wrapping C stdlib functions (isAlpha, isDigit, base conversion)
