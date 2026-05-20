#include "vm.h"
#include "../builtins.h"
#include "../gc_heap.h"
#include "../signal_state.h"
#include "../grain_resolve.h"
#include "../interpreter.h"
#include "../lexer.h"
#include "../parser.h"
#include "../unicode.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace fs = std::filesystem;

// In-flight async task tracker. The C++ standard makes the destructor of the
// last reference to a std::async(launch::async) shared state block until the
// task completes. For Praia's fire-and-forget pattern (`async fn()` as a
// statement), the shared_future inside the discarded PraiaFuture would be
// that last reference and block the caller. We keep an extra reference here
// so the call site is never the last one.
//
// Reaping: a lazy-spawned coordinator thread sweeps completed entries on a
// CV-driven schedule. Each retain notifies, so steady-state reap latency is
// bounded by ~one task duration, not a fixed interval. A periodic 5-second
// timer ensures burst-then-idle workloads (which would otherwise pin
// completed task results forever) eventually release their memory.
//
// Lifetime: the state is intentionally leaked (allocated once, never freed).
// At process exit the OS reclaims the memory and kills the detached reaper
// thread. We can't use static-storage objects because destroying the mutex
// or condition_variable while the reaper is blocked in wait_for() is
// undefined behavior — on Linux it deadlocks the process during the
// global-destructor pass; on macOS the runtime forces threads down first
// and gets away with it. The leak is one fixed-size struct per process.
struct InflightState {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::shared_future<Value>> futures;
};

static InflightState* getInflight() {
    static InflightState* state = new InflightState();
    return state;
}

static void inflightReaperLoop(InflightState* st) {
    using namespace std::chrono_literals;
    std::unique_lock<std::mutex> lock(st->mtx);
    while (true) {
        // Wait for either a notify (new retain) or the periodic timer.
        // No predicate — every wakeup performs a sweep regardless.
        st->cv.wait_for(lock, 5s);
        st->futures.erase(
            std::remove_if(st->futures.begin(), st->futures.end(),
                [](const std::shared_future<Value>& sf) {
                    return !sf.valid() ||
                           sf.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                }),
            st->futures.end());
    }
}

static std::once_flag g_inflightReaperOnce;

void retainInflightFuture(const std::shared_future<Value>& f) {
    InflightState* st = getInflight();
    std::call_once(g_inflightReaperOnce, [st] {
        std::thread(inflightReaperLoop, st).detach();
    });
    {
        std::lock_guard<std::mutex> lock(st->mtx);
        st->futures.push_back(f);
    }
    // Wake the reaper to consider this entry once it's ready; cheap no-op if
    // the reaper is already mid-sweep.
    st->cv.notify_one();
}

// ── Operator overloading helper ──
// Try to call a dunder method on a VM instance. Returns {true, result} if found.
//
// Hot path: most instances have no operator overloads, so we short-circuit on
// the precomputed `hasOperatorOverloads` flag (set during class definition,
// inherited on extends). This avoids the chain walk + dynamic_cast on every
// `+`, `==`, `<`, etc. for ordinary instances.
static std::pair<bool, Value> vmCallDunder(VM& vm, const Value& instance,
                                            const std::string& methodName,
                                            const std::vector<Value>& args) {
    if (!instance.isInstance()) return {false, Value()};
    auto inst = instance.asInstance();
    if (!inst->klass || !inst->klass->hasOperatorOverloads) return {false, Value()};
    auto walk = inst->klass;
    while (walk) {
        auto it = walk->vmMethods.find(methodName);
        if (it != walk->vmMethods.end() && it->second.isCallable()) {
            auto* vmcc = dynamic_cast<VMClosureCallable*>(it->second.asCallable().get());
            if (vmcc) {
                auto bm = std::make_shared<VMBoundMethod>(instance, vmcc->closure, walk);
                Value result = callWithVM(vm, std::static_pointer_cast<Callable>(bm), args);
                return {true, result};
            }
            break;
        }
        walk = walk->superclass;
    }
    return {false, Value()};
}

// ── Async deep-copy helpers ──
//
// Used by OP_ASYNC to clone heap-allocated values across the task boundary
// so the task VM doesn't share mutable state with the caller. Handles
// PraiaMap, PraiaArray, PraiaInstance (clone-fields), VMClosureCallable
// (clone wrapper with vm=nullptr; rewireForTaskVm fixes it up later), and
// VMBoundMethod (clone with deep-copied receiver). NativeFunction,
// PraiaClass, primitives, strings, futures are shared as-is.
//
// Cycle handling: `visited` maps original heap pointer → freshly-created
// copy. Caller scopes the visited map: a per-batch map (args + upvalues +
// receiver of one OP_ASYNC) gives shared identity across that batch; a
// per-call map (one global lazy-load) keeps identity inside one subtree
// but lets different subtrees clone shared sub-objects independently.
static Value deepCopyForTask(const Value& v,
                             std::unordered_map<void*, Value>& visited) {
    if (v.isCallable()) {
        auto* vmcc = dynamic_cast<VMClosureCallable*>(v.asCallable().get());
        if (vmcc) {
            auto clone = std::make_shared<VMClosureCallable>(vmcc->closure);
            clone->ownedPrototype = vmcc->ownedPrototype;
            clone->vm = nullptr; // rewireForTaskVm sets this to &taskVm
            return Value(std::static_pointer_cast<Callable>(clone));
        }
        auto* bm = dynamic_cast<VMBoundMethod*>(v.asCallable().get());
        if (bm) {
            Value recvCopy = deepCopyForTask(bm->receiver, visited);
            auto clone = std::make_shared<VMBoundMethod>(
                std::move(recvCopy), bm->method, bm->definingClass);
            return Value(std::static_pointer_cast<Callable>(clone));
        }
        return v; // NativeFunction etc — safe to share
    }
    if (v.isMap()) {
        void* key = static_cast<void*>(v.asMap().get());
        auto it = visited.find(key);
        if (it != visited.end()) return it->second;
        auto copy = gcNew<PraiaMap>();
        Value result(copy);
        visited[key] = result; // register before recursing
        for (auto& [k, val] : v.asMap()->entries)
            copy->entries[k] = deepCopyForTask(val, visited);
        return result;
    }
    if (v.isArray()) {
        void* key = static_cast<void*>(v.asArray().get());
        auto it = visited.find(key);
        if (it != visited.end()) return it->second;
        auto copy = gcNew<PraiaArray>();
        Value result(copy);
        visited[key] = result;
        for (auto& el : v.asArray()->elements)
            copy->elements.push_back(deepCopyForTask(el, visited));
        return result;
    }
    if (v.isInstance()) {
        void* key = static_cast<void*>(v.asInstance().get());
        auto it = visited.find(key);
        if (it != visited.end()) return it->second;
        auto copy = gcNew<PraiaInstance>();
        copy->klass = v.asInstance()->klass; // share class (immutable)
        Value result(copy);
        visited[key] = result;
        for (auto& [k, fv] : v.asInstance()->fields)
            copy->fields[k] = deepCopyForTask(fv, visited);
        return result;
    }
    return v; // primitives, strings, futures
}

// Walk a freshly cloned Value subtree and rewire every VMClosureCallable's
// `vm` pointer to the given task VM. Called per-subtree after a
// deepCopyForTask. Necessary so closures *returned* from the task that get
// invoked on the parent thread post-await can fall back to the right VM
// (VM::current() is null on a non-executing thread).
static void rewireForTaskVm(Value& v, VM* taskVm,
                            std::unordered_set<void*>& visited) {
    if (v.isCallable()) {
        auto* vc = dynamic_cast<VMClosureCallable*>(v.asCallable().get());
        if (vc) vc->vm = taskVm;
    }
    if (v.isMap()) {
        void* p = static_cast<void*>(v.asMap().get());
        if (visited.insert(p).second)
            for (auto& [k, mv] : v.asMap()->entries)
                rewireForTaskVm(mv, taskVm, visited);
    }
    if (v.isArray()) {
        void* p = static_cast<void*>(v.asArray().get());
        if (visited.insert(p).second)
            for (auto& el : v.asArray()->elements)
                rewireForTaskVm(el, taskVm, visited);
    }
    if (v.isInstance()) {
        void* p = static_cast<void*>(v.asInstance().get());
        if (visited.insert(p).second)
            for (auto& [k, fv] : v.asInstance()->fields)
                rewireForTaskVm(fv, taskVm, visited);
    }
}

// Slow path for OP_GET_GLOBAL / POST_INC/DEC_GLOBAL in a task VM whose
// snapshot entry hasn't been materialized yet. Deep-copies one slot's
// value from the parent's snapshot, rewires VMClosureCallable vm pointers
// in the cloned subtree, and marks the slot loaded.
//
// Per-slot visited map: identity within one global subtree is preserved,
// but two globals that share a parent sub-object clone it independently
// here — accepted minor behavior change from the eager design's one
// shared visited map. Cross-global identity divergence is only visible if
// user code asserts `id(A.x) == id(B.x)` after the boundary, which is
// extremely rare.
void VM::lazyLoadGlobal(int slot) {
    std::unordered_map<void*, Value> visited;
    Value cloned = deepCopyForTask(globalsSnapshot_[slot], visited);
    std::unordered_set<void*> rewireVisited;
    rewireForTaskVm(cloned, this, rewireVisited);
    globals[slot] = std::move(cloned);
    loadedMask_[slot] = 1;
    globalsSnapshot_[slot] = Value(); // release stale shared_ptr
}

// Thread-local pointer to the currently executing VM
thread_local VM* VM::currentVM_ = nullptr;

/* RAII guard to set/restore currentVM_ and executeFloor_ across execute() calls.
executeFloor_ scopes exception handlers: handlers pushed before this execute()
call belong to an outer scope and must not be consumed here. */
struct VMScope {
    VM* vm;
    VM* prevVM;
    int savedFloor;
    VMScope(VM* v) : vm(v), prevVM(VM::currentVM_), savedFloor(v->executeFloor_) {
        VM::currentVM_ = v;
        v->executeFloor_ = static_cast<int>(v->exceptionHandlers.size());
        v->executeDepth_++;
    }
    ~VMScope() {
        vm->executeDepth_--;
        vm->executeFloor_ = savedFloor;
        VM::currentVM_ = prevVM;
    }
};

/* VMClosureCallable::call — allows native functions (filter, map, etc.)
to call VM closures through the tree-walker's Callable interface.
Uses VM::current() so closures returned from async tasks work correctly
even after the task VM is destroyed. */
Value VMClosureCallable::call(Interpreter&, const std::vector<Value>& args) {
    // Prefer VM::current() (always the live VM). Fall back to stored vm only
    // when there's no active execute() (tree-walker compatibility path).
    // Async-returned closures have vm=nullptr, so this always does the right thing.
    VM* currentVm = VM::current();
    if (!currentVm) currentVm = vm;
    if (!currentVm) return Value();

    int savedFrameCount = currentVm->frameCount;

    // Slot 0 = the callable itself (functions reference slot 0 for recursion).
    // Build a lightweight wrapper pointing to the same ObjClosure.
    auto self = std::make_shared<VMClosureCallable>(closure);
    self->vm = currentVm;
    self->ownedPrototype = ownedPrototype;
    self->taskOwnership = taskOwnership;
    currentVm->push(Value(std::static_pointer_cast<Callable>(self)));
    for (auto& arg : args) currentVm->push(arg);

    currentVm->callClosure(closure, static_cast<int>(args.size()), 0); // throws on failure

    auto result = currentVm->execute(savedFrameCount);
    if (result != VM::Result::OK)
        throw RuntimeError(
            currentVm->lastError().empty() ? "Callback failed" : currentVm->lastError(), 0);

    return currentVm->pop();
}

VM::VM() : stack(std::make_unique<Value[]>(STACK_MAX)) {}

VM::~VM() = default;

void VM::defineNative(const std::string& name, Value value) {
    int slot = ensureGlobalSlot(name);
    globals[slot] = std::move(value);
    builtinNames_.insert(name);
}

void VM::setArgs(const std::vector<std::string>& args) {
    auto arr = gcNew<PraiaArray>();
    for (auto& a : args)
        arr->elements.push_back(Value(a));
    int slot = findGlobalSlot("sys");
    if (slot >= 0 && globals[slot].isMap())
        globals[slot].asMap()->entries[Value("args")] = Value(arr);
}

void VM::push(Value value) {
    if (stackTop >= STACK_MAX) {
        std::cerr << "Fatal: Stack overflow (depth " << stackTop << ")" << std::endl;
        std::cerr << formatStackTrace();
        resetStack();
        throw RuntimeError("Stack overflow", 0);
    }
    stack[stackTop++] = std::move(value);
}

Value VM::pop() {
    if (stackTop <= 0) {
        std::cerr << "Internal error: stack underflow" << std::endl;
        return Value();
    }
    return std::move(stack[--stackTop]);
}

Value& VM::peek(int distance) {
    int idx = stackTop - 1 - distance;
    if (idx < 0) {
        throw RuntimeError("Internal error: stack underflow (peek)", 0);
    }
    return stack[idx];
}
void VM::resetStack() { stackTop = 0; frameCount = 0; }

