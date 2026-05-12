// Fuzz-only stub of GcHeap.
//
// The real src/gc_heap.cpp markCallable() uses dynamic_cast against half
// a dozen callable subtypes (PraiaFunction, PraiaLambda, PraiaMethod,
// VMClosureCallable, etc.), pulling their typeinfo as undefined symbols
// — which means linking pulls in interpreter.cpp, vm.cpp, and friends.
// For fuzz binaries that only need to allocate Praia heap objects
// (PraiaMap/Array via gcNew during parsing) and free them per iteration,
// none of that traversal logic is needed.
//
// This stub provides the same API but skips the mark phase entirely.
// `collect()` falls through to phase 4 of the real sweep — pruning
// expired weak_ptrs — which is all the fuzz target needs to keep
// entries_ bounded across iterations. The Praia heap objects are
// dropped by refcount when the resulting Value goes out of scope at
// the end of LLVMFuzzerTestOneInput; we don't need cycle-breaking
// because no fuzz iteration builds cycles that outlive that scope.

#include "../src/gc_heap.h"
#include "../src/value.h"
#include "../src/environment.h"
#include <algorithm>

GcHeap& GcHeap::current() {
    static thread_local GcHeap instance;
    return instance;
}

void GcHeap::track(const std::shared_ptr<PraiaArray>& p) {
    entries_.push_back({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Array});
    allocsSinceGc_++;
}
void GcHeap::track(const std::shared_ptr<PraiaMap>& p) {
    entries_.push_back({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Map});
    allocsSinceGc_++;
}
void GcHeap::track(const std::shared_ptr<PraiaInstance>& p) {
    entries_.push_back({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Instance});
    allocsSinceGc_++;
}
void GcHeap::track(const std::shared_ptr<PraiaClass>& p) {
    entries_.push_back({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Class});
    allocsSinceGc_++;
}
void GcHeap::track(const std::shared_ptr<PraiaGenerator>& p) {
    entries_.push_back({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Generator});
    allocsSinceGc_++;
}
void GcHeap::track(const std::shared_ptr<PraiaTagged>& p) {
    entries_.push_back({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Tagged});
    allocsSinceGc_++;
}
void GcHeap::track(const std::shared_ptr<Environment>& p) {
    entries_.push_back({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Environment});
    allocsSinceGc_++;
}

void GcHeap::setRootMarker(RootMarker marker) { rootMarker_ = std::move(marker); }
void GcHeap::disable() { disabled_ = true; }

void GcHeap::collectIfNeeded() {
    if (disabled_ || allocsSinceGc_ < threshold_) return;
    collect();
}

// Simplified: just prune expired weak_ptrs. No mark, no cycle-breaking.
// `lastCollected_` and threshold auto-tuning aren't maintained here —
// fuzz targets call collect() directly on a fixed cadence, not through
// collectIfNeeded(), so the tuning would have no effect anyway.
void GcHeap::collect() {
    std::erase_if(entries_, [](const GcEntry& e) { return e.weak.expired(); });
    allocsSinceGc_ = 0;
}

// Mark functions kept as no-ops to satisfy the class declaration.
void GcHeap::mark() {}
void GcHeap::sweep() {}
void GcHeap::markValue(const Value&) {}
void GcHeap::markEnvironment(Environment*) {}
void GcHeap::markArray(PraiaArray*) {}
void GcHeap::markTagged(PraiaTagged*) {}
void GcHeap::markMap(PraiaMap*) {}
void GcHeap::markInstance(PraiaInstance*) {}
void GcHeap::markClass(PraiaClass*) {}
void GcHeap::markGenerator(PraiaGenerator*) {}
void GcHeap::markCallable(Callable*) {}

// Definition for the thread-local declared in interpreter.h. NativeFunction::call
// (inline in the header) references it via TLS, so any TU that uses a native —
// including json.cpp through its callable bindings — needs this symbol at link
// time. The real definition lives in src/interpreter.cpp, which fuzz binaries
// don't link.
class Interpreter;
thread_local Interpreter* g_currentInterp = nullptr;
