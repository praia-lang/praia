#pragma once

#include "../value.h"
#include "chunk.h"
#include "compiler.h"
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class GcHeap;

// Upvalue: heap-allocated box for captured variables
struct ObjUpvalue {
    Value* location;      // points into the stack while variable is live
    Value closed;          // holds the value after the variable goes out of scope
    ObjUpvalue* next;      // linked list for open upvalues

    ObjUpvalue(Value* slot) : location(slot), next(nullptr) {}
};

// Closure: a compiled function + its captured upvalues
struct ObjClosure {
    std::shared_ptr<CompiledFunction> function;
    std::vector<ObjUpvalue*> upvalues;
    int upvalueCount;

    ObjClosure(std::shared_ptr<CompiledFunction> fn) {
        function = std::move(fn);
        upvalueCount = function->upvalueCount;
        upvalues.resize(upvalueCount, nullptr);
    }
};

// Shared ownership bag for closures/upvalues returned from async tasks.
// Attached to VMClosureCallable wrappers in the result so the raw pointers
// survive after the task VM is destroyed.
struct TaskOwnership {
    std::vector<std::unique_ptr<ObjClosure>> closures;
    std::vector<std::unique_ptr<ObjUpvalue>> upvalues;
};

// Wrapper to store ObjClosure as a Callable in Value
struct VMClosureCallable : Callable {
    ObjClosure* closure;
    std::shared_ptr<ObjClosure> ownedPrototype; // keeps compiler prototypes alive (constant pool entries)
    std::shared_ptr<TaskOwnership> taskOwnership; // keeps async task closures/upvalues alive
    class VM* vm = nullptr; // set when closure is created during VM execution
    VMClosureCallable(ObjClosure* c) : closure(c) {}
    Value call(Interpreter&, const std::vector<Value>& args) override;
    int arity() const override { return closure->function->arity; }
    std::string name() const override { return closure->function->name; }
    const std::vector<std::string>* paramNames() const override { return &closure->function->paramNames; }
};

// A method bound to an instance — when called, slot 0 = this
struct VMBoundMethod : Callable {
    Value receiver;
    ObjClosure* method;
    std::shared_ptr<PraiaClass> definingClass; // which class owns this method (for super resolution)
    std::shared_ptr<TaskOwnership> taskOwnership; // keeps async task closures/upvalues alive
    VMBoundMethod(Value recv, ObjClosure* m, std::shared_ptr<PraiaClass> dc = nullptr)
        : receiver(std::move(recv)), method(m), definingClass(std::move(dc)) {}
    Value call(Interpreter&, const std::vector<Value>&) override { return Value(); } // unused
    int arity() const override { return method->function->arity; }
    std::string name() const override { return method->function->name; }
    const std::vector<std::string>* paramNames() const override { return &method->function->paramNames; }
};

struct VMCallFrame {
    ObjClosure* closure;   // the closure being executed (null for script w/o closure)
    std::shared_ptr<CompiledFunction> function; // fallback for script frame
    const uint8_t* ip;
    int baseSlot;
    // Bit i is set iff parameter i was NOT provided by the caller.
    // Used by OP_IS_MISSING_ARG to decide whether to apply defaults.
    // (Limit of 64 params is enforced at function-definition time.)
    uint64_t missingArgsMask;
    std::shared_ptr<PraiaClass> definingClass; // for super resolution in methods

    Chunk& chunk() const {
        return closure ? closure->function->chunk : function->chunk;
    }
    const std::string& name() const {
        return closure ? closure->function->name : function->name;
    }
};

class VM {
    friend struct VMScope;
public:
    VM();
    ~VM();

    enum class Result { OK, COMPILE_ERROR, RUNTIME_ERROR };

    Result run(std::shared_ptr<CompiledFunction> script);
    Result runRepl(std::shared_ptr<CompiledFunction> script);

