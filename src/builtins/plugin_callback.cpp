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

#include "../interpreter.h"   // g_currentInterp
#include "../builtins.h"      // callSafe
#include "../vm/vm.h"         // VM::current, callWithVM

#include <cstdint>

namespace praia {

namespace {
constexpr uintptr_t kTagBit = 1;
constexpr uintptr_t kTagVm  = 1;
}

void* currentExecutor() {
    if (VM* vm = VM::current()) {
        return reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(vm) | kTagVm);
    }
    if (g_currentInterp) {
        // Tag bit already clear — interpreter pointers stay as-is.
        return reinterpret_cast<void*>(g_currentInterp);
    }
    return nullptr;
}

Value invokeExecutor(void* exec,
                     const std::shared_ptr<Callable>& fn,
                     const std::vector<Value>& args) {
    if (!exec) {
        throw RuntimeError(
            "praia::call invoked with no active Praia executor on this thread", 0);
    }
    auto raw = reinterpret_cast<uintptr_t>(exec);
    if ((raw & kTagBit) == kTagVm) {
        auto* vm = reinterpret_cast<VM*>(raw & ~kTagBit);
        return callWithVM(*vm, fn, args);
    }
    auto* interp = reinterpret_cast<Interpreter*>(raw);
    return callSafe(*interp, fn, args);
}

}  // namespace praia
