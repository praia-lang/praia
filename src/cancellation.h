// Cooperative cancellation — shared state + per-thread current scope.
//
// `CancellationState` is the inner atomic flag a `CancellationToken`
// wraps (the user-facing PraiaMap with cancel/cancelled/throwIfCancelled
// methods is in src/builtins/concurrency.cpp). It's pulled out into
// its own header so the plugin runtime shim can read the same flag
// the user-visible methods read, without duplicating it.
//
// `g_currentCancel` is the thread-local that Praia's `withCancel(token,
// fn)` native sets while it runs `fn`. Plugin code reads it through
// the `praia::shouldCancel()` ABI declared in praia_runtime.h. The
// thread-local is unset by default — outside a `withCancel` scope,
// `shouldCancel()` returns `std::nullopt`.
//
// Threading. The flag itself is atomic; the thread-local pointer is
// per-thread by definition. A background worker thread spawned by a
// plugin (e.g. counter.scheduleAsync) starts with the thread-local
// clear; if it wants to honour cancellation, it must read the state
// pointer on the engine thread and pass it into the worker's scope
// explicitly. This is intentional: a worker thread that inherited an
// engine-thread token would block its own cleanup if the engine
// thread cancelled the token and then waited on the worker.

#pragma once

#include <atomic>

namespace praia { namespace detail {

struct CancellationState {
    std::atomic<bool> cancelled{false};
};

// Thread-local "current cancellation scope" pointer. Null when no
// `withCancel(token, fn)` scope is active on this thread. Lifetime
// of the pointed-to state is guaranteed by the closures captured in
// the CancellationToken map: as long as the token is reachable from
// Praia globals/locals, its shared_ptr<CancellationState> keeps the
// state alive. `withCancel` only needs to publish the raw pointer
// for the duration of its callee.
extern thread_local CancellationState* g_currentCancel;

// RAII guard for `withCancel`. Sets `g_currentCancel` to `s` on
// construction, restores the previous value on destruction (so
// nested withCancel scopes nest correctly, and a throw from the
// callee leaves the thread-local clean).
struct CancelScope {
    CancellationState* prev;
    explicit CancelScope(CancellationState* s) : prev(g_currentCancel) {
        g_currentCancel = s;
    }
    ~CancelScope() { g_currentCancel = prev; }
    CancelScope(const CancelScope&) = delete;
    CancelScope& operator=(const CancelScope&) = delete;
};

}}  // namespace praia::detail