    // Setup
    void defineNative(const std::string& name, Value value);
    void setCurrentFile(const std::string& path) { currentFile = path; }
    const std::string& getCurrentFile() const { return currentFile; }
    void setArgs(const std::vector<std::string>& args);

    // Thread-local current VM — used by native functions to find the active VM
    static VM* current() { return currentVM_; }

public:
    Result execute(int baseFrameCount = 0);

    static thread_local VM* currentVM_; // public for VMScope RAII guard
    int executeFloor_ = 0; // handler stack floor for current execute() — handlers below this belong to outer calls
    int executeDepth_ = 0; // nesting depth — 0 before first execute(), 1 during initial, 2+ during re-entrant
private:

    // Stack (heap-allocated so VM can be used in threads with small stacks)
    static constexpr int STACK_MAX = 16384;
    std::unique_ptr<Value[]> stack;
    int stackTop = 0;

public:
    void push(Value value);
    Value pop();
    Value& peek(int distance = 0);
    int getStackTop() const { return stackTop; }
    Value resumeGenerator(std::shared_ptr<PraiaGenerator> gen, Value sendVal);
private:
    void resetStack();

public:
    // Call frames
    static constexpr int FRAMES_MAX = 256;
    VMCallFrame frames[FRAMES_MAX];
    int frameCount = 0;
private:

    // Globals — slot-indexed for fast access. The compiler emits names as
    // constant-pool strings; the first OP_GET_GLOBAL / OP_SET_GLOBAL hit
    // resolves the name to a slot via globalIndices and writes it to the
    // chunk's parallel inline cache (Chunk::globalSlotCache), so subsequent
    // accesses bypass the string hash. Slots are append-only — once
    // assigned, never freed or moved — so cached slot indices remain valid
    // for the lifetime of the VM.
    std::vector<Value> globals;
    std::unordered_map<std::string, int> globalIndices;
    std::set<std::string> builtinNames_; // names registered via defineNative (for grain isolation)

    // Lazy/COW globals for async task VMs. On OP_ASYNC the parent hands the
    // task a shallow-copied `globalsSnapshot_` vector (shared_ptr increments,
    // no deep-copy) plus a verbatim copy of `globalIndices` so slot numbering
    // matches. `loadedMask_[slot]==1` means the task has materialized its
    // own deep-copy into `globals[slot]` and the snapshot entry is no longer
    // authoritative. `isTaskVm_` gates the lazy-check on the hot path — for
    // the main VM both fields stay empty and the branch predicts away.
    std::vector<Value> globalsSnapshot_;
    std::vector<uint8_t> loadedMask_;
    bool isTaskVm_ = false;

    // Helpers. Inline because they're on the global-access hot path.
    int ensureGlobalSlot(const std::string& name) {
        auto it = globalIndices.find(name);
        if (it != globalIndices.end()) return it->second;
        int slot = static_cast<int>(globals.size());
        globalIndices.emplace(name, slot);
        globals.emplace_back(); // default Value (nil)
        if (isTaskVm_) loadedMask_.push_back(1); // new slots are task-local
        return slot;
    }
    int findGlobalSlot(const std::string& name) const {
        auto it = globalIndices.find(name);
        return (it != globalIndices.end()) ? it->second : -1;
    }
public:
    // Slow path: deep-copy globalsSnapshot_[slot] into globals[slot] and
    // mark it loaded. Called by opcode handlers when isTaskVm_ &&
    // !loadedMask_[slot].
    void lazyLoadGlobal(int slot);
private:

    // Open upvalues (linked list, ordered by stack slot desc)
    ObjUpvalue* openUpvalues = nullptr;
    ObjUpvalue* captureUpvalue(Value* local);
    void closeUpvalues(Value* last);

    // Exception handlers
    struct ExceptionHandler {
        const uint8_t* catchIp;  // where to jump on exception
        int frameIndex;          // which call frame installed this
        int stackTop;            // stack depth to restore
    };
    std::vector<ExceptionHandler> exceptionHandlers;

