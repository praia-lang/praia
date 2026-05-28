#include "fiber.h"
#include "gc_heap.h"
#include "environment.h"
#include "interpreter.h"
#include "vm/vm.h"
#include <algorithm>

// ── Thread-local singleton ──

GcHeap& GcHeap::current() {
    static thread_local GcHeap instance;
    return instance;
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

void GcHeap::setRootMarker(RootMarker marker) { rootMarker_ = std::move(marker); }
void GcHeap::disable() { disabled_ = true; }

void GcHeap::collectIfNeeded() {
    if (disabled_ || collecting_ || !rootMarker_ || allocsSinceGc_ < threshold_) return;
    collect();
}

void GcHeap::collect() {
    if (collecting_ || !rootMarker_) return;
    collecting_ = true;
    allocsSinceGc_ = 0;
    marked_.clear();

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

// ── Mark phase ──

void GcHeap::mark() {
    rootMarker_(*this);
    // Plugin-pinned values: same reachability semantics as the
    // interpreter's roots — anything reachable from a pinned value
    // survives the sweep. Walked after the interpreter's marker so
    // the marker itself doesn't need to know about plugin state.
    for (const auto& v : pinned_) markValue(v);
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

void GcHeap::markValue(const Value& v) {
    if (v.isNil() || v.isBool() || v.isInt() || v.isDouble() || v.isString() || v.isFuture())
        return;

    if (v.isArray()) {
        markArray(v.asArray().get());
    } else if (v.isTagged()) {
        markTagged(v.asTagged().get());
    } else if (v.isMap()) {
        markMap(v.asMap().get());
    } else if (v.isSet()) {
        markSet(v.asSet().get());
    } else if (v.isInstance()) {
        markInstance(v.asInstance().get());
    } else if (v.isCallable()) {
        markCallable(v.asCallable().get());
    } else if (v.isGenerator()) {
        markGenerator(v.asGenerator().get());
    } else if (v.isExternal()) {
        // Externals are leaves — opaque void* + deleter, no Praia
        // children to traverse. Marking is just identity insertion
        // so the sweep knows it's reachable.
        marked_.insert(static_cast<void*>(v.asExternal().get()));
    }
}

void GcHeap::markArray(PraiaArray* arr) {
    if (!arr) return;
    if (!marked_.insert(static_cast<void*>(arr)).second) return;
    for (auto& elem : arr->elements) markValue(elem);
}

void GcHeap::markTagged(PraiaTagged* tag) {
    if (!tag) return;
    if (!marked_.insert(static_cast<void*>(tag)).second) return;
    for (auto& v : tag->values) markValue(v);
}

void GcHeap::markMap(PraiaMap* map) {
    if (!map) return;
    if (!marked_.insert(static_cast<void*>(map)).second) return;
    for (auto& [k, v] : map->entries) { markValue(k); markValue(v); }
}

void GcHeap::markSet(PraiaSet* set) {
    if (!set) return;
    if (!marked_.insert(static_cast<void*>(set)).second) return;
    for (auto& e : set->elements) markValue(e);
}

void GcHeap::markInstance(PraiaInstance* inst) {
    if (!inst) return;
    if (!marked_.insert(static_cast<void*>(inst)).second) return;
    if (inst->klass) markClass(inst->klass.get());
    for (auto& [k, v] : inst->fields) markValue(v);
}

void GcHeap::markClass(PraiaClass* cls) {
    if (!cls) return;
    if (!marked_.insert(static_cast<void*>(cls)).second) return;
    if (cls->superclass) markClass(cls->superclass.get());
    for (auto& [k, v] : cls->vmMethods) markValue(v);
    for (auto& [k, v] : cls->vmStaticMethods) markValue(v);
    for (auto& [k, vs] : cls->methodDecorators) {
        for (auto& v : vs) markValue(v);
    }
    if (cls->closure) markEnvironment(cls->closure.get());
}

void GcHeap::markGenerator(PraiaGenerator* gen) {
    if (!gen) return;
    if (!marked_.insert(static_cast<void*>(gen)).second) return;
    markValue(gen->lastYielded);
    markValue(gen->sendValue);
    for (auto& v : gen->savedStack) markValue(v);
    if (gen->fiberEnv) markEnvironment(gen->fiberEnv.get());
}

void GcHeap::markEnvironment(Environment* env) {
    if (!env) return;
    if (!marked_.insert(static_cast<void*>(env)).second) return;
    // Environment::variables and parent are private — GcHeap is a friend
    for (auto& [k, v] : env->variables) markValue(v);
    if (env->parent) markEnvironment(env->parent.get());
}

void GcHeap::markCallable(Callable* c) {
    if (!c) return;

    // PraiaClass has multiple inheritance (Callable + enable_shared_from_this)
    // so Callable* and PraiaClass* may differ — use PraiaClass* for identity
    if (auto* cls = dynamic_cast<PraiaClass*>(c)) {
        markClass(cls);
        return;
    }

    // Other callables: single inheritance, Callable* == concrete type*
    if (!marked_.insert(static_cast<void*>(c)).second) return;

    if (auto* f = dynamic_cast<PraiaFunction*>(c)) {
        if (f->closure) markEnvironment(f->closure.get());
    } else if (auto* l = dynamic_cast<PraiaLambda*>(c)) {
        if (l->closure) markEnvironment(l->closure.get());
    } else if (auto* m = dynamic_cast<PraiaMethod*>(c)) {
        if (m->closure) markEnvironment(m->closure.get());
        if (m->instance) markInstance(m->instance.get());
        if (m->definingClass) markClass(m->definingClass.get());
    } else if (auto* gf = dynamic_cast<PraiaGeneratorFunction*>(c)) {
        if (gf->closure) markEnvironment(gf->closure.get());
    } else if (auto* gl = dynamic_cast<PraiaGeneratorLambda*>(c)) {
        if (gl->closure) markEnvironment(gl->closure.get());
    } else if (auto* bm = dynamic_cast<VMBoundMethod*>(c)) {
        markValue(bm->receiver);
        if (bm->definingClass) markClass(bm->definingClass.get());
    } else if (auto* vmcc = dynamic_cast<VMClosureCallable*>(c)) {
        // Trace closed upvalues and function constants
        if (vmcc->closure) {
            for (auto* uv : vmcc->closure->upvalues) {
                if (uv && uv->location == &uv->closed)
                    markValue(uv->closed);
            }
            for (auto& constant : vmcc->closure->function->chunk.constants)
                markValue(constant);
        }
    }
    // NativeFunction: no traceable Praia-heap children
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
        if (!marked_.count(entry.rawPtr)) {
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

    lastCollected_ = static_cast<int>(toCollect.size());

    // Phase 3: Release locked shared_ptrs — objects whose cycles are broken
    // will now have refcount drop to 0 and be destroyed
    toCollect.clear();

    // Phase 4: Remove expired entries (dead via refcounting or just cleared)
    std::erase_if(entries_, [](const GcEntry& e) { return e.weak.expired(); });

    marked_.clear();
}
