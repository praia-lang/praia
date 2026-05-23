// Plugin runtime — bridges the two callback paths (tree-walker
// vs bytecode VM) behind a single opaque token so plugin code
// doesn't have to depend on either engine's headers.
//
// Token layout: a tagged pointer where the low bit selects which
// engine. Bit set ⇒ VM*; bit clear ⇒ Interpreter*. nullptr means
// no executor is active on this thread. The tag fits because both
// engine types are heap-allocated and trivially aligned to at
// least 2 bytes (in practice >= 8) — bit 0 is always 0 in a real
// pointer, so we can repurpose it for the kind discriminator.

#include "../praia_runtime.h"

#include "../cancellation.h"  // detail::g_currentCancel for shouldCancel
#include "../interpreter.h"   // g_currentInterp
#include "../builtins.h"      // callSafe
#include "../vm/vm.h"         // VM::current, callWithVM

#include <atomic>             // Promise::State fulfilled flag
#include <cassert>
#include <cstdint>
#include <exception>          // std::make_exception_ptr in Promise::reject
#include <future>             // std::promise / std::shared_future in Promise::State

namespace praia {

namespace {
constexpr uintptr_t kTagBit = 1;
constexpr uintptr_t kTagVm  = 1;
}

void* currentExecutor() {
    if (VM* vm = VM::current()) {
        // The tag-bit trick requires both engine types to be at
        // least 2-aligned so bit 0 is always clear in a real
        // pointer. Both VM and Interpreter have vtables, so
        // alignof >= sizeof(void*) >= 4 on every platform Praia
        // targets — assert to catch any future struct redesign
        // that would break this assumption.
        assert((reinterpret_cast<uintptr_t>(vm) & kTagBit) == 0);
        return reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(vm) | kTagVm);
    }
    if (g_currentInterp) {
        assert((reinterpret_cast<uintptr_t>(g_currentInterp) & kTagBit) == 0);
        // Tag bit already clear — interpreter pointers stay as-is.
        return reinterpret_cast<void*>(g_currentInterp);
    }
    return nullptr;
}

Value invokeExecutor(void* exec,
                     const std::shared_ptr<Callable>& fn,
                     const std::vector<Value>& args) {
    if (!exec) {
        // Different wording from the praia::call inline wrapper so
        // stack traces distinguish "wrapper saw no executor on this
        // thread" from "caller bypassed the wrapper and passed a
        // null token here". Bypassing the wrapper is unusual — it
        // happens only when a plugin checks currentExecutor()
        // explicitly and then calls invokeExecutor() directly.
        throw RuntimeError(
            "praia::invokeExecutor called with a null executor token", 0);
    }
    auto raw = reinterpret_cast<uintptr_t>(exec);
    if ((raw & kTagBit) == kTagVm) {
        auto* vm = reinterpret_cast<VM*>(raw & ~kTagBit);
        return callWithVM(*vm, fn, args);
    }
    auto* interp = reinterpret_cast<Interpreter*>(raw);
    return callSafe(*interp, fn, args);
}

std::optional<bool> shouldCancel() {
    // Thread-local read; no executor needed. Returning nullopt outside
    // a withCancel scope (rather than `false`) lets plugin code
    // distinguish "no cancellation in this context, run as long as
    // you want" from "cancellation in scope and not yet triggered" —
    // useful for natives that want different default behaviour in
    // each case (e.g. log a warning when no token is bound but the
    // operation is known to be cancellable).
    auto* state = detail::g_currentCancel;
    if (!state) return std::nullopt;
    return state->cancelled.load(std::memory_order_acquire);
}

void postToEngine(void* exec,
                  std::shared_ptr<Callable> fn,
                  std::vector<Value> args) {
    if (!exec) {
        // Background threads typically obtain `exec` by capturing
        // currentExecutor() on the engine thread before spawning. A
        // null token here means the caller passed the result of a
        // failed currentExecutor() call — surface that loudly rather
        // than silently dropping the post.
        throw RuntimeError(
            "praia::postToEngine called with a null executor token", 0);
    }
    auto raw = reinterpret_cast<uintptr_t>(exec);
    if ((raw & kTagBit) == kTagVm) {
        auto* vm = reinterpret_cast<VM*>(raw & ~kTagBit);
        vm->enqueuePosted(std::move(fn), std::move(args));
    } else {
        auto* interp = reinterpret_cast<Interpreter*>(raw);
        interp->enqueuePosted(std::move(fn), std::move(args));
    }
}

// ── Promise ───────────────────────────────────────────────────────
//
// State is the only thing the public class hides; everything else
// (resolve, reject, future) is a thin forwarder. Lifetime: each
// Promise instance holds a shared_ptr<State>; when the last one
// drops, ~State runs and — if no resolve/reject has run — sets a
// "dropped" RuntimeError on the linked future so awaiters don't
// block forever.
//
// Synchronisation: a single std::atomic<bool> CAS in resolve /
// reject / ~State decides who gets to call std::promise::set_value
// or set_exception. The std::promise itself is internally
// thread-safe between set and the future's get, so once the CAS
// has selected a winner there's no further coordination needed.
struct Promise::State {
    std::promise<Value>           promise;
    std::shared_ptr<PraiaFuture>  futureWrapper;
    std::atomic<bool>             fulfilled{false};

    State() {
        // Mirrors how AsyncExpr builds its Future
        // (src/interpreter.cpp ~line 1617) — share() so multiple
        // awaits work, wrap in PraiaFuture so Value accepts it.
        // The shared_future lives only inside futureWrapper; we
        // don't need a separate field on State — `promise` is what
        // we call set_value / set_exception on, and the wrapper
        // carries the reader side.
        futureWrapper = std::make_shared<PraiaFuture>();
        futureWrapper->future = promise.get_future().share();
    }

    ~State() {
        // Plant a clear "dropped" RuntimeError if no producer
        // resolved/rejected. Without this, std::promise's destructor
        // would set future_error{broken_promise} and `await` would
        // surface it as the generic "Async task failed:
        // <implementation message>". Praia's `await` rethrows
        // RuntimeError unchanged (interpreter.cpp:1633 /
        // vm.cpp:2685), so a RuntimeError here gives the user the
        // exact message below.
        if (!fulfilled.exchange(true)) {
            try {
                promise.set_exception(std::make_exception_ptr(
                    RuntimeError("plugin Promise was dropped before "
                                 "resolve or reject", 0)));
            } catch (...) {
                // set_exception can throw if no shared state exists
                // (e.g. get_future already had its result consumed
                // in some race). Swallow — destructors mustn't throw.
            }
        }
    }
};

Promise::Promise() : state_(std::make_shared<State>()) {}

Value Promise::future() const {
    return Value(state_->futureWrapper);
}

void Promise::resolve(Value result) noexcept {
    // CAS first so a losing thread can't double-fulfil; set_value
    // would throw future_error{promise_already_satisfied}. The
    // try/catch is belt-and-suspenders for any other implementation
    // weirdness (memory exhaustion etc.) — noexcept means an escape
    // would terminate.
    if (state_->fulfilled.exchange(true)) return;
    try {
        state_->promise.set_value(std::move(result));
    } catch (...) {}
}

void Promise::reject(const std::string& message) noexcept {
    if (state_->fulfilled.exchange(true)) return;
    try {
        state_->promise.set_exception(std::make_exception_ptr(
            RuntimeError(message, 0)));
    } catch (...) {}
}

}  // namespace praia
