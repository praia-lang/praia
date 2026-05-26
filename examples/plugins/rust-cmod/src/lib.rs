//! Praia plugin written in Rust, using the C facade in
//! `src/praia_plugin_c.h`. Demonstrates the binding pattern for
//! any language with a C FFI - see also the C version of the same
//! plugin at `examples/plugins/cmod.c`.
//!
//! The pattern is:
//!   1. Declare `extern "C"` bindings for the subset of the facade
//!      this plugin uses. (A real-world plugin would use `bindgen`
//!      against praia_plugin_c.h; we hand-roll a minimal subset here
//!      so the binding shape is visible.)
//!   2. Implement each native as an `extern "C" fn` taking a borrowed
//!      `PraiaArgs` and returning an owned `PraiaValue` (or NULL
//!      after calling `praia_throw`).
//!   3. Export `praia_register`, `praia_abi_version`, and the
//!      metadata symbols with `#[no_mangle]`.
//!
//! Build (from this directory):
//!     cargo build --release
//!     # macOS: produces target/release/librcmod.dylib
//!     # Linux: produces target/release/librcmod.so
//!
//! Use:
//!     ln -sf $(pwd)/target/release/librcmod.dylib ./rcmod.dylib
//!     praia -c 'let m = loadNative("./rcmod"); print(m.greet("rust"))'

#![allow(non_camel_case_types)]

use std::ffi::{CString, c_char, c_int, c_void};
use std::ptr;

// ── C facade bindings ────────────────────────────────────────────
//
// Opaque pointer types. Rust doesn't see the contents - every op
// goes through one of the extern functions below.

#[repr(C)] pub struct PraiaValueOpaque(());
#[repr(C)] pub struct PraiaArgsOpaque(());
#[repr(C)] pub struct PraiaMapOpaque(());

pub type PraiaValue     = *mut PraiaValueOpaque;
pub type PraiaArgs      = *mut PraiaArgsOpaque;
pub type PraiaMapHandle = PraiaMapOpaque;

pub type PraiaNativeFn  = extern "C" fn(args: PraiaArgs, userdata: *mut c_void) -> PraiaValue;

// The facade lives in the praia binary. Cargo links these as
// undefined references; the linker flag in .cargo/config.toml
// (macOS) plus the engine's -rdynamic (Linux) make them resolve
// at dlopen time.
extern "C" {
    // Value lifecycle
    fn praia_value_release(v: PraiaValue);

    // Constructors
    fn praia_value_int(i: i64) -> PraiaValue;
    fn praia_value_string_n(s: *const c_char, len: usize) -> PraiaValue;

    // Predicates + accessors
    fn praia_value_is_int(v: PraiaValue) -> bool;
    fn praia_value_is_string(v: PraiaValue) -> bool;
    fn praia_value_as_int(v: PraiaValue) -> i64;
    fn praia_value_as_string(v: PraiaValue, out_len: *mut usize) -> *const c_char;

    // Native callback args
    fn praia_args_count(args: PraiaArgs) -> c_int;
    fn praia_args_get(args: PraiaArgs, i: c_int) -> PraiaValue;

    // Registration
    fn praia_make_native(name: *const c_char, arity: c_int,
                         fn_: PraiaNativeFn, userdata: *mut c_void) -> PraiaValue;
    fn praia_module_set(module: *mut PraiaMapHandle, key: *const c_char, value: PraiaValue);

    // Error reporting
    fn praia_throw(msg: *const c_char);
}

// ── Helpers ──────────────────────────────────────────────────────

/// Stage a Praia error and return NULL. Used by every fallible
/// native so the bail path stays a one-liner at the call site.
fn fail(msg: &str) -> PraiaValue {
    // The CString outlives praia_throw's read because the facade
    // copies the message into thread-local storage before returning.
    if let Ok(c) = CString::new(msg) {
        unsafe { praia_throw(c.as_ptr()) }
    }
    ptr::null_mut()
}

