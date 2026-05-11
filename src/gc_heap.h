#pragma once

#include "value.h"
#include <functional>
#include <unordered_set>
#include <vector>

class Environment; // forward declaration

enum class GcType { Array, Tagged, Map, Instance, Class, Generator, Environment };

class GcHeap {
public:
    static GcHeap& current();

    // Track container objects (called by gcNew)
    void track(const std::shared_ptr<PraiaArray>& p);
    void track(const std::shared_ptr<PraiaMap>& p);
    void track(const std::shared_ptr<PraiaInstance>& p);
    void track(const std::shared_ptr<PraiaClass>& p);
    void track(const std::shared_ptr<PraiaGenerator>& p);
    void track(const std::shared_ptr<PraiaTagged>& p);
    void track(const std::shared_ptr<Environment>& p);

    // Mark API (called during root marking by VM/Interpreter)
    void markValue(const Value& v);
    void markEnvironment(Environment* env);

    // Collection control
    using RootMarker = std::function<void(GcHeap&)>;
    void setRootMarker(RootMarker marker);
    void collectIfNeeded();
    void collect();
    void disable();

    int trackedCount() const { return static_cast<int>(entries_.size()); }
    int collected() const { return lastCollected_; }

private:
    struct GcEntry {
        std::weak_ptr<void> weak;
        void* rawPtr;
        GcType type;
    };

    std::vector<GcEntry> entries_;
    std::unordered_set<void*> marked_;
    RootMarker rootMarker_;
    int allocsSinceGc_ = 0;
    int lastCollected_ = 0;
    int threshold_ = 500;
    bool collecting_ = false;
    bool disabled_ = false;

    void mark();
    void sweep();

    // Type-specific mark helpers
    void markArray(PraiaArray* arr);
    void markTagged(PraiaTagged* tag);
    void markMap(PraiaMap* map);
    void markInstance(PraiaInstance* inst);
    void markClass(PraiaClass* cls);
    void markGenerator(PraiaGenerator* gen);
    void markCallable(Callable* c);
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
