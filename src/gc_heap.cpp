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

std::mutex& GcHeap::upvalueMutex() {
    static auto* mu = new std::mutex; // never destroyed, like the registry
    return *mu;
}

// ── Tracking ──

void GcHeap::addEntry(GcEntry e) {
    if (lending_) lending_->objects.push_back(e.weak);
    entries_.push_back(std::move(e));
    allocsSinceGc_++;
}

void GcHeap::track(const std::shared_ptr<PraiaArray>& p) {
    addEntry({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Array});
}
void GcHeap::track(const std::shared_ptr<PraiaMap>& p) {
    addEntry({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Map});
}
void GcHeap::track(const std::shared_ptr<PraiaSet>& p) {
    addEntry({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Set});
}
void GcHeap::track(const std::shared_ptr<PraiaInstance>& p) {
    addEntry({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Instance});
}
void GcHeap::track(const std::shared_ptr<PraiaClass>& p) {
    addEntry({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Class});
}
void GcHeap::track(const std::shared_ptr<PraiaGenerator>& p) {
    addEntry({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Generator});
}
void GcHeap::track(const std::shared_ptr<PraiaTagged>& p) {
    addEntry({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Tagged});
}
void GcHeap::track(const std::shared_ptr<PraiaExternal>& p) {
    addEntry({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::External});
}
void GcHeap::track(const std::shared_ptr<Environment>& p) {
    addEntry({std::weak_ptr<void>(p), static_cast<void*>(p.get()), GcType::Environment});
}

// ── Ownership hand-offs ──

std::shared_ptr<GcHeap::LentObjects> GcHeap::beginLending() {
    lending_ = std::make_shared<LentObjects>();
    lent_.push_back(lending_);
    return lending_;
}

void GcHeap::endLending() { lending_.reset(); }

// The containers reachable from `root`, without going into callables.
// Called on values the calling thread is handing over (and holds a lock
// for), so their contents stay put while it reads them: the work stack
// holds pointers into them, and only containers go on it — a 100k-number
// array is one push, not 100k Value copies.
static void collectContainers(const Value& root, std::unordered_set<void*>& seen,
                              std::vector<std::weak_ptr<void>>& out) {
    auto isContainer = [](const Value& v) {
        return v.isArray() || v.isMap() || v.isSet() || v.isInstance() ||
               v.isTagged() || v.isGenerator();
    };
    std::vector<const Value*> work;
    if (isContainer(root)) work.push_back(&root);
    auto push = [&](const Value& v) { if (isContainer(v)) work.push_back(&v); };
    // Reads the shared_ptr in place, without copying it
    auto add = [&](const auto& p) {
        if (!p || !seen.insert(static_cast<void*>(p.get())).second) return false;
        out.push_back(std::weak_ptr<void>(p));
        return true;
    };
    while (!work.empty()) {
        const auto& d = work.back()->data;
        work.pop_back();
        if (auto* a = std::get_if<std::shared_ptr<PraiaArray>>(&d)) {
            if (add(*a)) for (auto& e : (*a)->elements) push(e);
        } else if (auto* m = std::get_if<std::shared_ptr<PraiaMap>>(&d)) {
            if (add(*m)) for (auto& [k, e] : (*m)->entries) { push(k); push(e); }
        } else if (auto* st = std::get_if<std::shared_ptr<PraiaSet>>(&d)) {
            if (add(*st)) for (auto& e : (*st)->elements) push(e);
        } else if (auto* inst = std::get_if<std::shared_ptr<PraiaInstance>>(&d)) {
            if (add(*inst)) for (auto& [k, e] : (*inst)->fields) push(e);
        } else if (auto* t = std::get_if<std::shared_ptr<PraiaTagged>>(&d)) {
            if (add(*t)) for (auto& e : (*t)->values) push(e);
        } else if (auto* g = std::get_if<std::shared_ptr<PraiaGenerator>>(&d)) {
            add(*g);
        }
    }
}