Value VM::resumeGenerator(std::shared_ptr<PraiaGenerator> gen, Value sendVal) {
    if (gen->state == PraiaGenerator::State::COMPLETED) {
        auto result = gcNew<PraiaMap>();
        result->entries[Value("value")] = Value();
        result->entries[Value("done")] = Value(true);
        return Value(result);
    }

    int restoreBase = stackTop;
    for (auto& val : gen->savedStack) push(val);

    auto* closure = static_cast<ObjClosure*>(gen->vmClosure);
    int genBase = frameCount;
    auto& frame = frames[frameCount++];
    frame.closure = closure;
    frame.function = closure->function;
    frame.ip = gen->savedIp;
    frame.baseSlot = restoreBase;
    // Generators are past their prologue when restored, so OP_IS_MISSING_ARG
    // never runs in a resumed frame. Zero mask means "everything provided".
    frame.missingArgsMask = 0;
    frame.definingClass = nullptr;

    // Push sendValue as result of yield expression (skip on first call)
    if (gen->state != PraiaGenerator::State::CREATED)
        push(sendVal);

    gen->state = PraiaGenerator::State::RUNNING;

    auto prevGen = currentGenerator_;
    auto prevGenBase = genBaseFrame_;
    auto prevGenStackTop = genBaseStackTop_;
    currentGenerator_ = gen;
    genBaseFrame_ = genBase;
    genBaseStackTop_ = restoreBase;

    auto result = execute(genBase);

    currentGenerator_ = prevGen;
    genBaseFrame_ = prevGenBase;
    genBaseStackTop_ = prevGenStackTop;

    if (result != Result::OK) {
        gen->state = PraiaGenerator::State::COMPLETED;
        throw RuntimeError(
            lastError_.empty() ? "Generator failed" : lastError_, 0);
    }

    return pop();
}

uint8_t VM::readByte() { return *frames[frameCount - 1].ip++; }

uint16_t VM::readU16() {
    auto& frame = frames[frameCount - 1];
    uint16_t val = (frame.ip[0] << 8) | frame.ip[1];
    frame.ip += 2;
    return val;
}

Value VM::readConstant() {
    uint16_t idx = readU16();
    auto& frame = frames[frameCount - 1];
    return frame.chunk().constants[idx];
}

std::string VM::readString() { return readConstant().asString(); }

// ── Upvalue management ──────────────────────────────────────

ObjUpvalue* VM::captureUpvalue(Value* local) {
    ObjUpvalue* prevUpvalue = nullptr;
    ObjUpvalue* upvalue = openUpvalues;

    while (upvalue && upvalue->location > local) {
        prevUpvalue = upvalue;
        upvalue = upvalue->next;
    }

    if (upvalue && upvalue->location == local) return upvalue;

    allUpvalues.push_back(std::make_unique<ObjUpvalue>(local));
    auto* created = allUpvalues.back().get();
    created->next = upvalue;

    if (prevUpvalue) prevUpvalue->next = created;
    else openUpvalues = created;

    return created;
}

void VM::closeUpvalues(Value* last) {
    while (openUpvalues && openUpvalues->location >= last) {
        ObjUpvalue* upvalue = openUpvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        openUpvalues = upvalue->next;
    }
}

// ── Function calls ──────────────────────────────────────────

bool VM::callClosure(ObjClosure* closure, int argCount, int line) {
    auto& fn = closure->function;
    bool hasRest = !fn->restParam.empty();
    int origArgCount = argCount;

    // Allow fewer args (defaults to nil) but not more (unless rest param).
    if (!hasRest && argCount > fn->arity) {
        throw RuntimeError(fn->name + "() expected at most " + std::to_string(fn->arity) +
            " " + argStr(fn->arity) + " but got " + std::to_string(argCount), line);
    }

    if (fn->arity > 64) {
        throw RuntimeError(fn->name + "() has more than 64 parameters (max supported)", line);
    }

    // Pad missing args with nil
    while (argCount < fn->arity) {
        push(Value());
        argCount++;
    }

    // Build missing-args mask: bits [origArgCount, arity) are set.
    uint64_t missingMask = 0;
    for (int i = origArgCount; i < fn->arity; i++) missingMask |= (uint64_t)1 << i;

    // Collect extra args into rest array
    if (hasRest) {
        int extraCount = argCount - fn->arity;
        auto rest = gcNew<PraiaArray>();
        // Extra args are at the top of the stack
        for (int i = extraCount - 1; i >= 0; i--)
            rest->elements.push_back(stack[stackTop - 1 - i]);
        // Pop the extra args
        stackTop -= extraCount;
        argCount -= extraCount;
        // Push rest array in place of the extra args
        push(Value(rest));
        argCount++; // rest param occupies one slot
    }

    if (frameCount >= FRAMES_MAX) {
        throw RuntimeError("Stack overflow (too many nested calls)", line);
    }

    auto& frame = frames[frameCount++];
    frame.closure = closure;
    frame.function = nullptr;
    frame.ip = fn->chunk.code.data();
    frame.baseSlot = stackTop - argCount - 1; // -1 for the closure itself on stack
    frame.missingArgsMask = missingMask;
    frame.definingClass = nullptr;
    return true;
}

bool VM::callValue(Value callee, int argCount, int line) {
    if (callee.isCallable()) {
        auto callable = callee.asCallable();

      try {
        // VM closure
        auto* vmClosure = dynamic_cast<VMClosureCallable*>(callable.get());
        if (vmClosure) {
            if (vmClosure->closure->function->isGenerator) {
                // Create generator object instead of executing
                auto fn = vmClosure->closure->function;
                int arity = fn->arity;
                if (argCount > arity)
                    throw RuntimeError(fn->name + "() expected at most " + std::to_string(arity) +
                        " " + argStr(arity) + " but got " + std::to_string(argCount), line);
                while (argCount < arity) { push(Value()); argCount++; }

                auto gen = gcNew<PraiaGenerator>();
                gen->isVM = true;
                gen->state = PraiaGenerator::State::CREATED;

                // Save initial state: slot 0 = callee, then args
                int baseSlot = stackTop - argCount - 1;
                gen->savedStack.clear();
                for (int i = baseSlot; i < stackTop; i++)
                    gen->savedStack.push_back(stack[i]);
                gen->savedIp = fn->chunk.code.data();
                gen->savedFrameCount = 1;
                gen->savedBaseSlot = 0;

                // Save closure pointer for frame setup during .next()
                gen->vmClosure = vmClosure->closure;

                // Pop args + callee, push generator
                stackTop = baseSlot;
                push(Value(gen));
                return true;
            }
            return callClosure(vmClosure->closure, argCount, line);
        }

        // Bound method (instance.method())
        auto* bound = dynamic_cast<VMBoundMethod*>(callable.get());
        if (bound) {
            // Replace the callee slot with the receiver (this)
            stack[stackTop - argCount - 1] = bound->receiver;
            callClosure(bound->method, argCount, line);
            // Tag the new frame with the defining class for super resolution
            frames[frameCount - 1].definingClass = bound->definingClass;
            return true;
        }

        // PraiaClass (instantiation)
        auto* klass = dynamic_cast<PraiaClass*>(callable.get());
        if (klass) {
            auto instance = gcNew<PraiaInstance>();
            instance->klass = std::dynamic_pointer_cast<PraiaClass>(callable);

            // Replace callee with the new instance
            stack[stackTop - argCount - 1] = Value(instance);

            // Call init if it exists — walk the class chain to find it
            std::shared_ptr<PraiaClass> initOwner;
            auto walkKlass = std::dynamic_pointer_cast<PraiaClass>(callable);
            while (walkKlass) {
                if (walkKlass->vmMethods.count("init")) { initOwner = walkKlass; break; }
                walkKlass = walkKlass->superclass;
            }
            if (initOwner) {
                auto& initVal = initOwner->vmMethods["init"];
                if (initVal.isCallable()) {
                    auto* initVmcc = dynamic_cast<VMClosureCallable*>(initVal.asCallable().get());
                    if (initVmcc) {
                        callClosure(initVmcc->closure, argCount, line);
                        frames[frameCount - 1].definingClass = initOwner;
                        return true;
                    }
                }
            } else if (argCount > 0) {
                throw RuntimeError(klass->className + "() takes no arguments (no init method)", line);
            }
            return true;
        }

        // Native function
        auto* native = dynamic_cast<NativeFunction*>(callable.get());
        if (native) {
            int arity = native->arity();
            if (arity != -1 && argCount != arity) {
                throw RuntimeError(native->name() + "() expected " + std::to_string(arity) +
                    " " + argStr(arity) + " but got " + std::to_string(argCount), line);
            }
            std::vector<Value> args(argCount);
            for (int i = argCount - 1; i >= 0; i--) args[i] = pop();
            pop(); // the callable
            // Defense-in-depth: any stdlib exception that escapes the
            // native (std::invalid_argument from stoi, std::bad_alloc,
            // std::regex_error, etc.) becomes a RuntimeError. Without
            // this, those abort the process. RuntimeError passes through
            // unchanged so line/column survive.
            Value result;
            try {
                result = native->fn(args);
            } catch (const RuntimeError&) {
                throw;
            } catch (const std::exception& e) {
                throw RuntimeError(native->name() + "(): " + e.what(), line);
            }
            push(std::move(result));
            return true;
        }

        throw RuntimeError("Can only call functions", line);

      } catch (const ExitSignal&) {
          throw; // propagate sys.exit() to main
      } catch (const RuntimeError& err) {
          if (tryHandleError(Value(std::string(err.what())))) return true;
          runtimeError(err.what(), err.line > 0 ? err.line : line);
          return false;
      }
    }

    // Not callable at all (e.g., 42())
    if (tryHandleError(Value(std::string("Can only call functions")))) return true;
    runtimeError("Can only call functions", line);
    return false;
}

void VM::runDefers(int frameIdx) {
    auto defers = std::move(deferStack[frameIdx]);
    deferStack[frameIdx].clear();
    // Save VM state, run defers on the same VM (re-entrant but safe because
    // result is popped before we get here), then let OP_RETURN restore.
    // Errors in one defer must not stop the rest from running, but we surface
    // each error to stderr so failures aren't silent.
    for (int i = static_cast<int>(defers.size()) - 1; i >= 0; i--) {
        int savedTop = stackTop;
        int savedFrameCount = frameCount;
        std::string errMsg;
        bool errored = false;
        try {
            if (defers[i].isCallable()) {
                push(defers[i]);
                if (callValue(defers[i], 0, 0)) {
                    if (execute(frameCount) == Result::RUNTIME_ERROR) {
                        errored = true;
                        errMsg = lastError_;
                    }
                } else {
                    errored = true;
                    errMsg = lastError_;
                }
            }
        } catch (const ExitSignal&) {
            // sys.exit() must propagate even from a defer.
            stackTop = savedTop;
            frameCount = savedFrameCount;
            throw;
        } catch (const std::exception& e) {
            errored = true;
            errMsg = e.what();
        } catch (...) {
            errored = true;
            errMsg = "unknown exception";
        }
        // Restore stack — deferred function's return might have changed stackTop
        stackTop = savedTop;
        frameCount = savedFrameCount;
        if (errored && !suppressErrors_) {
            std::cerr << "Error in deferred call: "
                      << (errMsg.empty() ? "(no message)" : errMsg) << std::endl;
        }
    }
}

void VM::runtimeError(const std::string& msg, int line, int column) {
    lastError_ = msg;
    // In re-entrant calls (depth > 1), suppress output — the error
    // will propagate to the outer scope which will print or catch it.
    if (!suppressErrors_ && executeDepth_ <= 1) {
        std::cerr << formatLocation(line, column) << " Runtime error: " << msg << std::endl;
        std::cerr << formatStackTrace();
    }
}

bool VM::tryHandleError(Value error) {
    // Only use handlers that belong to the current execute() scope.
    // Handlers below executeFloor_ belong to an outer (re-entrant) caller.
    if (static_cast<int>(exceptionHandlers.size()) > executeFloor_) {
        auto handler = exceptionHandlers.back();
        exceptionHandlers.pop_back();

        while (frameCount - 1 > handler.frameIndex) {
            runDefers(frameCount - 1);
            closeUpvalues(&stack[frames[frameCount - 1].baseSlot]);
            frameCount--;
        }

        stackTop = handler.stackTop;
        push(error);
        frames[frameCount - 1].ip = handler.catchIp;
        return true; // caught
    }
    return false; // uncaught in this scope
}

std::string VM::formatStackTrace() const {
    std::string trace;
    for (int i = frameCount - 1; i >= 0; i--) {
        auto& frame = frames[i];
        int offset = static_cast<int>(frame.ip - frame.chunk().code.data()) - 1;
        int safeOffset = offset > 0 ? offset : 0;
        int line = frame.chunk().getLine(safeOffset);
        int col = frame.chunk().getColumn(safeOffset);
        trace += "  at " + frame.name() + "() " + formatLocation(line, col) + "\n";
    }
    return trace;
}

// ── Module loading ──────────────────────────────────────────

std::string VM::resolveGrainPath(const std::string& path, int line) {
    try {
        return ::resolveGrainPath(path, currentFile);
    } catch (const std::runtime_error& e) {
        runtimeError(e.what(), line);
        return "";
    }
}

