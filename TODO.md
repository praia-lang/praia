# TODO - Features

Features that are missing or incomplete.

## Not currently planned

These are out of scope for Praia's identity as a scripting/tool-building language:

- **Data science / ML** - numpy, pandas, pytorch, etc. Requires wrapping large C libraries via native plugins. Although entirely possible, this is not planned as built-in features of Praia.
- **Type hints / gradual typing** - No type annotation system. Praia is dynamically typed by design, like Ruby and early JavaScript. Though something like Python's mypy could be created in the future.

## Medium priority

### String builders
Praia's string concatenation in loops is O(n²) because strings are immutable. Hypothetical StringBuilder example:

```praia
let b = str.builder()
for (line in lines) { b.append(line + "\n") }
let result = b.build()  // one allocation
```

Practical for template engines, code generation, large output assembly.

### Shared handler state and the cycle collector
Each thread's collector only reads objects its own thread owns (see Memory Management in DOCUMENTATION.md). One case slips through: a closure that a native runs on another thread — the request handler of `async server.listen`, an `http.sse` callback — shares the variables it captured with the thread that made it, and the maps and arrays behind them stay owned by that thread. Its collector reads them while the handler may be changing them. Closing this needs either stop-the-world collection (every thread parks at a safepoint, and every blocking native marks itself safe) or per-object ownership that moves when a closure starts running on another thread.

Related, outside the collector: closures passed as arguments to an async task get a new wrapper but still share their captured variables with the caller, although the async docs promise tasks a deep copy of captured state.

## Low priority

Reasonable workarounds exist. Would improve the language but aren't blocking:

### Exception hierarchy
Praia throws plain values (strings, maps). There's no `TypeError`, `ValueError`, `IOError` distinction. Workaround: throw maps like `{type: "IOError", message: "file not found"}` and match on the type field in catch blocks.

Having a built-in `Error` class with subclasses (or maybe just a `type` field convention) would make error handling more structured.

### Class features
Partially implemented. Single inheritance, super, operator overloading (__add, __sub, __eq, __lt, __str, __len, __index, __indexSet, etc.), static methods, method decorators. 

Missing: 
- multiple inheritance
- metaclasses
- properties
- abstract base classes
- dataclasses

These are advanced OOP features, need to decide whether to go this direction with the language.

### Introspection / reflection
Only `type()` and `is` exist. No `dir()`, `getattr()`, `hasattr()`, `inspect`. Can't enumerate an object's methods or fields at runtime. Would be useful for debugging tools and serialization frameworks.

### Tuple type (immutable arrays)
No immutable sequence type. Arrays are always mutable. Workaround: just don't mutate them (convention). A frozen/immutable array could be useful for map keys and function return values that shouldn't be modified.

### Slice syntax
No `arr[1:3]` or `arr[::2]` syntax. The `.slice(start, end)` method works but is more verbose. Slice syntax would require parser changes for the `[start:end:step]` form inside index expressions.
