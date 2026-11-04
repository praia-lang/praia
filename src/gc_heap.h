#pragma once

#include "value.h"
#include <functional>
#include <unordered_set>
#include <vector>

class Environment; // forward declaration
struct TaskOwnership;

enum class GcType { Array, Tagged, Map, Set, Instance, Class, Generator, Environment, External };

// Node kinds in the object graph the collector walks. Superset of
// GcType: Callables (functions, lambdas, bound methods), VM upvalues and
// async-task ownership bags aren't tracked themselves but sit on paths
// between tracked objects, so the external-reference scan has to see
// through them.
enum class GcNodeKind : uint8_t {
    Array, Tagged, Map, Set, Instance, Class, Generator, Environment, External, Callable,
    Upvalue, TaskOwnership
};

class GcHeap {
public:
    static GcHeap& current();

    // Track container objects (called by gcNew)
    void track(const std::shared_ptr<PraiaArray>& p);
    void track(const std::shared_ptr<PraiaMap>& p);
    void track(const std::shared_ptr<PraiaSet>& p);
    void track(const std::shared_ptr<PraiaInstance>& p);
    void track(const std::shared_ptr<PraiaClass>& p);
    void track(const std::shared_ptr<PraiaGenerator>& p);
    void track(const std::shared_ptr<PraiaTagged>& p);
    void track(const std::shared_ptr<PraiaExternal>& p);
    void track(const std::shared_ptr<Environment>& p);

    // Mark API (called during root marking by VM/Interpreter)
    void markValue(const Value& v);
    void markEnvironment(Environment* env);

    // GC root pinning for plugin-held Values. A native plugin that
    // stashes a Value in C++ static / global storage between calls
    // (callback table, cache, singleton) MUST pin it — the value
    // isn't reachable from the interpreter's roots and would be
    // hollowed out by the next sweep (the shared_ptr stays valid,
    // but the underlying object's internal references get cleared).
    //
    // Reference-counted by multiplicity: pinning the same value N
    // times requires N unpins to release. Last-in-first-out matching
    // (unpin removes the most recently inserted match) keeps RAII
    // pin/unpin pairs nesting cleanly.
    //
    // Non-GC-tracked values (numbers, strings, bools, nil) are
    // accepted but never need pinning; storing them in the registry
    // is a harmless no-op as far as the sweep is concerned.
    void pinValue(const Value& v);
    void unpinValue(const Value& v);

    // A finished async task's ownership bag. Its closures can capture
    // each other through the bag's own upvalues (bag -> upvalue ->
    // closure -> bag), a cycle with no tracked object on it, so heaps
    // find these bags through a process-wide registry instead.
    static void registerTaskBag(const std::shared_ptr<TaskOwnership>& bag);

    // Collection control
    using RootMarker = std::function<void(GcHeap&)>;
    void setRootMarker(RootMarker marker);
    void collectIfNeeded();
    void collect();
    void disable();

    int trackedCount() const { return static_cast<int>(entries_.size()); }
    int collected() const { return lastCollected_; }

    // Stress mode (PRAIA_GC_STRESS=N): collect after every N
    // allocations instead of the auto-tuned threshold, and the VM
    // checks for that at every instruction instead of every 1024.
    // Slow — meant for running the test suite to flush out objects
    // the collector can't see.
    bool stressMode() const { return stress_; }

private:
    struct GcEntry {
        std::weak_ptr<void> weak;
        void* rawPtr;
        GcType type;
    };

    // Per-collection bookkeeping keyed by object pointer: the external
    // reference count from findExternallyHeld() and the mark bit.
    // Open addressing over one flat array, kept between collections so
    // a large heap doesn't pay a node allocation per object per cycle.
    class NodeTable {
    public:
        struct Slot {
            void* key = nullptr;
            long refs = 0;
            GcNodeKind kind = GcNodeKind::External;
            bool marked = false;
        };
        // Empties the table, sized for about `expected` entries.
        void reset(size_t expected);
        // Returns the slot for key, inserting {kind, refs} if absent.
        // The pointer is only valid until the next insert.
        std::pair<Slot*, bool> insert(void* key, GcNodeKind kind, long refs);
        Slot* find(void* key);
        template <typename F> void forEach(F&& f) {
            for (auto& s : slots_) if (s.key) f(s);
        }
    private:
        std::vector<Slot> slots_;
        size_t size_ = 0;
        size_t mask_ = 0;
        size_t indexFor(void* key) const;
        void grow();
    };

    std::vector<GcEntry> entries_;
    NodeTable nodes_;
    // Strong references to every tracked object, taken for the duration
    // of a collection (see findExternallyHeld).
    std::vector<std::shared_ptr<void>> held_;
    // Registered task bags taken for the duration of a collection, and
    // whether this heap holds the registry's collection lock.
    std::vector<std::shared_ptr<TaskOwnership>> bags_;
    bool holdsBagLock_ = false;
    // Plugin-pinned values — see pinValue/unpinValue. Marked alongside
    // the interpreter's own roots on every collection.
    std::vector<Value> pinned_;
    RootMarker rootMarker_;
    int allocsSinceGc_ = 0;
    int lastCollected_ = 0;
    int threshold_ = 500;
    bool collecting_ = false;
    bool disabled_ = false;
    bool stress_ = false;
    int stressInterval_ = 1;

    struct MarkItem {
        void* obj;
        GcNodeKind kind;
    };
    // Explicit worklist so marking deep structures (long linked lists)
    // doesn't recurse on the C++ stack.
    std::vector<MarkItem> markStack_;

    GcHeap();

    void mark();
    void sweep();

    // Tracked objects whose shared_ptr count exceeds the references the
    // collector can see from other heap objects — something outside the
    // heap (a C++ local in a native, a native closure capture, another
    // thread) still holds them, so they're roots.
    std::vector<MarkItem> findExternallyHeld();

    // Calls f(obj, kind, useCount, owning) for each outgoing edge of a
    // node. Member so it can reach Environment's private fields.
    template <typename F>
    static void forEachChild(void* obj, GcNodeKind kind, F& f);

    void markNode(void* obj, GcNodeKind kind);
    void drainMarkStack();
};

// Factory: creates a shared_ptr and registers it with the thread-local GcHeap.
// Only use for GC-tracked types (PraiaArray, PraiaMap, PraiaInstance,
// PraiaClass, PraiaGenerator, Environment). Other types keep using make_shared.
template<typename T, typename... Args>
std::shared_ptr<T> gcNew(Args&&... args) {
    auto ptr = std::make_shared<T>(std::forward<Args>(args)...);
    GcHeap::current().track(ptr);
    return ptr;
}