Value VM::loadGrain(const std::string& importPath, int line) {
    std::string resolved = resolveGrainPath(importPath, line);
    if (resolved.empty()) return Value();

    if (importedInCurrentFile.count(resolved)) {
        runtimeError("Grain '" + importPath + "' is already imported in this file", line);
        return Value();
    }
    importedInCurrentFile.insert(resolved);

    auto cached = grainCache.find(resolved);
    if (cached != grainCache.end()) return cached->second;

    // Read source
    std::ifstream f(resolved);
    if (!f.is_open()) { runtimeError("Cannot read grain: " + resolved, line); return Value(); }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();

    // Lex + parse
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    if (lexer.hasError()) { runtimeError("Syntax error in grain: " + importPath, line); return Value(); }

    Parser parser(tokens);
    auto program = parser.parse();
    if (parser.hasError()) { runtimeError("Parse error in grain: " + importPath, line); return Value(); }

    // Compile
    Compiler compiler;
    auto script = compiler.compile(program);
    if (!script) { runtimeError("Compile error in grain: " + importPath, line); return Value(); }

    // Execute in a persistent per-grain VM with only builtins (not user
    // globals). The grain VM outlives this function — it's stashed in
    // `grainVMs` so its `globals[]` keep holding the module's `let`
    // bindings. Exported closures resolve their OP_*_GLOBAL through
    // Chunk::homeVm = &grainVm (set below), so two grains that both
    // define `let state = ...` see independent values.
    auto grainVmPtr = std::make_unique<VM>();
    VM& grainVm = *grainVmPtr;
    for (auto& name : builtinNames_) {
        int parentSlot = findGlobalSlot(name);
        if (parentSlot >= 0) {
            int childSlot = grainVm.ensureGlobalSlot(name);
            grainVm.globals[childSlot] = globals[parentSlot];
        }
    }
    grainVm.builtinNames_ = builtinNames_;
    grainVm.currentFile = resolved;

    // Tag every CompiledFunction reachable from the grain's script (the
    // top-level chunk and any nested function constants) so all of the
    // grain's bytecode resolves globals against grainVm — including
    // closures that escape into parent globals via the export map.
    std::function<void(CompiledFunction*, std::unordered_set<CompiledFunction*>&)> tagHome;
    tagHome = [&](CompiledFunction* fn, std::unordered_set<CompiledFunction*>& seen) {
        if (!fn || !seen.insert(fn).second) return;
        fn->chunk.homeVm = &grainVm;
        for (auto& c : fn->chunk.constants) {
            if (c.isCallable()) {
                auto* vmcc = dynamic_cast<VMClosureCallable*>(c.asCallable().get());
                if (vmcc && vmcc->closure && vmcc->closure->function)
                    tagHome(vmcc->closure->function.get(), seen);
            }
        }
    };
    {
        std::unordered_set<CompiledFunction*> seen;
        tagHome(script.get(), seen);
    }

    grainVm.allClosures.push_back(std::make_unique<ObjClosure>(script));
    auto* grainClosure = grainVm.allClosures.back().get();

    auto wrapper = std::make_shared<VMClosureCallable>(grainClosure);
    wrapper->vm = &grainVm;
    grainVm.push(Value(std::static_pointer_cast<Callable>(wrapper)));

    grainVm.frames[0].closure = grainClosure;
    grainVm.frames[0].function = script;
    grainVm.frames[0].ip = script->chunk.code.data();
    grainVm.frames[0].baseSlot = 0;
    grainVm.frames[0].missingArgsMask = 0;
    grainVm.frameCount = 1;

    auto result = grainVm.execute();

    // The grain should have ended with OP_EXPORT which pushes an exports map
    // and returns. Check if there's a result.
    Value exports;
    if (result == Result::OK && grainVm.stackTop > 0) {
        exports = grainVm.stack[grainVm.stackTop - 1];
    } else {
        exports = Value(gcNew<PraiaMap>()); // empty exports
    }

    // Keep the grain's AST alive (chunks reference its string storage).
    grainAsts.push_back(std::move(program));

    // Hand the grain VM off to the parent. It (and all of its closures,
    // upvalues, and globals) lives as long as the parent does. Chunks
    // already point at &grainVm via homeVm; the unique_ptr below is just
    // the lifetime anchor.
    grainVMs.push_back(std::move(grainVmPtr));

    grainCache[resolved] = exports;
    return exports;
}

VM::Result VM::run(std::shared_ptr<CompiledFunction> script) {
    GcHeap::current().setRootMarker([this](GcHeap& h) { gcMarkRoots(h); });

    // Create a closure for the top-level script
    allClosures.push_back(std::make_unique<ObjClosure>(script));
    auto* scriptClosure = allClosures.back().get();

    auto wrapper = std::make_shared<VMClosureCallable>(scriptClosure);
    wrapper->vm = this;
    push(Value(std::static_pointer_cast<Callable>(wrapper)));

    frames[0].closure = scriptClosure;
    frames[0].function = script;
    frames[0].ip = script->chunk.code.data();
    frames[0].baseSlot = 0;
    frames[0].missingArgsMask = 0;
    frameCount = 1;

    return execute();
}

VM::Result VM::runRepl(std::shared_ptr<CompiledFunction> script) {
    GcHeap::current().setRootMarker([this](GcHeap& h) { gcMarkRoots(h); });

    // Reset stack/frames/handlers for this line but keep globals
    stackTop = 0;
    frameCount = 0;
    exceptionHandlers.clear();

    allClosures.push_back(std::make_unique<ObjClosure>(script));
    auto* scriptClosure = allClosures.back().get();

    auto wrapper = std::make_shared<VMClosureCallable>(scriptClosure);
    wrapper->vm = this;
    push(Value(std::static_pointer_cast<Callable>(wrapper)));

    frames[0].closure = scriptClosure;
    frames[0].function = script;
    frames[0].ip = script->chunk.code.data();
    frames[0].baseSlot = 0;
    frames[0].missingArgsMask = 0;
    frames[0].definingClass = nullptr;
    frameCount = 1;

    return execute();
}