void GcHeap::disown(const Value& v) {
    // Only the containers: a callable's captured state stays with the
    // heap that made it (see "Shared handler state" in TODO.md)
    std::unordered_set<void*> seen;
    collectContainers(v, seen, disowned_);
    // Only a collection or a hand-over empties the list, and a task's
    // heap doesn't collect: compact it here, keeping each live object once
    if (disowned_.size() < compactDisownedAt_) return;
    std::unordered_set<void*> live;
    std::erase_if(disowned_, [&](const std::weak_ptr<void>& w) {
        auto p = w.lock();
        return !p || !live.insert(p.get()).second;
    });
    compactDisownedAt_ = std::max<size_t>(1024, disowned_.size() * 2);
}

void GcHeap::settleEntries() {
    {
        std::lock_guard<std::mutex> lk(inbox_->mu);
        allocsSinceGc_ += static_cast<int>(inbox_->entries.size());
        for (auto& e : inbox_->entries) entries_.push_back(std::move(e));
        inbox_->entries.clear();
        for (auto& l : inbox_->lent) {
            l->seenReturned = false; // looked at from the next collection on
            lent_.push_back(std::move(l));
        }
        inbox_->lent.clear();
        for (auto& w : inbox_->disowned) disowned_.push_back(std::move(w));
        inbox_->disowned.clear();
    }
    std::erase_if(foreignDisowns_, [](const std::weak_ptr<void>& w) { return w.expired(); });
    if (disowned_.empty()) return;
    // A disowned object that's still alive is the one at that address
    std::unordered_set<void*> gone, found;
    for (auto& w : disowned_)
        if (auto p = w.lock()) gone.insert(p.get());
    std::erase_if(entries_, [&](const GcEntry& e) {
        if (!gone.count(e.rawPtr) || e.weak.expired()) return false;
        found.insert(e.rawPtr);
        return true;
    });
    // The rest another heap tracks — or none does, once its heap is gone.
    // Each once: the same value can be disowned again and again (a
    // SharedMap entry updated in a loop).
    std::unordered_set<void*> recorded;
    for (auto& w : foreignDisowns_)
        if (auto p = w.lock()) recorded.insert(p.get());
    for (auto& w : disowned_) {
        auto p = w.lock();
        if (p && !found.count(p.get()) && recorded.insert(p.get()).second)
            foreignDisowns_.push_back(std::move(w));
    }
    disowned_.clear();
}

// Appends to the inbox that's still taking deliveries
static void deliver(std::shared_ptr<GcHeap::Inbox> to, GcHeap::Inbox& from) {
    while (true) {
        std::unique_lock<std::mutex> lk(to->mu);
        if (to->forward) {
            auto next = to->forward;
            lk.unlock();
            to = std::move(next);
            continue;
        }
        for (auto& e : from.entries) to->entries.push_back(std::move(e));
        for (auto& l : from.lent) to->lent.push_back(std::move(l));
        for (auto& w : from.disowned) to->disowned.push_back(std::move(w));
        break;
    }
    from.entries.clear();
    from.lent.clear();
    from.disowned.clear();
}