    // Defer stacks — one per call frame
    std::vector<Value> deferStack[FRAMES_MAX];
    void runDefers(int frameIdx);

    // Closures created during execution (RAII ownership)
    std::vector<std::unique_ptr<ObjClosure>> allClosures;
    std::vector<std::unique_ptr<ObjUpvalue>> allUpvalues;

    // In-flight async task roots. The deepCopy in OP_ASYNC runs on the parent
    // thread, so its gcNew'd containers are tracked in *this* VM's heap. The
    // running task holds them only via the std::async lambda capture (on a
    // different thread), so they are unreachable from this VM's GC roots and
    // would be swept mid-task. Each async dispatch builds a
    // shared_ptr<vector<Value>> of every deep-copied root, registers a
    // weak_ptr here, and captures the strong shared_ptr in the lambda. The
    // lambda's lifetime spans task execution AND any held futures (so this
    // covers both in-flight and pending-await phases). gcMarkRoots locks
    // these and marks live entries; expired ones are pruned.
    std::mutex inflightRootsMtx_;
    std::vector<std::weak_ptr<std::vector<Value>>> inflightTaskRoots_;

    // Module system
    std::string currentFile;
    // Authoritative grain cache lives on the root VM only. Child grain
    // VMs leave this empty and reach the root's cache through parentVm_
    // (see rootVm() / loadGrain) so that two sibling grains importing
    // the same dependency share a single execution.
    std::unordered_map<std::string, Value> grainCache;
    std::set<std::string> importedInCurrentFile;
    std::vector<std::vector<StmtPtr>> grainAsts;

    // Persistent per-grain VMs. Each loaded grain runs in its own VM so
    // module-level `let` bindings live in *that* VM's globals — keeping
    // two grains' identically-named module variables isolated. Each
    // grain's compiled chunks point back to the owning VM via
    // Chunk::homeVm, so OP_*_GLOBAL inside an exported closure resolves
    // against the grain's globals regardless of which VM is executing.
    // Owned by the parent (top-level) VM; child grains in turn own their
    // own transitive imports — so the lifetime chain is main → grain →
    // sub-grain.
    std::vector<std::unique_ptr<VM>> grainVMs;

    // Set by VM::loadGrain when this VM is created as a child grain VM.
    // Null on the process's root VM. Used to walk back up to the root
    // for cross-grain state like the shared grain cache.
    VM* parentVm_ = nullptr;
    VM& rootVm() { return parentVm_ ? parentVm_->rootVm() : *this; }

    Value loadGrain(const std::string& path, int line);
    std::string resolveGrainPath(const std::string& path, int line);

    // Helpers
    uint8_t readByte();
    uint16_t readU16();
    Value readConstant();
    std::string readString();
public:
    bool callValue(Value callee, int argCount, int line);
    bool callClosure(ObjClosure* closure, int argCount, int line);
private:

    void runtimeError(const std::string& msg, int line, int column = 0);
    bool tryHandleError(Value error);  // returns true if caught, false if uncaught
    std::string formatStackTrace() const;

public:
    // Generator state — set while executing a generator's body via .next()
    std::shared_ptr<PraiaGenerator> currentGenerator_;
    int genBaseFrame_ = 0;
    int genBaseStackTop_ = 0;
private:

    // Error capture — suppresses stderr output in re-entrant/async contexts
    // so errors propagate instead of being printed prematurely
    std::string lastError_;
    bool suppressErrors_ = false;
public:
    const std::string& lastError() const { return lastError_; }
    void gcMarkRoots(GcHeap& heap);
private:
    int gcCounter_ = 0;
};

// Helper: call any Callable within the VM context (handles VM closures, bound methods, natives)
Value callWithVM(VM& vm, std::shared_ptr<Callable> callable, const std::vector<Value>& args);