VM::Result VM::execute(int baseFrameCount_) {
    VMScope vmScope(this); // set thread-local current VM, restores on return

    #define FRAME (frames[frameCount - 1])
    #define READ_BYTE() (*FRAME.ip++)
    #define READ_U16() (FRAME.ip += 2, static_cast<uint16_t>((FRAME.ip[-2] << 8) | FRAME.ip[-1]))
    #define READ_CONSTANT() (FRAME.chunk().constants[READ_U16()])
    #define READ_STRING() (READ_CONSTANT().asString())
    #define CURRENT_OFFSET() (static_cast<int>(FRAME.ip - FRAME.chunk().code.data()) - 1)
    #define CURRENT_LINE() (FRAME.chunk().getLine(CURRENT_OFFSET()))
    #define CURRENT_COLUMN() (FRAME.chunk().getColumn(CURRENT_OFFSET()))
    #define RUNTIME_ERR(msg) { \
        std::string _msg = (msg); int _line = CURRENT_LINE(); int _col = CURRENT_COLUMN(); \
        if (tryHandleError(Value(_msg))) continue; \
        runtimeError(_msg, _line, _col); return Result::RUNTIME_ERROR; \
    }

    try {
    for (;;) {
        if (--gcCounter_ <= 0) {
            gcCounter_ = 1024;
            GcHeap::current().collectIfNeeded();
            // Check for pending SIGINT — makes Ctrl+C catchable by try/catch
            if (g_pendingSignals.load(std::memory_order_relaxed) & (1u << SIGINT)) {
                g_pendingSignals.fetch_and(~(1u << SIGINT));
                bool hasUserHandler = false;
                {
                    std::lock_guard<std::mutex> lock(g_signalMutex);
                    hasUserHandler = g_signalHandlers.count(SIGINT) > 0;
                }
                if (!hasUserHandler) {
                    if (tryHandleError(Value(std::string("Interrupted")))) continue;
                    runtimeError("Interrupted", CURRENT_LINE(), CURRENT_COLUMN());
                    return Result::RUNTIME_ERROR;
                }
                // User handler exists — handled by sys.checkSignals()
            }
        }
        uint8_t instruction = READ_BYTE();

        switch (static_cast<OpCode>(instruction)) {

        case OpCode::OP_CONSTANT: push(READ_CONSTANT()); break;
        case OpCode::OP_NIL: push(Value()); break;
        case OpCode::OP_TRUE: push(Value(true)); break;
        case OpCode::OP_FALSE: push(Value(false)); break;
        case OpCode::OP_POP: pop(); break;
        case OpCode::OP_DUP: push(peek()); break;
        case OpCode::OP_SWAP: { Value a = pop(); Value b = pop(); push(a); push(b); break; }
        case OpCode::OP_POPN: {
            uint8_t n = READ_BYTE();
            if (n > stackTop) n = static_cast<uint8_t>(stackTop);
            stackTop -= n;
            break;
        }

        // ── Arithmetic ──
        case OpCode::OP_ADD: {
            Value b = pop(), a = pop();
            if (a.isInstance()) { auto [ok, r] = vmCallDunder(*this, a, "__add", {b}); if (ok) { push(r); break; } }
            if (a.isInt() && b.isInt()) {
                int64_t r; if (!__builtin_add_overflow(a.asInt(), b.asInt(), &r)) { push(Value(r)); break; }
                push(Value(a.asNumber() + b.asNumber())); break;
            }
            if (a.isNumber() && b.isNumber()) { push(Value(a.asNumber() + b.asNumber())); break; }
            if (a.isArray() && b.isArray()) {
                auto r = gcNew<PraiaArray>();
                for (auto& el : a.asArray()->elements) r->elements.push_back(el);
                for (auto& el : b.asArray()->elements) r->elements.push_back(el);
                push(Value(r)); break;
            }
            if (a.isMap() && b.isMap()) {
                auto r = gcNew<PraiaMap>();
                for (auto& [k, v] : a.asMap()->entries) r->entries[k] = v;
                for (auto& [k, v] : b.asMap()->entries) r->entries[k] = v; // b overrides a
                push(Value(r)); break;
            }
            // Set + set = union. Used both at the language level
            // (`#{1, 2} + #{3, 4}`) and by the compiler's spread-
            // lowering for `#{1, ...other}`.
            if (a.isSet() && b.isSet()) {
                auto r = gcNew<PraiaSet>();
                r->elements = a.asSet()->elements;
                for (auto& e : b.asSet()->elements) r->elements.insert(e);
                push(Value(r)); break;
            }
            if (a.isString() || b.isString()) { push(Value(a.toString() + b.toString())); break; }
            RUNTIME_ERR("Operands of '+' must be numbers, strings, arrays, maps, or sets");
        }
        case OpCode::OP_SUBTRACT: {
            Value b = pop(), a = pop();
            if (a.isInstance()) { auto [ok, r] = vmCallDunder(*this, a, "__sub", {b}); if (ok) { push(r); break; } }
            if (a.isInt() && b.isInt()) {
                int64_t r; if (!__builtin_sub_overflow(a.asInt(), b.asInt(), &r)) { push(Value(r)); break; }
                push(Value(a.asNumber() - b.asNumber())); break;
            }
            if (a.isNumber() && b.isNumber()) { push(Value(a.asNumber() - b.asNumber())); break; }
            RUNTIME_ERR("Operands of '-' must be numbers");
        }
        case OpCode::OP_MULTIPLY: {
            Value b = pop(), a = pop();
            if (a.isInstance()) { auto [ok, r] = vmCallDunder(*this, a, "__mul", {b}); if (ok) { push(r); break; } }
            if (a.isInt() && b.isInt()) {
                int64_t r; if (!__builtin_mul_overflow(a.asInt(), b.asInt(), &r)) { push(Value(r)); break; }
                push(Value(a.asNumber() * b.asNumber())); break;
            }
            if (a.isNumber() && b.isNumber()) { push(Value(a.asNumber() * b.asNumber())); break; }
            if ((a.isString() && b.isInt()) || (a.isInt() && b.isString())) {
                auto& str = a.isString() ? a.asString() : b.asString();
                int64_t n = a.isInt() ? a.asInt() : b.asInt();
                if (n < 0) { RUNTIME_ERR("String repeat count cannot be negative"); }
                std::string result;
                result.reserve(str.size() * n);
                for (int64_t i = 0; i < n; i++) result += str;
                push(Value(std::move(result)));
                break;
            }
            RUNTIME_ERR("Operands of '*' must be numbers, or string * int");
        }
        case OpCode::OP_DIVIDE: {
            Value b = pop(), a = pop();
            if (a.isInstance()) { auto [ok, r] = vmCallDunder(*this, a, "__div", {b}); if (ok) { push(r); break; } }
            if (a.isNumber() && b.isNumber()) {
                if (b.asNumber() == 0) { RUNTIME_ERR("Division by zero"); }
                if (a.isInt() && b.isInt()) {
                    int64_t ai = a.asInt(), bi = b.asInt();
                    if (ai % bi == 0) { push(Value(ai / bi)); break; }
                }
                push(Value(a.asNumber() / b.asNumber())); break;
            }
            RUNTIME_ERR("Operands of '/' must be numbers");
        }
        case OpCode::OP_MODULO: {
            Value b = pop(), a = pop();
            if (a.isInstance()) { auto [ok, r] = vmCallDunder(*this, a, "__mod", {b}); if (ok) { push(r); break; } }
            if (a.isInt() && b.isInt()) {
                if (b.asInt() == 0) { RUNTIME_ERR("Modulo by zero"); }
                push(Value(a.asInt() % b.asInt())); break;
            }
            if (a.isNumber() && b.isNumber()) {
                if (b.asNumber() == 0) { RUNTIME_ERR("Modulo by zero"); }
                push(Value(std::fmod(a.asNumber(), b.asNumber()))); break;
            }
            RUNTIME_ERR("Operands of '%' must be numbers");
        }
        case OpCode::OP_NEGATE: {
            if (peek().isInstance()) {
                Value v = pop();
                auto [ok, r] = vmCallDunder(*this, v, "__neg", {});
                if (ok) { push(r); break; }
                push(v);
            }
            if (!peek().isNumber()) { RUNTIME_ERR("Operand of '-' must be a number"); }
            if (peek().isInt()) {
                int64_t v = pop().asInt();
                if (v == INT64_MIN) push(Value(-static_cast<double>(v)));
                else push(Value(-v));
            } else push(Value(-pop().asNumber()));
            break;
        }

        // ── Bitwise (use asInt() directly for int operands to preserve precision) ──
        #define BITWISE_INTS(a, b) \
            (a.isInt() && b.isInt()) ? a.asInt() : static_cast<int64_t>(a.asNumber()), \
            (a.isInt() && b.isInt()) ? b.asInt() : static_cast<int64_t>(b.asNumber())
        case OpCode::OP_BIT_AND: {
            Value b=pop(),a=pop();
            if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '&' must be numbers");}
            int64_t ai = a.isInt() ? a.asInt() : static_cast<int64_t>(a.asNumber());
            int64_t bi = b.isInt() ? b.asInt() : static_cast<int64_t>(b.asNumber());
            push(Value(ai & bi)); break;
        }
        case OpCode::OP_BIT_OR: {
            Value b=pop(),a=pop();
            if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '|' must be numbers");}
            int64_t ai = a.isInt() ? a.asInt() : static_cast<int64_t>(a.asNumber());
            int64_t bi = b.isInt() ? b.asInt() : static_cast<int64_t>(b.asNumber());
            push(Value(ai | bi)); break;
        }
        case OpCode::OP_BIT_XOR: {
            Value b=pop(),a=pop();
            if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '^' must be numbers");}
            int64_t ai = a.isInt() ? a.asInt() : static_cast<int64_t>(a.asNumber());
            int64_t bi = b.isInt() ? b.asInt() : static_cast<int64_t>(b.asNumber());
            push(Value(ai ^ bi)); break;
        }
        case OpCode::OP_BIT_NOT: {
            if(!peek().isNumber()){RUNTIME_ERR("Operand of '~' must be a number");}
            Value v = pop();
            int64_t vi = v.isInt() ? v.asInt() : static_cast<int64_t>(v.asNumber());
            push(Value(~vi)); break;
        }
        case OpCode::OP_SHL: {
            Value b=pop(),a=pop();
            if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '<<' must be numbers");}
            int64_t ai = a.isInt() ? a.asInt() : static_cast<int64_t>(a.asNumber());
            int64_t bi = b.isInt() ? b.asInt() : static_cast<int64_t>(b.asNumber());
            push(Value(ai << bi)); break;
        }
        case OpCode::OP_SHR: {
            Value b=pop(),a=pop();
            if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '>>' must be numbers");}
            int64_t ai = a.isInt() ? a.asInt() : static_cast<int64_t>(a.asNumber());
            int64_t bi = b.isInt() ? b.asInt() : static_cast<int64_t>(b.asNumber());
            push(Value(ai >> bi)); break;
        }
        #undef BITWISE_INTS

        // ── Comparison ──
        case OpCode::OP_EQUAL:         { Value b=pop(),a=pop(); if(a.isInstance()){auto[ok,r]=vmCallDunder(*this,a,"__eq",{b});if(ok){push(r);break;}} push(Value(a==b)); break; }
        case OpCode::OP_NOT_EQUAL:     { Value b=pop(),a=pop(); if(a.isInstance()){auto[ok,r]=vmCallDunder(*this,a,"__eq",{b});if(ok){push(Value(!r.isTruthy()));break;}} push(Value(a!=b)); break; }
        case OpCode::OP_IS: {
            Value right = pop();
            Value left = pop();
            if (right.isString()) {
                auto& tn = right.asString();
                bool result = false;
                if      (tn == "nil")       result = left.isNil();
                else if (tn == "bool")      result = left.isBool();
                else if (tn == "int")       result = left.isInt();
                else if (tn == "float")     result = left.isDouble();
                else if (tn == "string")    result = left.isString();
                else if (tn == "array")     result = left.isArray();
                else if (tn == "map")       result = left.isMap();
                else if (tn == "set")       result = left.isSet();
                else if (tn == "function")  result = left.isCallable();
                else if (tn == "instance")  result = left.isInstance();
                else if (tn == "tagged")    result = left.isTagged();
                else if (tn == "future")    result = left.isFuture();
                else if (tn == "generator") result = left.isGenerator();
                else { RUNTIME_ERR("Unknown type name '" + tn + "'"); }
                push(Value(result));
            } else if (right.isCallable()) {
                auto klass = std::dynamic_pointer_cast<PraiaClass>(right.asCallable());
                if (!klass) { RUNTIME_ERR("'is' requires a class or type name string"); }
                if (!left.isInstance()) { push(Value(false)); break; }
                auto walk = left.asInstance()->klass;
                while (walk) {
                    if (walk == klass) { push(Value(true)); break; }
                    walk = walk->superclass;
                }
                if (!walk) push(Value(false));
            } else {
                RUNTIME_ERR("'is' requires a type name string or class");
            }
            break;
        }
        case OpCode::OP_LESS:          { Value b=pop(),a=pop(); if(a.isInstance()){auto[ok,r]=vmCallDunder(*this,a,"__lt",{b});if(ok){push(r);break;}} if(a.isString()&&b.isString()){push(Value(a.asString()<b.asString()));break;} if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '<' must be numbers or strings");} push(Value(numbersLess(a, b))); break; }
        case OpCode::OP_GREATER:       { Value b=pop(),a=pop(); if(a.isInstance()){auto[ok,r]=vmCallDunder(*this,a,"__gt",{b});if(ok){push(r);break;}} if(a.isString()&&b.isString()){push(Value(a.asString()>b.asString()));break;} if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '>' must be numbers or strings");} push(Value(numbersLess(b, a))); break; }
        case OpCode::OP_LESS_EQUAL:    {
            Value b=pop(),a=pop();
            if(a.isInstance()){
                // Prefer __le; fall back to !__gt
                auto[ok,r]=vmCallDunder(*this,a,"__le",{b});
                if(ok){push(r);break;}
                auto[ok2,r2]=vmCallDunder(*this,a,"__gt",{b});
                if(ok2){push(Value(!r2.isTruthy()));break;}
            }
            if(a.isString()&&b.isString()){push(Value(a.asString()<=b.asString()));break;}
            if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '<=' must be numbers or strings");}
            push(Value(!numbersLess(b, a))); break;
        }
        case OpCode::OP_GREATER_EQUAL: {
            Value b=pop(),a=pop();
            if(a.isInstance()){
                // Prefer __ge; fall back to !__lt
                auto[ok,r]=vmCallDunder(*this,a,"__ge",{b});
                if(ok){push(r);break;}
                auto[ok2,r2]=vmCallDunder(*this,a,"__lt",{b});
                if(ok2){push(Value(!r2.isTruthy()));break;}
            }
            if(a.isString()&&b.isString()){push(Value(a.asString()>=b.asString()));break;}
            if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '>=' must be numbers or strings");}
            push(Value(!numbersLess(a, b))); break;
        }
        case OpCode::OP_NOT: { push(Value(!pop().isTruthy())); break; }

        // ── Variables ──
        // OP_*_GLOBAL operands are constant pool indices (the global's name
        // stored as a string). The chunk's `globalSlotCache[constIdx]` is
        // a per-call-site inline cache: -1 on first hit, the resolved slot
        // thereafter, so subsequent accesses skip the string hash and go
        // straight to `globals[slot]`.
        //
        // For chunks owned by a grain (chunk.homeVm != nullptr), all
        // resolution goes through the grain's VM so module-level lets
        // stay isolated across grains — see VM::loadGrain.
        case OpCode::OP_DEFINE_GLOBAL: {
            uint16_t constIdx = READ_U16();
            auto& chunk = FRAME.chunk();
            VM* gvm = chunk.homeVm ? chunk.homeVm : this;
            int slot = chunk.globalSlotCache[constIdx];
            if (slot < 0) {
                slot = gvm->ensureGlobalSlot(chunk.constants[constIdx].asString());
                chunk.globalSlotCache[constIdx] = slot;
            }
            gvm->globals[slot] = pop();
            // Overwrite wins over any pending lazy snapshot — mark loaded.
            if (gvm->isTaskVm_) gvm->loadedMask_[slot] = 1;
            break;
        }
        case OpCode::OP_GET_GLOBAL: {
            uint16_t constIdx = READ_U16();
            auto& chunk = FRAME.chunk();
            VM* gvm = chunk.homeVm ? chunk.homeVm : this;
            int slot = chunk.globalSlotCache[constIdx];
            if (slot < 0) {
                const std::string& n = chunk.constants[constIdx].asString();
                slot = gvm->findGlobalSlot(n);
                if (slot < 0) { RUNTIME_ERR("Undefined variable '" + n + "'"); }
                chunk.globalSlotCache[constIdx] = slot;
            }
            if (gvm->isTaskVm_ && !gvm->loadedMask_[slot]) gvm->lazyLoadGlobal(slot);
            push(gvm->globals[slot]);
            break;
        }
        case OpCode::OP_SET_GLOBAL: {
            uint16_t constIdx = READ_U16();
            auto& chunk = FRAME.chunk();
            VM* gvm = chunk.homeVm ? chunk.homeVm : this;
            int slot = chunk.globalSlotCache[constIdx];
            if (slot < 0) {
                const std::string& n = chunk.constants[constIdx].asString();
                slot = gvm->findGlobalSlot(n);
                if (slot < 0) { RUNTIME_ERR("Undefined variable '" + n + "'"); }
                chunk.globalSlotCache[constIdx] = slot;
            }
            gvm->globals[slot] = peek();
            if (gvm->isTaskVm_) gvm->loadedMask_[slot] = 1;
            break;
        }
        case OpCode::OP_GET_LOCAL: { uint16_t slot = READ_U16(); push(stack[FRAME.baseSlot + slot]); break; }
        case OpCode::OP_SET_LOCAL: { uint16_t slot = READ_U16(); stack[FRAME.baseSlot + slot] = peek(); break; }

        // ── Function prologue ──
        case OpCode::OP_IS_MISSING_ARG: {
            uint16_t slot = READ_U16();
            // slot is 1-indexed (slot 0 = closure itself). Param index = slot - 1.
            // Bit i in missingArgsMask is set iff param i was not provided.
            int paramIdx = static_cast<int>(slot) - 1;
            bool missing = (paramIdx < 64) &&
                ((FRAME.missingArgsMask >> paramIdx) & 1ULL) != 0;
            push(Value(missing));
            break;
        }

        // ── Postfix ──
        case OpCode::OP_POST_INC_LOCAL: {
            uint16_t slot = READ_U16();
            Value& val = stack[FRAME.baseSlot + slot];
            if (!val.isNumber()) { RUNTIME_ERR("Postfix operator requires a number"); }
            push(val);
            if (val.isInt()) { int64_t r; if (!__builtin_add_overflow(val.asInt(), (int64_t)1, &r)) val = Value(r); else val = Value(val.asNumber() + 1); }
            else val = Value(val.asNumber() + 1);
            break;
        }
        case OpCode::OP_POST_DEC_LOCAL: {
            uint16_t slot = READ_U16();
            Value& val = stack[FRAME.baseSlot + slot];
            if (!val.isNumber()) { RUNTIME_ERR("Postfix operator requires a number"); }
            push(val);
            if (val.isInt()) { int64_t r; if (!__builtin_sub_overflow(val.asInt(), (int64_t)1, &r)) val = Value(r); else val = Value(val.asNumber() - 1); }
            else val = Value(val.asNumber() - 1);
            break;
        }
        case OpCode::OP_POST_INC_GLOBAL:
        case OpCode::OP_POST_DEC_GLOBAL: {
            uint16_t constIdx = READ_U16();
            auto& chunk = FRAME.chunk();
            VM* gvm = chunk.homeVm ? chunk.homeVm : this;
            int slot = chunk.globalSlotCache[constIdx];
            if (slot < 0) {
                const std::string& n = chunk.constants[constIdx].asString();
                slot = gvm->findGlobalSlot(n);
                if (slot < 0) { RUNTIME_ERR("Undefined variable '" + n + "'"); }
                chunk.globalSlotCache[constIdx] = slot;
            }
            if (gvm->isTaskVm_ && !gvm->loadedMask_[slot]) gvm->lazyLoadGlobal(slot);
            Value& val = gvm->globals[slot];
            if (!val.isNumber()) { RUNTIME_ERR("Postfix operator requires a number"); }
            push(val);
            bool inc = static_cast<OpCode>(instruction) == OpCode::OP_POST_INC_GLOBAL;
            if (val.isInt()) {
                int64_t r; int64_t delta = inc ? 1 : -1;
                bool ov = inc ? __builtin_add_overflow(val.asInt(), (int64_t)1, &r)
                              : __builtin_sub_overflow(val.asInt(), (int64_t)1, &r);
                if (!ov) val = Value(r); else val = Value(val.asNumber() + delta);
            } else val = Value(val.asNumber() + (inc ? 1 : -1));
            break;
        }
        case OpCode::OP_POST_INC_UPVALUE:
        case OpCode::OP_POST_DEC_UPVALUE: {
            uint16_t slot = READ_U16();
            Value& val = *FRAME.closure->upvalues[slot]->location;
            if (!val.isNumber()) { RUNTIME_ERR("Postfix operator requires a number"); }
            push(val);
            bool inc = static_cast<OpCode>(instruction) == OpCode::OP_POST_INC_UPVALUE;
            if (val.isInt()) {
                int64_t r; int64_t delta = inc ? 1 : -1;
                bool ov = inc ? __builtin_add_overflow(val.asInt(), (int64_t)1, &r)
                              : __builtin_sub_overflow(val.asInt(), (int64_t)1, &r);
                if (!ov) val = Value(r); else val = Value(val.asNumber() + delta);
            } else val = Value(val.asNumber() + (inc ? 1 : -1));
            break;
        }

        // ── Control flow ──
        case OpCode::OP_JUMP: { uint16_t off = READ_U16(); FRAME.ip += off; break; }
        case OpCode::OP_JUMP_BACK: { uint16_t off = READ_U16(); FRAME.ip -= off; break; }
        case OpCode::OP_JUMP_IF_FALSE: { uint16_t off = READ_U16(); if (!peek().isTruthy()) FRAME.ip += off; break; }
        case OpCode::OP_JUMP_IF_TRUE: { uint16_t off = READ_U16(); if (peek().isTruthy()) FRAME.ip += off; break; }
        case OpCode::OP_JUMP_IF_NOT_NIL: { uint16_t off = READ_U16(); if (!peek().isNil()) FRAME.ip += off; break; }
        case OpCode::OP_POP_JUMP_IF_FALSE: { uint16_t off = READ_U16(); if (!pop().isTruthy()) FRAME.ip += off; break; }

        // ── Functions ──
        case OpCode::OP_CALL: {
            uint8_t argc = READ_BYTE();
            Value callee = peek(argc);
            if (!callValue(callee, argc, CURRENT_LINE())) return Result::RUNTIME_ERROR;
            break;
        }

        case OpCode::OP_CALL_SPREAD: {
            // Stack: [callee, argsArray]
            Value argsArray = pop();
            if (!argsArray.isArray()) { RUNTIME_ERR("Spread call requires array of arguments"); }
            auto& elems = argsArray.asArray()->elements;
            // Push each element as an argument
            for (auto& elem : elems) push(elem);
            int argc = static_cast<int>(elems.size());
            Value callee = peek(argc);
            if (!callValue(callee, argc, CURRENT_LINE())) return Result::RUNTIME_ERROR;
            break;
        }

        case OpCode::OP_CALL_NAMED: {
            uint8_t argc = READ_BYTE();
            uint16_t namesIdx = READ_U16();
            Value namesVal = FRAME.chunk().constants[namesIdx];
            auto& namesArr = namesVal.asArray()->elements;
            Value callee = peek(argc);

            if (!callee.isCallable()) { RUNTIME_ERR("Can only call functions"); }
            const auto* params = callee.asCallable()->paramNames();
            if (!params) { RUNTIME_ERR("Named arguments not supported for '" + callee.asCallable()->name() + "'"); }

            int paramCount = static_cast<int>(params->size());
            // Pop args from stack (in reverse order)
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; i--) args[i] = pop();
            pop(); // pop callee

            // Reorder named args to match parameter positions
            std::vector<Value> reordered(paramCount);
            std::vector<bool> filled(paramCount, false);
            int positionalIdx = 0;
            for (int i = 0; i < argc; i++) {
                std::string name = namesArr[i].asString();
                if (name.empty()) {
                    if (positionalIdx >= paramCount) { RUNTIME_ERR(callee.asCallable()->name() + "() too many arguments"); }
                    reordered[positionalIdx] = args[i];
                    filled[positionalIdx] = true;
                    positionalIdx++;
                } else {
                    int found = -1;
                    for (int p = 0; p < paramCount; p++) {
                        if ((*params)[p] == name) { found = p; break; }
                    }
                    if (found == -1) { RUNTIME_ERR(callee.asCallable()->name() + "() unknown parameter '" + name + "'"); }
                    if (filled[found]) { RUNTIME_ERR(callee.asCallable()->name() + "() parameter '" + name + "' specified twice"); }
                    reordered[found] = args[i];
                    filled[found] = true;
                }
            }

            // Push callee back + reordered args
            push(callee);
            for (auto& a : reordered) push(a);
            int prevFrameCount = frameCount;
            if (!callValue(callee, paramCount, CURRENT_LINE())) return Result::RUNTIME_ERROR;
            // If a Praia frame was pushed, override its missing mask so that
            // omitted named-arg positions (which we filled with nil placeholders)
            // are treated as missing for default evaluation.
            if (frameCount > prevFrameCount) {
                uint64_t mask = 0;
                int n = std::min(paramCount, 64);
                for (int p = 0; p < n; p++) {
                    if (!filled[p]) mask |= (uint64_t)1 << p;
                }
                frames[frameCount - 1].missingArgsMask = mask;
            }
            break;
        }

        case OpCode::OP_RETURN: {
            Value result = pop();

            // Run deferred calls for this frame (uses tree-walker, no stack issues)
            runDefers(frameCount - 1);

            int returnBase = FRAME.baseSlot;
            closeUpvalues(&stack[returnBase]);

            // Generator return: if we're returning from the generator's base frame,
            // mark it completed and return {value, done: true}
            if (currentGenerator_ && frameCount - 1 == genBaseFrame_) {
                currentGenerator_->state = PraiaGenerator::State::COMPLETED;
                frameCount = genBaseFrame_;
                stackTop = genBaseStackTop_;
                auto doneResult = gcNew<PraiaMap>();
                doneResult->entries[Value("value")] = result;
                doneResult->entries[Value("done")] = Value(true);
                push(Value(doneResult));
                return Result::OK;
            }

            // Safety net: remove any exception handlers belonging to this frame
            // (compiler emits OP_TRY_END for return/break/continue, but guard
            // against edge cases where a handler leaks)
            int returningFrame = frameCount - 1;
            while (!exceptionHandlers.empty() &&
                   exceptionHandlers.back().frameIndex >= returningFrame) {
                exceptionHandlers.pop_back();
            }
            frameCount--;
            if (frameCount <= baseFrameCount_) {
                if (frameCount == 0) pop();
                stackTop = returnBase;
                push(std::move(result));
                return Result::OK;
            }
            stackTop = returnBase;
            push(std::move(result));
            break;
        }

        case OpCode::OP_CLOSURE: {
            auto fnVal = READ_CONSTANT();
            // The constant should be a CompiledFunction wrapped in a Value
            // We stored it as a Callable (VMClosureCallable) during compilation... no.
            // Actually, the compiler stores the CompiledFunction directly.
            // We need to extract it. Let's use a convention: closures store
            // the CompiledFunction as a special map with __fn field, or we
            // add a helper. Simplest: store CompiledFunction as a Callable wrapper.

            // For now: the compiler emits the function index, the constant is a
            // string marker. Let me redesign: the compiler should store the
            // CompiledFunction ptr directly in the constant pool.
            // Since Value can't hold CompiledFunction, I'll use a side channel.

            // Alternative approach: the compiler creates the closure value at compile time
            // and stores it as a Callable in the constant pool. The OP_CLOSURE opcode
            // just reads it and sets up upvalues.

            // Let me use the simplest working approach:
            // The compiler stores a VMClosureCallable in the constant pool.
            // OP_CLOSURE reads it, creates a new ObjClosure with upvalues.

            if (!fnVal.isCallable()) {
                RUNTIME_ERR("Internal error: OP_CLOSURE constant is not a function");
            }
            auto* vmcc = dynamic_cast<VMClosureCallable*>(fnVal.asCallable().get());
            if (!vmcc) {
                RUNTIME_ERR("Internal error: OP_CLOSURE constant is not a VM closure");
            }

            // Construct-before-publish: build the closure on a local
            // unique_ptr and fully populate its upvalues before handing
            // ownership to allClosures. captureUpvalue's only realistic
            // throw is std::bad_alloc; if that fires mid-loop, the local
            // unique_ptr's destructor cleans up and no half-initialized
            // closure ever enters allClosures. unique_ptr's move ctor is
            // noexcept, so push_back has the strong exception guarantee.
            auto pending = std::make_unique<ObjClosure>(vmcc->closure->function);
            for (int i = 0; i < pending->upvalueCount; i++) {
                uint8_t isLocal = READ_BYTE();
                uint16_t index = READ_U16();
                if (isLocal) {
                    pending->upvalues[i] = captureUpvalue(&stack[FRAME.baseSlot + index]);
                } else {
                    pending->upvalues[i] = FRAME.closure->upvalues[index];
                }
            }

            allClosures.push_back(std::move(pending));
            auto* closure = allClosures.back().get();
            auto wrapper = std::make_shared<VMClosureCallable>(closure);
            wrapper->vm = this;
            push(Value(std::static_pointer_cast<Callable>(wrapper)));
            break;
        }

        case OpCode::OP_GET_UPVALUE: {
            uint16_t slot = READ_U16();
            push(*FRAME.closure->upvalues[slot]->location);
            break;
        }

        case OpCode::OP_SET_UPVALUE: {
            uint16_t slot = READ_U16();
            *FRAME.closure->upvalues[slot]->location = peek();
            break;
        }

        case OpCode::OP_CLOSE_UPVALUE: {
            closeUpvalues(&stack[stackTop - 1]);
            pop();
            break;
        }

        // ── Classes ──
        case OpCode::OP_CLASS: {
            std::string name = READ_STRING();
            auto klass = gcNew<PraiaClass>();
            klass->className = name;
            push(Value(std::static_pointer_cast<Callable>(klass)));
            break;
        }

        case OpCode::OP_METHOD: {
            std::string name = READ_STRING();
            Value method = pop(); // the closure
            Value& klass = peek(); // the class

            auto klassPtr = std::dynamic_pointer_cast<PraiaClass>(klass.asCallable());
            if (!klassPtr) { RUNTIME_ERR("OP_METHOD: not a class"); }

            // Store the closure as a method — the VM will bind it when accessed
            klassPtr->vmMethods[name] = method;
            // Track dunder presence for the operator-dispatch fast path.
            if (name.size() >= 2 && name[0] == '_' && name[1] == '_')
                klassPtr->hasOperatorOverloads = true;
            break;
        }

        case OpCode::OP_METHOD_DECORATOR: {
            std::string name = READ_STRING();
            Value decorator = pop();
            Value& klass = peek();
            auto klassPtr = std::dynamic_pointer_cast<PraiaClass>(klass.asCallable());
            if (!klassPtr) { RUNTIME_ERR("OP_METHOD_DECORATOR: not a class"); }
            klassPtr->methodDecorators[name].push_back(decorator);
            break;
        }

        case OpCode::OP_STATIC_METHOD: {
            std::string name = READ_STRING();
            Value method = pop();
            Value& klass = peek();

            auto klassPtr = std::dynamic_pointer_cast<PraiaClass>(klass.asCallable());
            if (!klassPtr) { RUNTIME_ERR("OP_STATIC_METHOD: not a class"); }

            klassPtr->vmStaticMethods[name] = method;
            break;
        }

        case OpCode::OP_INHERIT: {
            Value subclass = pop();
            Value superclass = peek(); // leave superclass on stack? no, pop both

            auto superPtr = std::dynamic_pointer_cast<PraiaClass>(superclass.asCallable());
            auto subPtr = std::dynamic_pointer_cast<PraiaClass>(subclass.asCallable());

            if (!superPtr) { RUNTIME_ERR("Superclass must be a class"); }
            if (!subPtr) { RUNTIME_ERR("Subclass must be a class"); }

            subPtr->superclass = superPtr;
            // Inherit the dunder flag — if any ancestor has overloads, we
            // need to dispatch through the chain. OP_INHERIT runs before
            // OP_METHOD in class declaration codegen, so this captures the
            // ancestor state; OP_METHOD then OR-s in any locally declared
            // dunders.
            if (superPtr->hasOperatorOverloads)
                subPtr->hasOperatorOverloads = true;

            pop(); // pop superclass
            break;
        }

        case OpCode::OP_GET_PROPERTY_OPT:
        case OpCode::OP_GET_PROPERTY: {
            bool _propOpt = (static_cast<OpCode>(instruction) == OpCode::OP_GET_PROPERTY_OPT);
            std::string name = READ_STRING();
            Value obj = pop();

            if (_propOpt && obj.isNil()) { push(Value()); break; }

            if (obj.isTagged()) {
                auto t = obj.asTagged();
                if (name == "tag") { push(Value(t->tag)); break; }
                if (name == "values") {
                    auto arr = gcNew<PraiaArray>();
                    arr->elements = t->values;
                    push(Value(arr)); break;
                }
                RUNTIME_ERR("Tagged value has no field '" + name + "'");
            }

            if (obj.isGenerator()) {
                auto gen = obj.asGenerator();
                if (name == "next") {
                    VM* vm = this;
                    auto fn = std::make_shared<NativeFunction>();
                    fn->funcName = "next";
                    fn->numArgs = -1;
                    fn->fn = [gen, vm](const std::vector<Value>& args) -> Value {
                        Value sendVal = args.empty() ? Value() : args[0];
                        return vm->resumeGenerator(gen, sendVal);
                    };
                    push(Value(std::static_pointer_cast<Callable>(fn)));
                    break;
                }
                if (name == "done") {
                    push(Value(gen->state == PraiaGenerator::State::COMPLETED));
                    break;
                }
                RUNTIME_ERR("Generator has no property '" + name + "'");
            }

            if (obj.isInstance()) {
                auto inst = obj.asInstance();
                // Fields first
                auto fit = inst->fields.find(name);
                if (fit != inst->fields.end()) { push(fit->second); break; }

                // Then methods — walk class chain, track which class owns it
                std::shared_ptr<PraiaClass> methodOwner;
                Value methodVal;
                {
                    auto walk = inst->klass;
                    while (walk) {
                        auto sit = walk->vmMethods.find(name);
                        if (sit != walk->vmMethods.end()) {
                            methodOwner = walk;
                            methodVal = sit->second;
                            break;
                        }
                        walk = walk->superclass;
                    }
                }
                if (methodOwner) {
                    if (methodVal.isCallable()) {
                        auto* vmcc = dynamic_cast<VMClosureCallable*>(methodVal.asCallable().get());
                        if (vmcc) {
                            auto bm = std::make_shared<VMBoundMethod>(obj, vmcc->closure, methodOwner);
                            Value bound = Value(std::static_pointer_cast<Callable>(bm));
                            // Apply decorators if present
                            auto dit = methodOwner->methodDecorators.find(name);
                            if (dit != methodOwner->methodDecorators.end()) {
                                // Push decorator calls onto the stack for the VM to execute:
                                // We build: deco_n(... deco_1(bound) ...)
                                // by pushing a chain of calls.
                                // Use callWithVM from vm_natives.cpp.
                                for (auto& deco : dit->second) {
                                    bound = callWithVM(*this, deco.asCallable(), {bound});
                                }
                            }
                            push(bound);
                            break;
                        }
                    }
                    push(methodVal);
                    break;
                }

                // Fall through to universal methods below
            }

            // Map fields take priority over methods
            if (obj.isMap()) {
                auto& entries = obj.asMap()->entries;
                auto it = entries.find(Value(name));
                if (it != entries.end()) { push(it->second); break; }
                if (name == "has" || name == "get" || name == "delete" || name == "merge" ||
                    name == "entries" || name == "clear") {
                    push(getMapMethod(obj.asMap(), name, CURRENT_LINE()));
                    break;
                }
                // Fall through to universal methods below
            }

            // Universal methods — work on any value type, but instance
            // fields/methods and map keys take priority (checked above).
            if (name == "toString") {
                Value captured = obj;
                auto fn = std::make_shared<NativeFunction>();
                fn->funcName = "toString";
                fn->numArgs = 0;
                fn->fn = [captured](const std::vector<Value>&) -> Value {
                    return Value(captured.toString());
                };
                push(Value(std::static_pointer_cast<Callable>(fn)));
                break;
            }
            if (name == "toNum") {
                Value captured = obj;
                auto fn = std::make_shared<NativeFunction>();
                fn->funcName = "toNum";
                fn->numArgs = 0;
                fn->fn = [captured](const std::vector<Value>&) -> Value {
                    if (captured.isNumber()) return captured;
                    if (captured.isBool()) return Value(captured.asBool() ? 1.0 : 0.0);
                    if (captured.isString()) {
                        auto& s = captured.asString();
                        std::string lower = s;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                        if (lower == "true") return Value(1.0);
                        if (lower == "false") return Value(0.0);
                        try {
                            size_t pos = 0;
                            double d = std::stod(s, &pos);
                            if (pos == s.size()) return Value(d);
                        } catch (...) {}
                        throw RuntimeError("toNum: cannot parse \"" + s + "\" as a number", 0);
                    }
                    throw RuntimeError("toNum: cannot convert " + captured.toString() +
                                       " to a number", 0);
                };
                push(Value(std::static_pointer_cast<Callable>(fn)));
                break;
            }

            // Instance/map with no matching field and no universal method match
            if (obj.isInstance()) {
                if (_propOpt) { push(Value()); break; }
                RUNTIME_ERR("Instance has no property '" + name + "'");
            }
            if (obj.isMap()) {
                // Maps return nil for missing fields; see Interpreter::IndexGet
                // for the design rationale (matches Python/JS/Lua semantics).
                push(Value());
                break;
            }

            // String/array methods
            if (obj.isString()) {
                try {
                    push(getStringMethod(obj.asString(), name, CURRENT_LINE()));
                } catch (const RuntimeError& err) {
                    if (tryHandleError(Value(std::string(err.what())))) break;
                    runtimeError(err.what(), CURRENT_LINE());
                    return Result::RUNTIME_ERROR;
                }
                break;
            }
            if (obj.isArray()) {
                try {
                    push(getArrayMethod(obj.asArray(), name, CURRENT_LINE(), nullptr, this));
                } catch (const RuntimeError& err) {
                    if (tryHandleError(Value(std::string(err.what())))) break;
                    runtimeError(err.what(), CURRENT_LINE());
                    return Result::RUNTIME_ERROR;
                }
                break;
            }
            if (obj.isSet()) {
                try {
                    push(getSetMethod(obj.asSet(), name, CURRENT_LINE()));
                } catch (const RuntimeError& err) {
                    if (tryHandleError(Value(std::string(err.what())))) break;
                    runtimeError(err.what(), CURRENT_LINE());
                    return Result::RUNTIME_ERROR;
                }
                break;
            }

            // Static methods on classes
            if (obj.isCallable()) {
                auto klass = std::dynamic_pointer_cast<PraiaClass>(obj.asCallable());
                if (klass) {
                    auto walk = klass;
                    while (walk) {
                        auto it = walk->vmStaticMethods.find(name);
                        if (it != walk->vmStaticMethods.end()) {
                            push(it->second); // push closure directly (no this binding)
                            break;
                        }
                        walk = walk->superclass;
                    }
                    if (walk) break;
                }
            }

            if (_propOpt) { push(Value()); break; }
            RUNTIME_ERR("Cannot access property '" + name + "' on this type");
        }

        case OpCode::OP_SET_PROPERTY: {
            std::string name = READ_STRING();
            Value val = pop();
            Value obj = pop();

            if (obj.isInstance()) {
                obj.asInstance()->fields[name] = val;
                push(val);
                break;
            }
            if (obj.isMap()) {
                obj.asMap()->entries[Value(name)] = val;
                push(val);
                break;
            }

            RUNTIME_ERR("Can only set properties on instances and maps");
        }

        case OpCode::OP_GET_THIS: {
            push(stack[FRAME.baseSlot]);
            break;
        }

        case OpCode::OP_GET_SUPER: {
            std::string name = READ_STRING();
            Value instance = pop();

            if (!instance.isInstance()) {
                RUNTIME_ERR("'super' used outside of a method");
            }

            // Use the defining class of the current method (not inst->klass)
            // so multi-level inheritance resolves super correctly
            auto defClass = FRAME.definingClass;
            if (!defClass) {
                // Fallback: no defining class on frame, use instance's class
                defClass = instance.asInstance()->klass;
            }
            auto super = defClass->superclass;
            if (!super) { RUNTIME_ERR("Class has no superclass"); }

            // Walk up from super to find the method
            std::shared_ptr<PraiaClass> methodOwner;
            Value methodVal;
            {
                auto walk = super;
                while (walk) {
                    auto sit = walk->vmMethods.find(name);
                    if (sit != walk->vmMethods.end()) {
                        methodOwner = walk;
                        methodVal = sit->second;
                        break;
                    }
                    walk = walk->superclass;
                }
            }
            if (!methodOwner) {
                RUNTIME_ERR("Superclass has no method '" + name + "'");
            }

            auto* vmcc = dynamic_cast<VMClosureCallable*>(methodVal.asCallable().get());
            if (vmcc) {
                auto bm = std::make_shared<VMBoundMethod>(instance, vmcc->closure, methodOwner);
                push(Value(std::static_pointer_cast<Callable>(bm)));
            } else {
                push(methodVal);
            }
            break;
        }

        case OpCode::OP_INVOKE:
        case OpCode::OP_SUPER_INVOKE: {
            // Reserved opcodes for a future method-call optimization
            // (combine OP_GET_PROPERTY + OP_CALL into one). The compiler
            // currently emits the two-opcode form instead, so these are
            // unreachable. If you see this error, the compiler started
            // emitting them without a corresponding VM handler.
            RUNTIME_ERR("internal: OP_INVOKE/OP_SUPER_INVOKE are reserved and not implemented");
        }

        case OpCode::OP_BUILD_ARRAY: {
            uint16_t count = READ_U16();
            auto arr = gcNew<PraiaArray>();
            if (count == 0xFFFF) {
                // Dynamic count (with spreads) — not yet supported, use fixed count
                // For now, this shouldn't happen since we fall back to fixed count
                RUNTIME_ERR("Dynamic array builds not yet supported in VM");
            }
            arr->elements.resize(count);
            for (int i = count - 1; i >= 0; i--) arr->elements[i] = pop();
            push(Value(arr));
            break;
        }

        case OpCode::OP_BUILD_SET: {
            uint16_t count = READ_U16();
            auto set = gcNew<PraiaSet>();
            // Pop N values off the stack into a temporary so we can
            // insert in source order (matches tree-walker iteration
            // order for `#{1, 2, 3}` despite the unordered_set's
            // unspecified iteration order — important only for the
            // first insert that determines internal bucket layout
            // when sizes are very small).
            std::vector<Value> tmp(count);
            for (int i = count - 1; i >= 0; i--) tmp[i] = pop();
            for (auto& v : tmp) {
                if (!isHashable(v)) { RUNTIME_ERR("Unhashable type used as set element"); }
                set->elements.insert(std::move(v));
            }
            push(Value(set));
            break;
        }

        case OpCode::OP_SET_INSERT: {
            // Stack: [..., set, value] → [..., set] with `value`
            // inserted in place. Used by compileSetLiteralExpr's
            // spread path so static elements get inserted directly
            // into the accumulator instead of routing through
            // OP_BUILD_SET-of-pending + OP_ADD-as-union, which
            // allocated a temp set and copied the accumulator on
            // every batch boundary.
            Value v = pop();
            Value setVal = pop();
            if (!setVal.isSet()) { RUNTIME_ERR("OP_SET_INSERT: expected set on stack"); }
            if (!isHashable(v)) { RUNTIME_ERR("Unhashable type used as set element"); }
            setVal.asSet()->elements.insert(std::move(v));
            push(setVal);
            break;
        }

        case OpCode::OP_SET_SPREAD: {
            // Stack: [..., set, iterable] → [..., set] with every
            // element of `iterable` inserted into `set`. Used by
            // compileSetLiteralExpr to lower `#{1, ...other}` without
            // routing through a name-resolvable global (which a user
            // could shadow with `let __toSet = ...` and silently
            // change set-literal semantics under the VM). The set
            // operand is freshly-allocated by the emitting code path
            // (`OP_BUILD_SET 0` + a chain of `OP_SET_INSERT` /
            // `OP_SET_SPREAD` instructions), so in-place mutation is
            // safe — nothing else holds a reference.
            Value iterable = pop();
            Value setVal = pop();
            if (!setVal.isSet()) { RUNTIME_ERR("OP_SET_SPREAD: expected set on stack"); }
            auto& dest = setVal.asSet()->elements;
            if (iterable.isSet()) {
                for (auto& e : iterable.asSet()->elements) dest.insert(e);
            } else if (iterable.isArray()) {
                for (auto& e : iterable.asArray()->elements) {
                    if (!isHashable(e))
                        { RUNTIME_ERR("Spread into set: element must be hashable"); }
                    dest.insert(e);
                }
            } else {
                RUNTIME_ERR("Spread into set literal requires a set or array");
            }
            push(setVal);
            break;
        }

        case OpCode::OP_BUILD_MAP: {
            uint16_t count = READ_U16();
            auto map = gcNew<PraiaMap>();
            // Stack has count pairs: key, value, key, value, ...
            // Pop in reverse
            std::vector<std::pair<Value, Value>> pairs(count);
            for (int i = count - 1; i >= 0; i--) {
                Value val = pop();
                Value key = pop();
                if (!isHashable(key)) { RUNTIME_ERR("Unhashable type used as map key"); }
                pairs[i] = {std::move(key), std::move(val)};
            }
            for (auto& [k, v] : pairs) map->entries[k] = std::move(v);
            push(Value(map));
            break;
        }

        case OpCode::OP_INDEX_GET_OPT:
        case OpCode::OP_INDEX_GET: {
            bool _idxOpt = (static_cast<OpCode>(instruction) == OpCode::OP_INDEX_GET_OPT);
            Value idx = pop();
            Value obj = pop();
            if (_idxOpt && obj.isNil()) { push(Value()); break; }
            // Optional access (`?[`) suppresses "missing value" failures
            // (out-of-bounds, missing map key) but still throws on type
            // errors — those indicate a programming bug, not an absent element.
            if (obj.isArray()) {
                if (!idx.isNumber()) { RUNTIME_ERR("Array index must be a number"); }
                auto& elems = obj.asArray()->elements;
                int i = static_cast<int>(idx.asNumber());
                if (i < 0) i += static_cast<int>(elems.size());
                if (i < 0 || i >= static_cast<int>(elems.size())) {
                    if (_idxOpt) { push(Value()); break; }
                    RUNTIME_ERR("Array index out of bounds");
                }
                push(elems[i]);
            } else if (obj.isString()) {
                if (!idx.isNumber()) { RUNTIME_ERR("String index must be a number"); }
                auto& str = obj.asString();
                int i = static_cast<int>(idx.asNumber());
#ifdef HAVE_UTF8PROC
                auto gs = utf8_graphemes(str);
                int slen = static_cast<int>(gs.size());
                if (i < 0) i += slen;
                if (i < 0 || i >= slen) {
                    if (_idxOpt) { push(Value()); break; }
                    RUNTIME_ERR("String index out of bounds");
                }
                push(Value(gs[i]));
#else
                if (i < 0) i += static_cast<int>(str.size());
                if (i < 0 || i >= static_cast<int>(str.size())) {
                    if (_idxOpt) { push(Value()); break; }
                    RUNTIME_ERR("String index out of bounds");
                }
                push(Value(std::string(1, str[i])));
#endif
            } else if (obj.isMap()) {
                auto& entries = obj.asMap()->entries;
                auto it = entries.find(idx);
                // Missing keys return nil — see Interpreter::IndexGet rationale.
                if (it == entries.end()) push(Value());
                else push(it->second);
            } else if (obj.isInstance()) {
                auto [ok, r] = vmCallDunder(*this, obj, "__index", {idx});
                if (ok) { push(r); }
                else { RUNTIME_ERR("Can only index into arrays, strings, and maps"); }
            } else {
                RUNTIME_ERR("Can only index into arrays, strings, and maps");
            }
            break;
        }


        case OpCode::OP_INDEX_SET: {
            Value val = pop();
            Value idx = pop();
            Value obj = pop();
            if (obj.isInstance()) {
                auto [ok, r] = vmCallDunder(*this, obj, "__indexSet", {idx, val});
                if (ok) { push(val); break; }
            }
            if (obj.isArray()) {
                if (!idx.isNumber()) { RUNTIME_ERR("Array index must be a number"); }
                auto& elems = obj.asArray()->elements;
                int i = static_cast<int>(idx.asNumber());
                if (i < 0) i += static_cast<int>(elems.size());
                if (i < 0 || i >= static_cast<int>(elems.size())) { RUNTIME_ERR("Array index out of bounds"); }
                elems[i] = val;
                push(val);
            } else if (obj.isMap()) {
                if (!isHashable(idx)) { RUNTIME_ERR("Unhashable type used as map key"); }
                obj.asMap()->entries[idx] = val;
                push(val);
            } else {
                RUNTIME_ERR("Can only assign to array or map indices");
            }
            break;
        }

        case OpCode::OP_UNPACK_SPREAD: {
            // Reserved opcode. Spread (`...arr`) is currently handled at
            // build time by the compiler emitting the appropriate fixed-count
            // OP_BUILD_ARRAY/OP_BUILD_MAP sequences, so this is unreachable.
            // If you see this error, the compiler started emitting it without
            // a corresponding VM handler.
            RUNTIME_ERR("internal: OP_UNPACK_SPREAD is reserved and not implemented");
        }

        case OpCode::OP_BUILD_STRING: {
            uint16_t count = READ_U16();
            std::string result;
            // Collect all parts in order
            std::vector<Value> parts(count);
            for (int i = count - 1; i >= 0; i--) parts[i] = pop();
            for (auto& p : parts) result += p.toString();
            push(Value(std::move(result)));
            break;
        }
        case OpCode::OP_TRY_BEGIN: {
            uint16_t catchOffset = READ_U16();
            ExceptionHandler handler;
            handler.catchIp = FRAME.ip + catchOffset;
            handler.frameIndex = frameCount - 1;
            handler.stackTop = stackTop;
            exceptionHandlers.push_back(handler);
            break;
        }

        case OpCode::OP_TRY_END: {
            if (!exceptionHandlers.empty()) {
                exceptionHandlers.pop_back();
            }
            break;
        }

        case OpCode::OP_THROW: {
            Value error = pop();
            if (tryHandleError(error)) break;

            // Uncaught in this execute scope
            lastError_ = error.toString();
            if (!suppressErrors_ && executeDepth_ <= 1) {
                std::cerr << formatLocation(CURRENT_LINE(), CURRENT_COLUMN())
                          << " Uncaught error: " << error.toString() << std::endl;
                std::cerr << formatStackTrace();
            }
            return Result::RUNTIME_ERROR;
        }
        case OpCode::OP_DEFER: {
            Value closure = pop();
            deferStack[frameCount - 1].push_back(std::move(closure));
            break;
        }
        case OpCode::OP_RUN_DEFERS:
            // Not emitted by compiler — defers run in OP_RETURN and exception unwinding
            break;
        case OpCode::OP_BUILD_TAGGED: {
            std::string tagName = READ_STRING();
            uint8_t argc = READ_BYTE();
            auto tagged = gcNew<PraiaTagged>();
            tagged->tag = tagName;
            tagged->values.resize(argc);
            for (int i = argc - 1; i >= 0; i--) tagged->values[i] = pop();
            push(Value(tagged));
            break;
        }
        case OpCode::OP_TAG_OR_CALL: {
            // Stack has [arg0, arg1, ...] — NO callee on stack.
            // Look up tagName as a global. If callable (class), call it.
            // Otherwise, build a tagged value.
            //
            // Honors Chunk::homeVm so a grain method like `this._d = Deque()`
            // resolves Deque against the grain's own globals, not whatever
            // VM happens to be running this frame. Without this, Deque
            // wouldn't be found in the parent VM and would silently fall
            // through to constructing a tagged value named "Deque" — see
            // the cross-grain isolation regression in tests.
            std::string tagName = READ_STRING();
            uint8_t argc = READ_BYTE();
            VM* gvm = FRAME.chunk().homeVm ? FRAME.chunk().homeVm : this;
            int gslot = gvm->findGlobalSlot(tagName);
            if (gslot >= 0 && gvm->isTaskVm_ && !gvm->loadedMask_[gslot]) gvm->lazyLoadGlobal(gslot);
            if (gslot >= 0 && gvm->globals[gslot].isCallable()) {
                // Global class/function — insert callee below args and call
                // Shift args up by 1 to make room for callee
                push(Value()); // make space
                for (int i = 0; i < argc; i++)
                    stack[stackTop - 1 - i] = stack[stackTop - 2 - i];
                stack[stackTop - argc - 1] = gvm->globals[gslot];
                if (!callValue(gvm->globals[gslot], argc, CURRENT_LINE()))
                    return Result::RUNTIME_ERROR;
            } else {
                auto tagged = gcNew<PraiaTagged>();
                tagged->tag = tagName;
                tagged->values.resize(argc);
                for (int i = argc - 1; i >= 0; i--) tagged->values[i] = pop();
                push(Value(tagged));
            }
            break;
        }
        case OpCode::OP_IMPORT: {
            std::string path = READ_STRING();
            std::string alias = READ_STRING();
            (void)alias; // alias is handled by the compiler (defines the variable)
            Value exports = loadGrain(path, CURRENT_LINE());
            if (exports.isNil() && grainCache.find(path) == grainCache.end()) {
                return Result::RUNTIME_ERROR;
            }
            push(exports);
            break;
        }

        case OpCode::OP_EXPORT: {
            (void)READ_BYTE(); // unused count byte
            // The exports map is on top of the stack.
            // For a grain, this is the return value. Just return from execution.
            {
                Value result = pop();
                int returnBase = FRAME.baseSlot;
                closeUpvalues(&stack[returnBase]);
                frameCount--;
                if (frameCount == 0) {
                    push(std::move(result));
                    return Result::OK;
                }
                stackTop = returnBase;
                push(std::move(result));
            }
            break;
        }

        case OpCode::OP_ASYNC_NAMED:
        case OpCode::OP_ASYNC: {
            uint8_t argc = READ_BYTE();

            // For OP_ASYNC_NAMED, read the names constant and reorder args
            // before entering the shared async dispatch.
            std::vector<std::string> argNamesList;
            bool hasNamedArgs = (static_cast<OpCode>(instruction) == OpCode::OP_ASYNC_NAMED);
            if (hasNamedArgs) {
                uint16_t namesIdx = READ_U16();
                auto& namesArr = FRAME.chunk().constants[namesIdx].asArray()->elements;
                for (auto& n : namesArr) argNamesList.push_back(n.asString());
            }

            Value callee = peek(argc);

            if (!callee.isCallable()) {
                RUNTIME_ERR("async requires a callable");
            }

            auto callable = callee.asCallable();

            // Collect args
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; i--) args[i] = pop();
            pop(); // callee

            // Reorder named args to match parameter positions
            if (hasNamedArgs) {
                const auto* params = callable->paramNames();
                if (!params) {
                    RUNTIME_ERR("Named arguments not supported for '" + callable->name() + "'");
                }
                int paramCount = static_cast<int>(params->size());
                std::vector<Value> reordered(paramCount);
                std::vector<bool> filled(paramCount, false);
                int positionalIdx = 0;
                for (int i = 0; i < argc; i++) {
                    if (argNamesList[i].empty()) {
                        if (positionalIdx >= paramCount) {
                            RUNTIME_ERR(callable->name() + "() too many arguments");
                        }
                        reordered[positionalIdx] = args[i];
                        filled[positionalIdx] = true;
                        positionalIdx++;
                    } else {
                        int found = -1;
                        for (int p = 0; p < paramCount; p++) {
                            if ((*params)[p] == argNamesList[i]) { found = p; break; }
                        }
                        if (found == -1) {
                            RUNTIME_ERR(callable->name() + "() unknown parameter '" + argNamesList[i] + "'");
                        }
                        if (filled[found]) {
                            RUNTIME_ERR(callable->name() + "() parameter '" + argNamesList[i] + "' specified twice");
                        }
                        reordered[found] = args[i];
                        filled[found] = true;
                    }
                }
                args = std::move(reordered);
            }

            /* Deep-copy heap-allocated values so the async task doesn't
            share the caller's mutable state. The actual logic is in
            deepCopyForTask (free helper above). `visited` is shared across
            args / upvalues / receiver below so a single heap object passed
            via multiple paths is cloned once. */
            std::unordered_map<void*, Value> visited;

            // Deep-copy args so async task can't mutate caller's objects
            for (auto& arg : args)
                arg = deepCopyForTask(arg, visited);

            auto* native = dynamic_cast<NativeFunction*>(callable.get());
            auto* vmcc = dynamic_cast<VMClosureCallable*>(callable.get());
            auto* bound = dynamic_cast<VMBoundMethod*>(callable.get());
            auto klass = std::dynamic_pointer_cast<PraiaClass>(callable);

            // For class construction, locate init in the vmMethods chain
            std::shared_ptr<PraiaClass> initOwner;
            VMClosureCallable* initVmcc = nullptr;
            if (klass) {
                auto walk = klass;
                while (walk) {
                    if (walk->vmMethods.count("init")) { initOwner = walk; break; }
                    walk = walk->superclass;
                }
                if (initOwner) {
                    auto& initVal = initOwner->vmMethods["init"];
                    if (initVal.isCallable())
                        initVmcc = dynamic_cast<VMClosureCallable*>(initVal.asCallable().get());
                }
            }

            std::shared_future<Value> sharedFuture;
            if (vmcc || bound || initVmcc) {
                /* VMClosureCallable, VMBoundMethod, and PraiaClass (with VM
                init) all use the same task-VM path. The differences:
                - bound methods push receiver as slot 0 and set definingClass
                - class construction creates a fresh instance as slot 0,
                  runs init, and returns the instance (not init's return) */
                auto fn = bound ? bound->method->function
                        : initVmcc ? initVmcc->closure->function
                        : vmcc->closure->function;
                auto* closureSrc = bound ? bound->method
                                  : initVmcc ? initVmcc->closure
                                  : vmcc->closure;
                int arity = fn->arity;
                bool isConstructor = (initVmcc != nullptr);

                // Arity check — must match callClosure's behavior
                if (static_cast<int>(args.size()) > arity) {
                    std::string errName = klass ? klass->className : fn->name;
                    RUNTIME_ERR(errName + "() expected at most " + std::to_string(arity) +
                        " " + argStr(arity) + " but got " + std::to_string(args.size()));
                }

                // Lazy/COW globals: build a shallow snapshot of the parent's
                // globals (just shared_ptr increments — no deep-copy). The
                // task VM will deep-copy a slot only the first time it
                // reads or writes it (see VM::lazyLoadGlobal). A task that
                // touches only `sys` pays 1 deepCopy instead of N.
                //
                // Nested async: if THIS VM is itself a task, slots we
                // haven't loaded yet still hold Value() in `globals` —
                // fall through to OUR snapshot so the child task sees the
                // value the grand-parent had, not nil.
                //
                // Indices map is copied verbatim (not rebuilt) so slot
                // numbering matches between parent and task — the chunk's
                // shared globalSlotCache is correct in both VMs.
                std::vector<Value> globalsCopy;
                globalsCopy.reserve(globals.size());
                for (size_t s = 0; s < globals.size(); s++) {
                    if (isTaskVm_ && !loadedMask_[s])
                        globalsCopy.push_back(globalsSnapshot_[s]);
                    else
                        globalsCopy.push_back(globals[s]);
                }
                auto indicesCopy = globalIndices;

                // Snapshot upvalues and deep-copy so captured arrays/maps/instances
                // are isolated from the caller
                std::vector<Value> upvalueSnapshot;
                for (int i = 0; i < closureSrc->upvalueCount; i++) {
                    auto* uv = closureSrc->upvalues[i];
                    upvalueSnapshot.push_back(uv ? deepCopyForTask(*uv->location, visited) : Value());
                }

                // For bound methods, deep-copy the receiver so the task
                // doesn't share instance fields with the caller.
                // For constructors, the instance is created fresh in the task.
                Value receiverCopy = bound ? deepCopyForTask(bound->receiver, visited) : Value();
                auto defClass = bound ? bound->definingClass
                              : initVmcc ? initOwner : nullptr;

                // Pin the deep-copied receiver / upvalues / args (those
                // are gcNew'd on the parent thread, so they're tracked in
                // *this* heap but only reachable via the lambda capture on
                // another thread). The globals snapshot does NOT need
                // pinning — its entries point to objects the parent's
                // gcMarkRoots already covers through this VM's live
                // `globals` vector. See audit fix #10 for the original
                // bug this guards against.
                auto rootsHolder = std::make_shared<std::vector<Value>>();
                if (!receiverCopy.isNil()) rootsHolder->push_back(receiverCopy);
                for (auto& uv : upvalueSnapshot) rootsHolder->push_back(uv);
                for (auto& a : args) rootsHolder->push_back(a);
                {
                    std::lock_guard<std::mutex> lk(inflightRootsMtx_);
                    inflightTaskRoots_.push_back(rootsHolder);
                }

                sharedFuture = std::async(std::launch::async,
                    [fn, args, globalsCopy = std::move(globalsCopy),
                     indicesCopy = std::move(indicesCopy), arity,
                     upvalueSnapshot = std::move(upvalueSnapshot),
                     builtinNames = builtinNames_,
                     receiverCopy = std::move(receiverCopy),
                     defClass = std::move(defClass),
                     rootsHolder, // pinned roots — keep alive while lambda lives
                     isConstructor, klass]() mutable -> Value {
                        VM taskVm;
                        GcHeap::current().disable(); // task VMs are short-lived
                        // Lazy/COW globals setup. globalsCopy here is the
                        // shallow snapshot (parent's shared_ptrs). The
                        // task's own `globals` vector starts as all-nil
                        // and is filled in by lazyLoadGlobal as each slot
                        // is first touched.
                        taskVm.isTaskVm_ = true;
                        taskVm.globalIndices = std::move(indicesCopy);
                        taskVm.globalsSnapshot_ = std::move(globalsCopy);
                        taskVm.globals.assign(taskVm.globalsSnapshot_.size(), Value());
                        taskVm.loadedMask_.assign(taskVm.globalsSnapshot_.size(), 0);
                        taskVm.builtinNames_ = std::move(builtinNames);
                        taskVm.suppressErrors_ = true; // errors propagate to await, not stderr

                        // Rewire VMClosureCallable vm pointers in the
                        // eagerly deep-copied args / upvalues / receiver
                        // (globals are rewired lazily inside lazyLoadGlobal
                        // on first touch). Shared visited so cross-batch
                        // identity matches what deepCopyForTask produced.
                        std::unordered_set<void*> rewireVisited;
                        for (auto& a : args) rewireForTaskVm(a, &taskVm, rewireVisited);
                        for (auto& u : upvalueSnapshot) rewireForTaskVm(u, &taskVm, rewireVisited);
                        if (!receiverCopy.isNil())
                            rewireForTaskVm(receiverCopy, &taskVm, rewireVisited);

                        // Pad args to match arity
                        std::vector<Value> paddedArgs = args;
                        while (static_cast<int>(paddedArgs.size()) < arity)
                            paddedArgs.push_back(Value());

                        taskVm.allClosures.push_back(std::make_unique<ObjClosure>(fn));
                        auto* closure = taskVm.allClosures.back().get();

                        // Restore upvalues as closed (self-contained) values
                        for (int i = 0; i < closure->upvalueCount && i < static_cast<int>(upvalueSnapshot.size()); i++) {
                            taskVm.allUpvalues.push_back(std::make_unique<ObjUpvalue>(nullptr));
                            auto* uv = taskVm.allUpvalues.back().get();
                            uv->closed = upvalueSnapshot[i];
                            uv->location = &uv->closed;
                            closure->upvalues[i] = uv;
                        }

                        auto wrapper = std::make_shared<VMClosureCallable>(closure);
                        wrapper->vm = &taskVm;

                        // For constructors, create a fresh instance as the receiver
                        Value instanceVal;
                        if (isConstructor) {
                            auto instance = gcNew<PraiaInstance>();
                            instance->klass = klass;
                            instanceVal = Value(instance);
                        }

                        // Slot 0: receiver (this) for bound methods/constructors,
                        // closure for plain functions
                        if (isConstructor) {
                            taskVm.push(instanceVal);
                        } else if (!receiverCopy.isNil()) {
                            taskVm.push(receiverCopy);
                        } else {
                            taskVm.push(Value(std::static_pointer_cast<Callable>(wrapper)));
                        }
                        for (auto& arg : paddedArgs) taskVm.push(arg);

                        taskVm.frames[0].closure = closure;
                        taskVm.frames[0].function = fn;
                        taskVm.frames[0].ip = fn->chunk.code.data();
                        taskVm.frames[0].baseSlot = 0;
                        taskVm.frames[0].definingClass = defClass;
                        taskVm.frameCount = 1;

                        auto result = taskVm.execute();
                        if (result != Result::OK)
                            throw RuntimeError(
                                taskVm.lastError_.empty() ? "Async task failed" : taskVm.lastError_, 0);

                        // Constructor: return the instance (init mutated its
                        // fields via `this`). Otherwise return the function's
                        // return value from the stack.
                        Value retVal = isConstructor ? instanceVal : taskVm.pop();

                        // Transfer closure/upvalue ownership from taskVm into a
                        // shared bag attached to any VMClosureCallable in the result.
                        // This keeps the raw pointers alive after taskVm is destroyed.
                        auto ownership = std::make_shared<TaskOwnership>();
                        ownership->closures = std::move(taskVm.allClosures);
                        ownership->upvalues = std::move(taskVm.allUpvalues);
                        taskVm.allClosures.clear();
                        taskVm.allUpvalues.clear();

                        // Helper to walk a PraiaClass chain's vmMethods.
                        // Track visited objects to handle cycles.
                        std::unordered_set<void*> attachVisited;
                        std::function<void(Value&)> attachOwnership;
                        auto walkClassChain = [&attachOwnership, &attachVisited](std::shared_ptr<PraiaClass> klass) {
                            while (klass) {
                                void* p = static_cast<void*>(klass.get());
                                if (!attachVisited.insert(p).second) break;
                                for (auto& [k, mv] : klass->vmMethods)
                                    attachOwnership(mv);
                                klass = klass->superclass;
                            }
                        };

                        attachOwnership = [&ownership, &attachOwnership, &walkClassChain, &attachVisited](Value& v) {
                            if (v.isCallable()) {
                                auto* vc = dynamic_cast<VMClosureCallable*>(v.asCallable().get());
                                if (vc) {
                                    vc->taskOwnership = ownership;
                                    vc->vm = nullptr; // force VM::current() in call()
                                }
                                auto* bm = dynamic_cast<VMBoundMethod*>(v.asCallable().get());
                                if (bm) {
                                    bm->taskOwnership = ownership;
                                    attachOwnership(bm->receiver);
                                }
                                auto klass = std::dynamic_pointer_cast<PraiaClass>(v.asCallable());
                                if (klass) walkClassChain(klass);
                            }
                            if (v.isMap()) {
                                void* p = static_cast<void*>(v.asMap().get());
                                if (attachVisited.insert(p).second)
                                    for (auto& [k, mv] : v.asMap()->entries) attachOwnership(mv);
                            }
                            if (v.isArray()) {
                                void* p = static_cast<void*>(v.asArray().get());
                                if (attachVisited.insert(p).second)
                                    for (auto& el : v.asArray()->elements) attachOwnership(el);
                            }
                            if (v.isInstance()) {
                                void* p = static_cast<void*>(v.asInstance().get());
                                if (attachVisited.insert(p).second) {
                                    for (auto& [k, fv] : v.asInstance()->fields) attachOwnership(fv);
                                    walkClassChain(v.asInstance()->klass);
                                }
                            }
                        };
                        attachOwnership(retVal);

                        return retVal;
                    }).share();
            } else if (klass && !initVmcc) {
                // Class with no init method — just create the instance
                if (!args.empty()) {
                    RUNTIME_ERR(klass->className + "() takes no arguments (no init method)");
                }
                auto instance = gcNew<PraiaInstance>();
                instance->klass = klass;
                std::promise<Value> prom;
                prom.set_value(Value(instance));
                sharedFuture = prom.get_future().share();
            } else if (native) {
                int arity = native->arity();
                if (arity != -1 && static_cast<int>(args.size()) != arity) {
                    RUNTIME_ERR(native->name() + "() expected " + std::to_string(arity) +
                        " " + argStr(arity) + " but got " + std::to_string(args.size()));
                }
                // Pin deep-copied args for the native task — see comment in the
                // VMClosureCallable path above.
                auto rootsHolder = std::make_shared<std::vector<Value>>(args);
                {
                    std::lock_guard<std::mutex> lk(inflightRootsMtx_);
                    inflightTaskRoots_.push_back(rootsHolder);
                }
                sharedFuture = std::async(std::launch::async,
                    [native, args, rootsHolder]() -> Value {
                        (void)rootsHolder; // capture-only: keeps roots alive
                        return native->fn(args);
                    }).share();
            } else {
                // Bound methods, etc. — run synchronously
                Interpreter dummy;
                Value result = callable->call(dummy, args);
                std::promise<Value> p;
                p.set_value(std::move(result));
                sharedFuture = p.get_future().share();
            }

            auto fut = std::make_shared<PraiaFuture>();
            fut->future = sharedFuture;
            // Keep an extra reference to the shared_future in a global tracker.
            // Without this, the shared_future inside `fut` is the LAST reference
            // when the user discards the future (e.g. `async fn()` as a bare
            // statement). The C++ standard makes the destructor of the last
            // reference to a std::async-launched shared state block until the
            // task completes — turning fire-and-forget calls into synchronous
            // ones. retainInflightFuture() reaps completed entries on each call,
            // so the tracker only ever holds in-flight tasks.
            retainInflightFuture(sharedFuture);
            push(Value(fut));
            break;
        }

        case OpCode::OP_YIELD: {
            Value yieldedValue = pop();
            if (!currentGenerator_) { RUNTIME_ERR("yield outside of generator"); }
            auto gen = currentGenerator_;

            // Save VM state into the generator
            int baseSlot = frames[genBaseFrame_].baseSlot;
            gen->savedStack.clear();
            for (int i = baseSlot; i < stackTop; i++)
                gen->savedStack.push_back(stack[i]);
            gen->savedIp = FRAME.ip;
            gen->savedFrameCount = frameCount - genBaseFrame_;
            gen->savedBaseSlot = baseSlot;
            gen->state = PraiaGenerator::State::SUSPENDED;

            // Restore VM to state before .next() was called
            closeUpvalues(&stack[baseSlot]);
            frameCount = genBaseFrame_;
            stackTop = genBaseStackTop_;

            // Push {value, done: false} result
            auto result = gcNew<PraiaMap>();
            result->entries[Value("value")] = yieldedValue;
            result->entries[Value("done")] = Value(false);
            push(Value(result));
            return Result::OK;
        }

        case OpCode::OP_AWAIT: {
            Value val = pop();
            if (!val.isFuture()) {
                RUNTIME_ERR("Can only await a future");
            }
            try {
                Value result = val.asFuture()->future.get();
                push(std::move(result));
            } catch (const RuntimeError& err) {
                RUNTIME_ERR(err.what());
            } catch (...) {
                RUNTIME_ERR("Async task failed");
            }
            break;
        }
        }
    }
    } catch (const ExitSignal&) {
        throw; // propagate sys.exit() to main
    } catch (const RuntimeError& err) {
        // Fatal error (e.g. stack overflow from push())
        lastError_ = err.what();
        return Result::RUNTIME_ERROR;
    }

    #undef FRAME
    #undef READ_BYTE
    #undef READ_U16
    #undef READ_CONSTANT
    #undef READ_STRING
    #undef CURRENT_LINE
}

