# Native C++ Plugins

Praia supports loading native C++ modules at runtime via `loadNative()`. This lets you write performance-critical code or wrap C/C++ libraries without modifying Praia itself.

## Quick start

**1. Write a plugin** (`mymodule.cpp`):

```cpp
#include "praia_plugin.h"

extern "C" void praia_register(PraiaMap* module) {
    module->entries["double"] = Value(makeNative("mymodule.double", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("expected a number", 0);
            return Value(args[0].asNumber() * 2);
        }));
}
```

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

extern "C" void praia_register(PraiaMap* module) {
    module->entries["parseBase"] = Value(makeNative("mymod.parseBase", 2,
        [](const std::vector<Value>& args) -> Value {
            auto& str = args[0].asString();
            int base = static_cast<int>(args[1].asInt());
            return Value(static_cast<int64_t>(strtoll(str.c_str(), nullptr, base)));
        }));
}
```

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
