// Praia Plugin Runtime ABI
//
// Stable, narrow surface that lets plugin code invoke Praia
// callbacks (Callables passed in as arguments) without depending
// on either the tree-walker Interpreter or the bytecode VM
// internals.
//
// The two primitives below are versioned by contract — their
// signatures are frozen across Praia releases. Internal types
// (Interpreter, VM, ObjClosure, etc.) can evolve freely; only
// this pair, plus the data types in value.h, comprise the plugin
// ABI.
//
// Plugin authors normally don't call these directly — use the
// `praia::call` inline helper declared in praia_plugin.h. They're
// public only so a plugin that wants zero-overhead bindings (e.g.
// in a hot loop) can skip the wrapper if it prefers.
//
// Engine selection. Praia ships two engines; either may be the
// one driving the current call stack. `currentExecutor()` returns
// an opaque token identifying whichever is active on the calling
// thread (with a tag bit so the receiver can untag without
// leaking the underlying type). `invokeExecutor()` consumes the
// token and dispatches to the matching invocation helper.
//
// Threading. Both engines stash their "current" pointer in a
// thread-local set by an RAII scope around the active call
// frame. Off-thread callbacks (worker thread, libuv-style
// scheduler, network callback) will see `currentExecutor`
// return null and `invokeExecutor` throw. This is intentional:
// invoking user Praia code on a thread the engine didn't sanction
// races against the GC and the stack-based VM. Plugins that need
// deferred invocation must marshal back to the engine thread
// before calling.

#pragma once

#include "value.h"  // Value, Callable, RuntimeError

#include <memory>
#include <vector>

namespace praia {

// Returns an opaque token for the engine currently driving this
// thread, or nullptr if no Praia executor is active. The token's
// low bit distinguishes which engine produced it; the high bits
// store the engine pointer. Pass the token verbatim to
// invokeExecutor — don't try to inspect or dereference it.
void* currentExecutor();

// Invokes `fn` with `args` through the engine identified by
// `exec`. Throws RuntimeError if `exec` is null. Argument-count
// validation is performed by the underlying invocation helper
// (so error messages match what user Praia code sees from a
// mistyped call).
Value invokeExecutor(void* exec,
                     const std::shared_ptr<Callable>& fn,
                     const std::vector<Value>& args);

}  // namespace praia