/// Borrow the byte view of a Praia string Value without copying.
/// Returns None if `v` isn't a string. The slice's lifetime is tied
/// to `v`'s - fine for the duration of a native call.
unsafe fn as_str_bytes<'a>(v: PraiaValue) -> Option<&'a [u8]> {
    if !praia_value_is_string(v) { return None; }
    let mut len: usize = 0;
    let ptr = praia_value_as_string(v, &mut len);
    if ptr.is_null() { return None; }
    Some(std::slice::from_raw_parts(ptr as *const u8, len))
}

// ── Natives ──────────────────────────────────────────────────────

extern "C" fn rcmod_greet(args: PraiaArgs, _ud: *mut c_void) -> PraiaValue {
    unsafe {
        if praia_args_count(args) != 1 {
            return fail("rcmod.greet: expected 1 argument");
        }
        let name = praia_args_get(args, 0);
        let bytes = match as_str_bytes(name) {
            Some(b) => b,
            None => return fail("rcmod.greet: expected a string"),
        };
        // Format via Rust's own string handling, then hand the
        // bytes back through the facade. The greeting buffer is
        // dropped at the end of this scope - fine because
        // praia_value_string_n copies the bytes into a new Value.
        let greeting = format!("hello, {}", String::from_utf8_lossy(bytes));
        praia_value_string_n(greeting.as_ptr() as *const c_char, greeting.len())
    }
}

extern "C" fn rcmod_add(args: PraiaArgs, _ud: *mut c_void) -> PraiaValue {
    unsafe {
        if praia_args_count(args) != 2 {
            return fail("rcmod.add: expected 2 arguments");
        }
        let a = praia_args_get(args, 0);
        let b = praia_args_get(args, 1);
        if !praia_value_is_int(a) || !praia_value_is_int(b) {
            return fail("rcmod.add: both arguments must be ints");
        }
        let sum = praia_value_as_int(a).wrapping_add(praia_value_as_int(b));
        praia_value_int(sum)
    }
}

extern "C" fn rcmod_boom(_args: PraiaArgs, _ud: *mut c_void) -> PraiaValue {
    fail("rcmod.boom: this is a deliberate error")
}

// ── Required exports ─────────────────────────────────────────────
//
// `#[no_mangle]` + `extern "C"` so the symbols match exactly what
// the engine's loadNative resolves via dlsym. The names are part
// of the plugin ABI contract documented in praia_plugin_c.h.

#[no_mangle]
pub extern "C" fn praia_abi_version() -> c_int {
    5  // matches PRAIA_PLUGIN_ABI_VERSION in praia_plugin_c.h
}

#[no_mangle]
pub extern "C" fn praia_plugin_name() -> *const c_char {
    // Static NUL-terminated bytes; Rust string literals don't include
    // the terminator, so we splice "\0" on the source side.
    b"rcmod\0".as_ptr() as *const c_char
}

#[no_mangle]
pub extern "C" fn praia_plugin_version() -> *const c_char {
    b"0.1.0\0".as_ptr() as *const c_char
}

#[no_mangle]
pub extern "C" fn praia_plugin_description() -> *const c_char {
    b"Pure-Rust plugin demo for the praia_plugin_c.h facade\0".as_ptr() as *const c_char
}

#[no_mangle]
pub extern "C" fn praia_register(module: *mut PraiaMapHandle) {
    // Small helper to compress "make + set + release" per native.
    // The CStrings are constructed inside the loop so each one
    // lives just long enough for the unsafe calls below.
    unsafe fn reg(module: *mut PraiaMapHandle, name: &str,
                  key: &str, arity: c_int, fn_: PraiaNativeFn) {
        let cname = CString::new(name).unwrap();
        let ckey  = CString::new(key).unwrap();
        let v = praia_make_native(cname.as_ptr(), arity, fn_, ptr::null_mut());
        praia_module_set(module, ckey.as_ptr(), v);
        praia_value_release(v);
    }

    unsafe {
        reg(module, "rcmod.greet", "greet", 1, rcmod_greet);
        reg(module, "rcmod.add",   "add",   2, rcmod_add);
        reg(module, "rcmod.boom",  "boom",  0, rcmod_boom);
    }
}