// ── GC root marking ──

void VM::gcMarkRoots(GcHeap& heap) {
    // Stack
    for (int i = 0; i < stackTop; i++)
        heap.markValue(stack[i]);

    // Globals
    for (auto& v : globals)
        heap.markValue(v);

    // Lazy task-VM snapshot. Empty for non-task VMs; defensive even for
    // task VMs (GC is disabled inside them today, but mark would be
    // correct if that ever changed).
    for (auto& v : globalsSnapshot_)
        heap.markValue(v);

    // Call frame classes (for super resolution)
    for (int i = 0; i < frameCount; i++) {
        if (frames[i].definingClass)
            heap.markValue(Value(std::static_pointer_cast<Callable>(frames[i].definingClass)));
    }

    // All upvalue closed values
    for (auto& uv : allUpvalues)
        heap.markValue(uv->closed);

    // Function constant pools (may contain callable/container literals)
    for (auto& c : allClosures) {
        for (auto& constant : c->function->chunk.constants)
            heap.markValue(constant);
    }

    // Currently resuming generator
    if (currentGenerator_)
        heap.markValue(Value(currentGenerator_));

    // Grain cache
    for (auto& [k, v] : grainCache)
        heap.markValue(v);

    // In-flight async task roots — deep-copied containers used by running
    // tasks live in this VM's heap (because deepCopy ran on this thread).
    // Mark them through the weak_ptr registry so they survive until the task
    // completes and the future is dropped.
    {
        std::lock_guard<std::mutex> lk(inflightRootsMtx_);
        inflightTaskRoots_.erase(
            std::remove_if(inflightTaskRoots_.begin(), inflightTaskRoots_.end(),
                [](const std::weak_ptr<std::vector<Value>>& wp) { return wp.expired(); }),
            inflightTaskRoots_.end());
        for (auto& wp : inflightTaskRoots_) {
            if (auto sp = wp.lock())
                for (auto& v : *sp) heap.markValue(v);
        }
    }

    // Persistent grain VMs — their globals, upvalues, and constant pools
    // hold values created by grain code that exported closures (now
    // reachable through *this* VM's globals) still depend on. Recurse so
    // a sub-grain loaded transitively by an outer grain is also marked.
    for (auto& gvm : grainVMs)
        gvm->gcMarkRoots(heap);
}
