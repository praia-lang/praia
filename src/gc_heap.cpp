#include "fiber.h"
#include "gc_heap.h"
#include "environment.h"
#include "interpreter.h"
#include "vm/vm.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <utility>
#include <variant>

// ── Thread-local singleton ──

GcHeap& GcHeap::current() {
    static thread_local GcHeap instance;
    return instance;
}

// ── Task bag registry ──
//
// Process-wide: a bag outlives the task thread that made it and becomes
// garbage on whichever thread drops its last reference. `collecting`
// lets one heap at a time scan and clear bags, so no other collector is
// walking a bag's upvalues while they're cleared.

namespace {
struct TaskBagRegistry {
    std::mutex mu;          // guards bags
    std::mutex collecting;
    std::vector<std::weak_ptr<TaskOwnership>> bags;
};

TaskBagRegistry& bagRegistry() {
    static auto* reg = new TaskBagRegistry; // never destroyed: task threads may outlive statics
    return *reg;
}
} // namespace

void GcHeap::registerTaskBag(const std::shared_ptr<TaskOwnership>& bag) {
    auto& reg = bagRegistry();
    std::lock_guard<std::mutex> lk(reg.mu);
    reg.bags.push_back(bag);
}

// ── Tracking ──

void GcHeap::track(const std::shared_ptr<PraiaArray>& p) {
    entries_.push_back({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Array});
    allocsSinceGc_++;
}
void GcHeap::track(const std::shared_ptr<PraiaMap>& p) {
    entries_.push_back({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Map});
    allocsSinceGc_++;
}
void GcHeap::track(const std::shared_ptr<PraiaSet>& p) {
    entries_.push_back({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Set});
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
void GcHeap::track(const std::shared_ptr<PraiaExternal>& p) {
    entries_.push_back({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::External});
    allocsSinceGc_++;
}
void GcHeap::track(const std::shared_ptr<Environment>& p) {
    entries_.push_back({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Environment});
    allocsSinceGc_++;
}

// ── Control ──

GcHeap::GcHeap() {
    // PRAIA_GC_STRESS=N collects at the first safepoint after every N
    // allocations (N defaults to 1; 0 or unset disables stress mode).
    const char* s = std::getenv("PRAIA_GC_STRESS");
    if (s && *s) {
        long n = std::strtol(s, nullptr, 10);
        if (n > 0) {
            stress_ = true;
            stressInterval_ = static_cast<int>(std::min(n, 1000000L));
        } else if (std::strcmp(s, "0") != 0) {
            stress_ = true;
        }
    }
}

void GcHeap::setRootMarker(RootMarker marker) { rootMarker_ = std::move(marker); }
void GcHeap::disable() { disabled_ = true; }

void GcHeap::collectIfNeeded() {
    if (disabled_ || collecting_ || !rootMarker_) return;
    if (allocsSinceGc_ < (stress_ ? stressInterval_ : threshold_)) return;
    collect();
}

void GcHeap::collect() {
    if (collecting_ || !rootMarker_) return;
    collecting_ = true;
    allocsSinceGc_ = 0;
    nodes_.reset(entries_.size());

    mark();
    sweep();

    // Auto-tune threshold based on results
    if (lastCollected_ == 0 && threshold_ < 10000) {
        // No garbage found — collect less frequently
        threshold_ = std::min(threshold_ * 2, 10000);
    } else if (lastCollected_ > 0 && threshold_ > 500) {
        // Found garbage — collect more frequently
        threshold_ = std::max(threshold_ / 2, 500);
    }
    collecting_ = false;
}

// Identify a Value by its underlying GC-heap pointer. Used for the
// pinValue/unpinValue match — Value::operator== does deep equality
// for arrays/maps/etc. and falls through to `false` for Callables,
// neither of which is the semantics we want here. We want "same
// underlying allocation" so unpinValue(v) removes the entry pinValue(v)
// added, regardless of whether two distinct Values would compare equal.
// Returns nullptr for non-GC-tracked values (numbers, strings, bools,
// nil) — those have nothing for the sweep to clear and don't need
// to live in the registry.
static void* gcPointer(const Value& v) {
    if (v.isArray())     return v.asArray().get();
    if (v.isTagged())    return v.asTagged().get();
    if (v.isMap())       return v.asMap().get();
    if (v.isSet())       return v.asSet().get();
    if (v.isInstance())  return v.asInstance().get();
    if (v.isCallable())  return v.asCallable().get();
    if (v.isGenerator()) return v.asGenerator().get();
    if (v.isExternal())  return v.asExternal().get();
    return nullptr;
}

void GcHeap::pinValue(const Value& v) {
    // Non-GC values have nothing for the sweep to break, so the
    // registry doesn't need them — skipping keeps unpinValue's
    // match-by-pointer invariant (every entry has a non-null pointer)
    // and avoids retaining a never-needed string copy.
    if (gcPointer(v)) pinned_.push_back(v);
}

void GcHeap::unpinValue(const Value& v) {
    void* target = gcPointer(v);
    if (!target) return;
    // Walk back-to-front so the most recently pinned matching entry
    // wins — gives RAII guards LIFO semantics. Linear in pin count,
    // which is fine for the realistic case of "a handful of pinned
    // callbacks/caches per plugin"; if a plugin pins thousands of
    // values, replace with a hash-multiset.
    //
    // We match by underlying GC-heap pointer rather than
    // Value::operator==, which does deep equality for arrays/maps
    // and falls through to false for Callables — neither matches
    // the "same allocation I pinned earlier" semantics we want.
    for (auto it = pinned_.rbegin(); it != pinned_.rend(); ++it) {
        if (gcPointer(*it) == target) {
            pinned_.erase(std::next(it).base());
            return;
        }
    }
}

// ── Node table ──

void GcHeap::NodeTable::reset(size_t expected) {
    size_t cap = 64;
    while (cap < expected * 2) cap <<= 1;
    if (slots_.size() == cap) {
        std::fill(slots_.begin(), slots_.end(), Slot{});
    } else {
        slots_.assign(cap, Slot{});
    }
    size_ = 0;
    mask_ = cap - 1;
}

size_t GcHeap::NodeTable::indexFor(void* key) const {
    // Fibonacci hashing; low bits of heap pointers are alignment zeros.
    auto h = reinterpret_cast<uintptr_t>(key) * 0x9E3779B97F4A7C15ull;
    return static_cast<size_t>(h >> 20) & mask_;
}

std::pair<GcHeap::NodeTable::Slot*, bool>
GcHeap::NodeTable::insert(void* key, GcNodeKind kind, long refs) {
    if ((size_ + 1) * 2 > slots_.size()) grow();
    for (size_t i = indexFor(key);; i = (i + 1) & mask_) {
        Slot& s = slots_[i];
        if (s.key == key) return {&s, false};
        if (!s.key) {
            s.key = key;
            s.kind = kind;
            s.refs = refs;
            s.marked = false;
            size_++;
            return {&s, true};
        }
    }
}

GcHeap::NodeTable::Slot* GcHeap::NodeTable::find(void* key) {
    if (slots_.empty()) return nullptr;
    for (size_t i = indexFor(key);; i = (i + 1) & mask_) {
        Slot& s = slots_[i];
        if (s.key == key) return &s;
        if (!s.key) return nullptr;
    }
}

void GcHeap::NodeTable::grow() {
    std::vector<Slot> old = std::move(slots_);
    size_t cap = old.empty() ? 64 : old.size() * 2;
    slots_.assign(cap, Slot{});
    mask_ = cap - 1;
    for (auto& s : old) {
        if (!s.key) continue;
        size_t i = indexFor(s.key);
        while (slots_[i].key) i = (i + 1) & mask_;
        slots_[i] = s;
    }
}

// ── Object graph ──
//
// One edge enumerator shared by marking and by the external-reference
// scan, so the two can never disagree about what an object points at.
// The visitor is called as f(obj, kind, useCount, owning):
//   - obj/kind identify the target (PraiaClass is keyed by PraiaClass*,
//     which may differ from its Callable* because of multiple
//     inheritance; every other node by its own pointer),
//   - useCount is the target's shared_ptr count,
//   - owning is true when the edge is itself one of those shared_ptr
//     references. The scan only subtracts owning edges; marking follows
//     both kinds.

namespace {

GcNodeKind nodeKindOf(GcType t) {
    switch (t) {
        case GcType::Array:       return GcNodeKind::Array;
        case GcType::Tagged:      return GcNodeKind::Tagged;
        case GcType::Map:         return GcNodeKind::Map;
        case GcType::Set:         return GcNodeKind::Set;
        case GcType::Instance:    return GcNodeKind::Instance;
        case GcType::Class:       return GcNodeKind::Class;
        case GcType::Generator:   return GcNodeKind::Generator;
        case GcType::Environment: return GcNodeKind::Environment;
        case GcType::External:    return GcNodeKind::External;
    }
    return GcNodeKind::External;
}

template <typename T, typename F>
void visitPtr(const std::shared_ptr<T>& p, GcNodeKind kind, bool owning, F& f) {
    if (p) f(static_cast<void*>(p.get()), kind, static_cast<long>(p.use_count()), owning);
}

// Reads the shared_ptr in place — Value::asArray() and friends return
// copies, which would bump the use_count being measured.
template <typename F>
void visitValue(const Value& v, bool owning, F& f) {
    const auto& d = v.data;
    if (auto* p = std::get_if<std::shared_ptr<PraiaArray>>(&d))          visitPtr(*p, GcNodeKind::Array, owning, f);
    else if (auto* p = std::get_if<std::shared_ptr<PraiaMap>>(&d))       visitPtr(*p, GcNodeKind::Map, owning, f);
    else if (auto* p = std::get_if<std::shared_ptr<PraiaInstance>>(&d))  visitPtr(*p, GcNodeKind::Instance, owning, f);
    else if (auto* p = std::get_if<std::shared_ptr<PraiaSet>>(&d))       visitPtr(*p, GcNodeKind::Set, owning, f);
    else if (auto* p = std::get_if<std::shared_ptr<PraiaTagged>>(&d))    visitPtr(*p, GcNodeKind::Tagged, owning, f);
    else if (auto* p = std::get_if<std::shared_ptr<PraiaGenerator>>(&d)) visitPtr(*p, GcNodeKind::Generator, owning, f);
    else if (auto* p = std::get_if<std::shared_ptr<PraiaExternal>>(&d))  visitPtr(*p, GcNodeKind::External, owning, f);
    else if (auto* p = std::get_if<std::shared_ptr<Callable>>(&d)) {
        if (!*p) return;
        long uc = static_cast<long>(p->use_count());
        if (auto* cls = dynamic_cast<PraiaClass*>(p->get()))
            f(static_cast<void*>(cls), GcNodeKind::Class, uc, owning);
        else
            f(static_cast<void*>(p->get()), GcNodeKind::Callable, uc, owning);
    }
    // Primitives and futures: no traced children.
}

} // namespace

template <typename F>
void GcHeap::forEachChild(void* obj, GcNodeKind kind, F& f) {
    switch (kind) {
        case GcNodeKind::Array:
            for (auto& e : static_cast<PraiaArray*>(obj)->elements) visitValue(e, true, f);
            break;
        case GcNodeKind::Tagged:
            for (auto& e : static_cast<PraiaTagged*>(obj)->values) visitValue(e, true, f);
            break;
        case GcNodeKind::Map:
            for (auto& [k, v] : static_cast<PraiaMap*>(obj)->entries) {
                visitValue(k, true, f);
                visitValue(v, true, f);
            }
            break;
        case GcNodeKind::Set:
            for (auto& e : static_cast<PraiaSet*>(obj)->elements) visitValue(e, true, f);
            break;
        case GcNodeKind::Instance: {
            auto* inst = static_cast<PraiaInstance*>(obj);
            visitPtr(inst->klass, GcNodeKind::Class, true, f);
            for (auto& [k, v] : inst->fields) visitValue(v, true, f);
            break;
        }
        case GcNodeKind::Class: {
            auto* cls = static_cast<PraiaClass*>(obj);
            visitPtr(cls->superclass, GcNodeKind::Class, true, f);
            for (auto& [k, v] : cls->vmMethods) visitValue(v, true, f);
            for (auto& [k, v] : cls->vmStaticMethods) visitValue(v, true, f);
            for (auto& [k, vs] : cls->methodDecorators)
                for (auto& v : vs) visitValue(v, true, f);
            visitPtr(cls->closure, GcNodeKind::Environment, true, f);
            break;
        }
        case GcNodeKind::Generator: {
            auto* gen = static_cast<PraiaGenerator*>(obj);
            visitValue(gen->lastYielded, true, f);
            visitValue(gen->sendValue, true, f);
            for (auto& v : gen->savedStack) visitValue(v, true, f);
            visitPtr(gen->fiberEnv, GcNodeKind::Environment, true, f);
            break;
        }
        case GcNodeKind::Environment: {
            // Environment::variables and parent are private — GcHeap is a friend
            auto* env = static_cast<Environment*>(obj);
            for (auto& [k, v] : env->variables) visitValue(v, true, f);
            visitPtr(env->parent, GcNodeKind::Environment, true, f);
            break;
        }
        case GcNodeKind::External:
            // Leaf — opaque void* + deleter, no Praia children.
            break;
        case GcNodeKind::Callable: {
            auto* c = static_cast<Callable*>(obj);
            if (auto* fn = dynamic_cast<PraiaFunction*>(c)) {
                visitPtr(fn->closure, GcNodeKind::Environment, true, f);
            } else if (auto* l = dynamic_cast<PraiaLambda*>(c)) {
                visitPtr(l->closure, GcNodeKind::Environment, true, f);
            } else if (auto* m = dynamic_cast<PraiaMethod*>(c)) {
                visitPtr(m->closure, GcNodeKind::Environment, true, f);
                visitPtr(m->instance, GcNodeKind::Instance, true, f);
                visitPtr(m->definingClass, GcNodeKind::Class, true, f);
            } else if (auto* gf = dynamic_cast<PraiaGeneratorFunction*>(c)) {
                visitPtr(gf->closure, GcNodeKind::Environment, true, f);
            } else if (auto* gl = dynamic_cast<PraiaGeneratorLambda*>(c)) {
                visitPtr(gl->closure, GcNodeKind::Environment, true, f);
            } else if (auto* bm = dynamic_cast<VMBoundMethod*>(c)) {
                visitValue(bm->receiver, true, f);
                visitPtr(bm->definingClass, GcNodeKind::Class, true, f);
                visitPtr(bm->taskOwnership, GcNodeKind::TaskOwnership, true, f);
            } else if (auto* vmcc = dynamic_cast<VMClosureCallable*>(c)) {
                if (vmcc->closure) {
                    // Upvalues are nodes of their own: several closures
                    // can share one, and its single owner (the VM's
                    // allUpvalues, or a task's ownership bag) holds the
                    // only reference to it. This edge isn't that one.
                    for (auto* uv : vmcc->closure->upvalues)
                        if (uv) f(static_cast<void*>(uv), GcNodeKind::Upvalue, 1, false);
                    // Constant pools belong to the compiled function.
                    for (auto& constant : vmcc->closure->function->chunk.constants)
                        visitValue(constant, false, f);
                }
                visitPtr(vmcc->taskOwnership, GcNodeKind::TaskOwnership, true, f);
            }
            // NativeFunction: captures live inside std::function and
            // can't be enumerated. Anything it captures therefore shows
            // up as externally held and is kept.
            break;
        }
        case GcNodeKind::Upvalue: {
            // Counted as having one reference — its owner. An upvalue the
            // VM owns stays a root; one owned by a task bag cancels out
            // through the bag's edge below. Open upvalues point into the
            // VM stack, which the VM's root marker covers.
            auto* uv = static_cast<ObjUpvalue*>(obj);
            if (uv->location == &uv->closed) visitValue(uv->closed, true, f);
            break;
        }
        case GcNodeKind::TaskOwnership: {
            // A finished async task's closures keep their upvalues here
            // (unique_ptr) instead of in a VM's allUpvalues. Locked: the
            // task's thread fills the bag when its VM is destroyed.
            auto* bag = static_cast<TaskOwnership*>(obj);
            std::lock_guard<std::mutex> lk(bag->mu);
            for (auto& uv : bag->upvalues)
                if (uv) f(static_cast<void*>(uv.get()), GcNodeKind::Upvalue, 1, true);
            break;
        }
    }
}

// ── Mark phase ──

void GcHeap::markNode(void* obj, GcNodeKind kind) {
    if (!obj) return;
    auto [slot, inserted] = nodes_.insert(obj, kind, 0);
    (void)inserted;
    if (!slot->marked) {
        slot->marked = true;
        markStack_.push_back({obj, kind});
    }
}

void GcHeap::drainMarkStack() {
    auto push = [this](void* obj, GcNodeKind kind, long, bool) { markNode(obj, kind); };
    while (!markStack_.empty()) {
        MarkItem item = markStack_.back();
        markStack_.pop_back();
        forEachChild(item.obj, item.kind, push);
    }
}

void GcHeap::markValue(const Value& v) {
    auto push = [this](void* obj, GcNodeKind kind, long, bool) { markNode(obj, kind); };
    visitValue(v, true, push);
    drainMarkStack();
}

void GcHeap::markEnvironment(Environment* env) {
    markNode(static_cast<void*>(env), GcNodeKind::Environment);
    drainMarkStack();
}

// Trial deletion, as in CPython's cycle collector. Every tracked object
// starts with its real shared_ptr count; each owning edge from another
// object in the graph subtracts one. Whatever is left over is held from
// outside the heap. Untracked callables reachable from tracked objects
// join the graph too, so a function/environment cycle still cancels out.
//
// A count that doesn't land on exactly zero is treated as a root — a
// miscount in either direction only keeps an object alive longer.
std::vector<GcHeap::MarkItem> GcHeap::findExternallyHeld() {
    std::vector<void*> work;
    work.reserve(entries_.size());

    // Hold every live object for the rest of the collection. A root
    // found here may be owned only by another thread (an async task's
    // captures); without our reference that thread could free it while
    // we're still marking through it. Our own reference is subtracted
    // from the count.
    held_.reserve(entries_.size());
    for (auto& entry : entries_) {
        auto strong = entry.weak.lock();
        if (!strong) continue;
        long uc = static_cast<long>(strong.use_count()) - 1;
        if (nodes_.insert(entry.rawPtr, nodeKindOf(entry.type), uc).second) {
            work.push_back(entry.rawPtr);
            held_.push_back(std::move(strong));
        }
    }

    // Finished task bags, held like tracked objects above. Skipped this
    // round if another heap is already collecting them.
    auto& reg = bagRegistry();
    if (reg.collecting.try_lock()) {
        holdsBagLock_ = true;
        std::lock_guard<std::mutex> lk(reg.mu);
        std::erase_if(reg.bags, [](const std::weak_ptr<TaskOwnership>& w) { return w.expired(); });
        for (auto& weak : reg.bags) {
            auto strong = weak.lock();
            if (!strong) continue;
            long uc = static_cast<long>(strong.use_count()) - 1;
            if (nodes_.insert(strong.get(), GcNodeKind::TaskOwnership, uc).second) {
                work.push_back(strong.get());
                bags_.push_back(std::move(strong));
            }
        }
    }

    auto visit = [&](void* obj, GcNodeKind kind, long uc, bool owning) {
        auto [slot, inserted] = nodes_.insert(obj, kind, uc);
        if (owning) slot->refs--;
        if (inserted) work.push_back(obj);
    };
    while (!work.empty()) {
        void* obj = work.back();
        work.pop_back();
        GcNodeKind kind = nodes_.find(obj)->kind;
        forEachChild(obj, kind, visit);
    }

    std::vector<MarkItem> roots;
    nodes_.forEach([&](NodeTable::Slot& slot) {
        if (slot.refs == 0) return;
        if (stress_ && slot.refs < 0) {
            // A miscounted edge is a collector bug; fail the run on the
            // first one instead of warning on every later collection.
            std::cerr << "[gc-stress] negative external count (" << slot.refs
                      << ") for node kind " << static_cast<int>(slot.kind)
                      << " — an edge is counted that isn't a shared_ptr reference\n";
            std::abort();
        }
        roots.push_back({slot.key, slot.kind});
    });
    return roots;
}

void GcHeap::mark() {
    // Must run first: it seeds the node table with refcounts, and
    // marking then sets bits on the same slots.
    auto external = findExternallyHeld();

    rootMarker_(*this);
    // Plugin-pinned values: same reachability semantics as the
    // interpreter's roots — anything reachable from a pinned value
    // survives the sweep. Walked after the interpreter's marker so
    // the marker itself doesn't need to know about plugin state.
    for (const auto& v : pinned_) markValue(v);

    for (auto& item : external) markNode(item.obj, item.kind);
    drainMarkStack();
}

// ── Sweep phase ──

void GcHeap::sweep() {
    // Phase 1: Collect unreachable-but-alive objects, locking weak_ptrs
    // so they stay alive during clearing
    struct Unreachable {
        std::shared_ptr<void> strong;
        GcType type;
        void* rawPtr;
    };
    std::vector<Unreachable> toCollect;

    for (auto& entry : entries_) {
        auto* slot = nodes_.find(entry.rawPtr);
        if (!slot || !slot->marked) {
            auto strong = entry.weak.lock();
            if (strong) {
                toCollect.push_back({std::move(strong), entry.type, entry.rawPtr});
            }
        }
    }

    // Phase 2: Break cycles by clearing internal references
    for (auto& item : toCollect) {
        switch (item.type) {
            case GcType::Array:
                static_cast<PraiaArray*>(item.rawPtr)->elements.clear();
                break;
            case GcType::Tagged:
                // tag is a plain std::string (no GC roots); only the
                // values vector can carry cycles back into the heap.
                static_cast<PraiaTagged*>(item.rawPtr)->values.clear();
                break;
            case GcType::Map:
                static_cast<PraiaMap*>(item.rawPtr)->entries.clear();
                break;
            case GcType::Set:
                static_cast<PraiaSet*>(item.rawPtr)->elements.clear();
                break;
            case GcType::Instance: {
                auto* inst = static_cast<PraiaInstance*>(item.rawPtr);
                inst->fields.clear();
                inst->klass.reset();
                break;
            }
            case GcType::Class: {
                auto* cls = static_cast<PraiaClass*>(item.rawPtr);
                cls->vmMethods.clear();
                cls->vmStaticMethods.clear();
                cls->methodDecorators.clear();
                cls->superclass.reset();
                cls->closure.reset();
                break;
            }
            case GcType::Generator: {
                auto* gen = static_cast<PraiaGenerator*>(item.rawPtr);
                gen->lastYielded = Value();
                gen->sendValue = Value();
                gen->savedStack.clear();
                gen->ownedResources.clear();
                gen->fiber.reset();
                gen->fiberEnv.reset();
                break;
            }
            case GcType::Environment: {
                auto* env = static_cast<Environment*>(item.rawPtr);
                env->variables.clear();
                env->parent.reset();
                break;
            }
            case GcType::External:
                // Leaf — no internal Praia refs to break. The
                // deleter runs in PraiaExternal::~PraiaExternal()
                // when toCollect.clear() drops the last shared_ptr.
                break;
        }
    }

    // Unreachable task bags: drop their upvalues' captured values, which
    // breaks bag -> upvalue -> closure -> bag. Released after the bag
    // locks, since dropping a closure releases its bag reference.
    std::vector<Value> dropped;
    for (auto& bag : bags_) {
        auto* slot = nodes_.find(bag.get());
        if (slot && slot->marked) continue;
        std::lock_guard<std::mutex> lk(bag->mu);
        for (auto& uv : bag->upvalues)
            if (uv) dropped.push_back(std::exchange(uv->closed, Value()));
    }
    dropped.clear();

    lastCollected_ = static_cast<int>(toCollect.size());

    // Phase 3: Release locked shared_ptrs — objects whose cycles are broken
    // will now have refcount drop to 0 and be destroyed
    toCollect.clear();
    // Drop the references findExternallyHeld() took, so collected
    // objects die now and their entries are pruned below.
    held_.clear();
    bags_.clear();
    if (holdsBagLock_) {
        holdsBagLock_ = false;
        bagRegistry().collecting.unlock();
    }

    // Phase 4: Remove expired entries (dead via refcounting or just cleared)
    std::erase_if(entries_, [](const GcEntry& e) { return e.weak.expired(); });
}
