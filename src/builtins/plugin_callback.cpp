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

#include <cassert>
#include <cstdint>

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

}  // namespace praia