void GcHeap::handOverTo(const std::shared_ptr<Inbox>& to) {
    settleEntries();
    lending_.reset();
    // Plugin-pinned values stay: the plugin may use them from any thread
    std::unordered_set<void*> pinned;
    std::vector<std::weak_ptr<void>> unused;
    for (const auto& v : pinned_) collectContainers(v, pinned, unused);
    Inbox out;
    for (auto& e : entries_)
        if (!e.weak.expired() && !pinned.count(e.rawPtr)) out.entries.push_back(std::move(e));
    entries_.clear();
    // Copies this task lent to tasks of its own that are still running
    for (auto& l : lent_)
        if (!l->returned.load(std::memory_order_acquire)) out.lent.push_back(std::move(l));
    lent_.clear();
    // Copies it was lent and then disowned: the lender stops tracking
    // them before it sees this task's copies returned
    out.disowned = std::move(foreignDisowns_);
    foreignDisowns_.clear();
    deliver(to, out);
    // Anything a task of this one hands over later goes on to `to`
    {
        std::lock_guard<std::mutex> lk(inbox_->mu);
        inbox_->forward = to;
        out.entries = std::move(inbox_->entries);
        out.lent = std::move(inbox_->lent);
        out.disowned = std::move(inbox_->disowned);
        inbox_->entries.clear();
        inbox_->lent.clear();
        inbox_->disowned.clear();
    }
    deliver(to, out);
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
    // Read before the inbox: a task delivers what it disowned before it
    // marks its lent copies returned, so a copy seen returned here has
    // its disown waiting in the inbox
    for (auto& l : lent_) l->seenReturned = l->returned.load(std::memory_order_acquire);
    settleEntries();
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
            std::lock_guard<std::mutex> lk(upvalueMutex());
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

bool GcHeap::canTraverse(void* obj, GcNodeKind kind) const {
    switch (kind) {
        case GcNodeKind::Callable:
        case GcNodeKind::TaskOwnership:
            return true;  // fixed once made; a bag's contents are locked
        case GcNodeKind::Upvalue: {
            // Ours, or a finished task's (read under upvalueMutex). Another
            // live thread's upvalue it writes without the lock.
            auto* owner = static_cast<ObjUpvalue*>(obj)->owner.load(std::memory_order_acquire);
            return owner == this || owner == nullptr;
        }
        default:
            return false; // a container another heap tracks, or none does
    }
}

void GcHeap::markNode(void* obj, GcNodeKind kind) {
    if (!obj) return;
    auto [slot, inserted] = nodes_.insert(obj, kind, 0);
    if (inserted) slot->traverse = canTraverse(obj, kind);
    if (!slot->marked) {
        slot->marked = true;
        markStack_.push_back({obj, kind, slot->traverse});
    }
}

void GcHeap::drainMarkStack() {
    auto push = [this](void* obj, GcNodeKind kind, long, bool) { markNode(obj, kind); };
    while (!markStack_.empty()) {
        MarkItem item = markStack_.back();
        markStack_.pop_back();
        if (item.traverse) forEachChild(item.obj, item.kind, push);
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

void GcHeap::markUpvalue(ObjUpvalue* uv) {
    markNode(static_cast<void*>(uv), GcNodeKind::Upvalue);
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
    //
    // Objects lent to a running task are kept without being looked inside.
    std::unordered_set<void*> lent;
    std::erase_if(lent_, [&](const std::shared_ptr<LentObjects>& l) {
        if (l != lending_ && l->seenReturned) return true;
        for (auto& w : l->objects)
            if (auto p = w.lock()) lent.insert(p.get());
        return false;
    });
    std::vector<MarkItem> lentRoots;
    held_.reserve(entries_.size());
    for (auto& entry : entries_) {
        auto strong = entry.weak.lock();
        if (!strong) continue;
        long uc = static_cast<long>(strong.use_count()) - 1;
        GcNodeKind kind = nodeKindOf(entry.type);
        auto [slot, inserted] = nodes_.insert(entry.rawPtr, kind, uc);
        if (inserted) {
            if (lent.count(entry.rawPtr)) {
                slot->traverse = false;
                lentRoots.push_back({entry.rawPtr, kind, false});
            } else {
                work.push_back(entry.rawPtr);
            }
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
        if (inserted) {
            slot->traverse = canTraverse(obj, kind);
            if (slot->traverse) work.push_back(obj);
        }
    };
    while (!work.empty()) {
        void* obj = work.back();
        work.pop_back();
        GcNodeKind kind = nodes_.find(obj)->kind;
        forEachChild(obj, kind, visit);
    }

    std::vector<MarkItem> roots = std::move(lentRoots);
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
        roots.push_back({slot.key, slot.kind, slot.traverse});
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
                gen->savedUpvalues.clear();
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
        std::lock_guard<std::mutex> uvLk(upvalueMutex());
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
