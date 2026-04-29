# TODO - Features

Features that are missing or incomplete.

## Not currently planned

These are out of scope for Praia's identity as a scripting/tool-building language:

- **Data science / ML** - numpy, pandas, pytorch, etc. Requires wrapping large C libraries via native plugins. Although entirely possible, this is not planned as built-in features of Praia.
- **Type hints / gradual typing** - No type annotation system. Praia is dynamically typed by design, like Ruby and early JavaScript. Though something like Python's mypy could be created in the future.

## Low priority

Reasonable workarounds exist. Would improve the language but aren't blocking:

### Exception hierarchy
Praia throws plain values (strings, maps). There's no `TypeError`, `ValueError`, `IOError` distinction. Workaround: throw maps like `{type: "IOError", message: "file not found"}` and match on the type field in catch blocks.

Having a built-in `Error` class with subclasses (or maybe just a `type` field convention) would make error handling more structured.

### Introspection / reflection
Only `type()` and `is` exist. No `dir()`, `getattr()`, `hasattr()`, `inspect`. Can't enumerate an object's methods or fields at runtime. Would be useful for debugging tools and serialization frameworks.

### Tuple type (immutable arrays)
No immutable sequence type. Arrays are always mutable. Workaround: just don't mutate them (convention). A frozen/immutable array could be useful for map keys and function return values that shouldn't be modified.

### Set type
No dedicated hash set. Workarounds:
- `arr |> unique` for deduplication
- Map with dummy values (`{[item]: true}`) for O(1) membership testing
- `.has()` on maps works as a set membership check

A native `Set` type with `.add()`, `.has()`, `.delete()`, `.union()`, `.intersection()` would be cleaner.

### Slice syntax
No `arr[1:3]` or `arr[::2]` syntax. The `.slice(start, end)` method works but is more verbose. Slice syntax would require parser changes for the `[start:end:step]` form inside index expressions.
