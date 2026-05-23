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

// Schedule `fn(args)` to run on the engine identified by `exec` at
// its next yield point (between statements for the tree-walker;
// between throttled GC/SIGINT checks for the bytecode VM). Safe to
// call from any thread, including threads with no active Praia
// executor — that's the whole point. Returns immediately;
// fire-and-forget.
//
// `exec` must be a token previously captured via currentExecutor()
// while running on the target engine. Pass it across threads as-is.
//
// Lifetime — engine. The caller is responsible for ensuring `exec`'s
// engine outlives any pending posts. In practice this means the
// plugin must guarantee the engine that loaded it isn't torn down
// with worker threads still queued. The deferred call's return
// value and exceptions are not observable from the caller — for
// results, arrange your own signaling (e.g. a Praia Queue captured
// in the callback's closure).
//
// Lifetime — captured Values. The `fn` callable and any GC-tracked
// Values inside `args` are NOT rooted by the engine while they
// travel from the worker through to drainPosted. The engine's
// gcMarkRoots (Interpreter::gcMarkRoots / VM::gcMarkRoots) walks
// globals + the current env + savedEnvStack_ + grainCache + the
// pinned_ registry — it does NOT walk postedQueue_, and it cannot
// see the worker thread's std::thread lambda captures. A sweep
// fired between the worker capturing the Value and drainPosted
// running it would clear those Values' interiors (env variables on
// a callable's closure, entries on a map, etc.) even though the
// shared_ptr keeps the address valid.
//
// The caller MUST pin every GC-tracked Value (the callable plus any
// trailing args) with praia::pinValue BEFORE handing them off to
// the worker, and the pins MUST remain in place until the engine
// has finished running the deferred callback (i.e. until
// drainPosted has invoked it and returned). The matching
// praia::unpinValue calls MUST run on the engine thread —
// pinValue/unpinValue throw on threads without an active executor,
// so the worker can't release them itself.
//
// The canonical pattern (see examples/plugins/counter.cpp ::
// scheduleAsync) is to post a small NativeFunction wrapper that
// the engine's drainPosted invokes; the wrapper calls the user
// callback and then calls praia::unpinValue for each previously
// pinned Value (in a try/catch so an exception from the user
// callback doesn't leak pins). That keeps every pinValue paired
// with an unpinValue without burdening the user-supplied callback.
//
// Non-GC values (numbers, strings, bools, nil) are unaffected; they
// have no interior the sweep could clear. pinValue/unpinValue on
// them are no-ops, so the wrapper's cleanup loop can stay uniform.
void postToEngine(void* exec,
                  std::shared_ptr<Callable> fn,
                  std::vector<Value> args);

}  // namespace praia
