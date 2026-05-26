# rcmod - Praia plugin in Rust

A small Praia plugin written in pure Rust against the C facade declared in [`src/praia_plugin_c.h`](../../../src/praia_plugin_c.h). The same plugin in C lives at [`examples/plugins/cmod.c`](../cmod.c).

This example shows the binding pattern for any language with a C FFI - the same approach works for Zig, Go (via cgo), Swift, etc.

## What it does

- `rcmod.greet(name: string) -> string` - returns `"hello, <name>"`
- `rcmod.add(a: int, b: int) -> int` - sum (with arg validation)
- `rcmod.boom() -> never` - throws a `RuntimeError` so you can see the error-reporting path

## Build

```sh
cd examples/plugins/rust-cmod
cargo build --release
```

Output:

- **macOS:** `target/release/librcmod.dylib`
- **Linux:** `target/release/librcmod.so`

The `.cargo/config.toml` here passes `-undefined dynamic_lookup` on macOS so the linker is OK with the unresolved `praia_*` symbols (they'll resolve at dlopen time against the engine binary). Linux needs no special flags - `cdylib` + the engine's `-rdynamic` are enough.

## Run

`loadNative` looks for `<basename>.dylib` or `<basename>.so`, so symlink the built artifact to a plain name first:

```sh
# macOS
ln -sf "$(pwd)/target/release/librcmod.dylib" ./rcmod.dylib

# Linux
ln -sf "$(pwd)/target/release/librcmod.so"    ./rcmod.so

praia -c '
let m = loadNative("./rcmod")
print(m._meta)             // {name: "rcmod", version: "0.1.0", description: ...}
print(m.greet("rust"))     // "hello, rust"
print(m.add(2, 3))         // 5
try { m.boom() } catch (e) { print("caught:", e) }
'
```

## Notes

- The `extern "C"` declarations at the top of `src/lib.rs` are hand-rolled for clarity - a real-world plugin would generate them with [`bindgen`](https://docs.rs/bindgen/) from `praia_plugin_c.h`, which gives you the full surface in one shot and stays in sync as the facade evolves.
- `praia_abi_version()` returns `5` to match `PRAIA_PLUGIN_ABI_VERSION` in the facade header. When that number bumps, every plugin (C, C++, Rust, …) needs a rebuild against the new headers.
- See `examples/plugins/cmod.c` for additional patterns covered there but not duplicated here: external handles with deleters, map construction.
