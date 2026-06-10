#pragma once

#include "ast.h"
#include "environment.h"
#include "value.h"
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class GcHeap;
class Interpreter;

// Thread-local pointer to the Interpreter that is currently executing a
// native function on this thread. Set by NativeFunction::call before the
// native body runs; natives that need to invoke a user-supplied callable
// (Lock.withLock, SharedMap.update, etc.) must call back through this
// interpreter rather than a registration-time captured pointer — otherwise
// a native called from an `async` task thread would mutate the *parent*
// Interpreter's env field while the parent is running, causing
// "Undefined variable" failures and worse. Mirrors VM::current() for the
// bytecode engine.
extern thread_local Interpreter* g_currentInterp;

// Thrown by return statements to unwind back to the enclosing function call
struct ReturnSignal {
    Value value;
};

// Thrown by break/continue to unwind to the enclosing loop
struct BreakSignal {};
struct ContinueSignal {};

// Thrown by throw statements, caught by try/catch
struct ThrowSignal {
    Value value;
    int line;
    int column = 0;
};

// A user-defined Praia function
struct PraiaFunction : Callable {
    std::string funcName;
    std::vector<std::string> params;
    const std::vector<ExprPtr>* defaults = nullptr; // from FuncStmt, nullptr entries = no default
    std::string restParam;
    const BlockStmt* body;
    std::shared_ptr<Environment> closure;
    std::shared_ptr<void> astOwner; // prevents AST deallocation while this callable exists

    Value call(Interpreter& interp, const std::vector<Value>& args) override;
    int arity() const override { return restParam.empty() ? static_cast<int>(params.size()) : -1; }
    std::string name() const override { return funcName; }
    const std::vector<std::string>* paramNames() const override { return &params; }
};

// A lambda (anonymous function)
struct PraiaLambda : Callable {
    std::vector<std::string> params;
    std::string restParam;
    const LambdaExpr* expr;  // points into AST (kept alive by astOwner)
    std::shared_ptr<Environment> closure;
    std::shared_ptr<void> astOwner;

    Value call(Interpreter& interp, const std::vector<Value>& args) override;
    int arity() const override { return restParam.empty() ? static_cast<int>(params.size()) : -1; }
    std::string name() const override { return "<lambda>"; }
    const std::vector<std::string>* paramNames() const override { return &params; }
};

// A generator function — calling it returns a PraiaGenerator instead of executing
struct PraiaGeneratorFunction : Callable {
    std::string funcName;
    std::vector<std::string> params;
    const std::vector<ExprPtr>* defaults = nullptr;
    std::string restParam;
    const BlockStmt* body;
    std::shared_ptr<Environment> closure;
    std::shared_ptr<void> astOwner;

    Value call(Interpreter& interp, const std::vector<Value>& args) override;
    int arity() const override { return restParam.empty() ? static_cast<int>(params.size()) : -1; }
    std::string name() const override { return funcName; }
    const std::vector<std::string>* paramNames() const override { return &params; }
};

// A generator lambda
struct PraiaGeneratorLambda : Callable {
    std::vector<std::string> params;
    std::string restParam;
    const LambdaExpr* expr;
    std::shared_ptr<Environment> closure;
    std::shared_ptr<void> astOwner;

    Value call(Interpreter& interp, const std::vector<Value>& args) override;
    int arity() const override { return restParam.empty() ? static_cast<int>(params.size()) : -1; }
    std::string name() const override { return "<generator>"; }
    const std::vector<std::string>* paramNames() const override { return &params; }
};

// A built-in native function
struct NativeFunction : Callable {
    std::string funcName;
    int numArgs;  // -1 = variadic
    std::function<Value(const std::vector<Value>&)> fn;
    // Optional parameter names — populated when the native opts in
    // to named-argument calls via the trailing `paramNames` arg of
    // `makeNative`. Empty means "no named-arg support"; the base
    // class's `paramNames()` returns nullptr in that case and the
    // engine raises the standard "Named arguments not supported"
    // error. Defining the names here lets `mod.fn(x: 1, y: 2)`
    // dispatch into plugin and stdlib natives just like it does
    // into Praia-defined functions.
    std::vector<std::string> paramNames_;

    Value call(Interpreter& interp, const std::vector<Value>& args) override {
        // Defense-in-depth: any C++ stdlib exception that escapes the
        // native (e.g. std::invalid_argument from std::stoi on bad input,
        // std::bad_alloc, std::regex_error, fs::filesystem_error) becomes
        // a Praia RuntimeError tagged with the function name. Without
        // this, those exceptions propagate to main() and terminate the
        // process. RuntimeError is rethrown unchanged so its line/column
        // info is preserved.
        //
        // Track the calling Interpreter in a thread-local so natives that
        // need to invoke user callbacks (Lock.withLock, SharedMap.update)
        // route them through the *caller's* interpreter rather than a
        // registration-time captured pointer. Nested native calls chain
        // via save/restore.
        Interpreter* prev = g_currentInterp;
        g_currentInterp = &interp;
        try {
            Value result = fn(args);
            g_currentInterp = prev;
            return result;
        } catch (const RuntimeError&) {
            g_currentInterp = prev;
            throw;
        } catch (const std::exception& e) {
            g_currentInterp = prev;
            throw RuntimeError(funcName + "(): " + e.what(), 0);
        } catch (...) {
            g_currentInterp = prev;
            throw;
        }
    }
    int arity() const override { return numArgs; }
    std::string name() const override { return funcName; }
    const std::vector<std::string>* paramNames() const override {
        return paramNames_.empty() ? nullptr : &paramNames_;
    }
};

// A method bound to an instance
struct PraiaMethod : Callable {
    std::string methodName;
    std::vector<std::string> params;
    const ClassMethod* decl;  // points into the ClassStmt AST
    std::shared_ptr<Environment> closure;
    std::shared_ptr<void> astOwner;
    std::shared_ptr<PraiaInstance> instance;
    std::shared_ptr<PraiaClass> definingClass;  // class where this method is defined

    Value call(Interpreter& interp, const std::vector<Value>& args) override;
    int arity() const override {
        return (decl && !decl->restParam.empty()) ? -1 : static_cast<int>(params.size());
    }
    std::string name() const override { return methodName; }
    const std::vector<std::string>* paramNames() const override { return &params; }
};

// A class (callable to create instances)
struct PraiaClass : Callable, std::enable_shared_from_this<PraiaClass> {
    std::string className;
    std::shared_ptr<PraiaClass> superclass;
    std::unordered_map<std::string, const ClassMethod*> methods;        // tree-walker: instance methods
    std::unordered_map<std::string, const ClassMethod*> staticMethods; // tree-walker: static methods
    std::unordered_map<std::string, std::vector<Value>> methodDecorators; // decorator callables per method
    std::unordered_map<std::string, Value> vmMethods;                  // VM: instance methods
    std::unordered_map<std::string, Value> vmStaticMethods;            // VM: static methods
    std::shared_ptr<Environment> closure;
    std::shared_ptr<void> astOwner;

    // True if this class (or any ancestor) declares any dunder method
    // (__add, __eq, __index, etc.). Lets the operator dispatch hot path
    // skip the chain walk + dynamic_cast when no overloads exist — which
    // is the overwhelming majority of classes. Set in OP_METHOD/OP_INHERIT
    // (VM) and in StmtType::Class (tree-walker).
    bool hasOperatorOverloads = false;

    Value call(Interpreter& interp, const std::vector<Value>& args) override;
    int arity() const override;
    std::string name() const override { return className; }
    const std::vector<std::string>* paramNames() const override {
        // Tree-walker path
        auto* init = findMethod("init");
        if (init) return &init->params;
        // VM path: check vmMethods
        auto it = vmMethods.find("init");
        if (it != vmMethods.end() && it->second.isCallable())
            return it->second.asCallable()->paramNames();
        return nullptr;
    }

    const ClassMethod* findMethod(const std::string& name) const;
};

// Thrown by sys.exit() to terminate cleanly
struct ExitSignal {
    int code;
};

// Thrown by export statements to deliver exports back to the importer
struct ExportSignal {
    std::shared_ptr<PraiaMap> exports;
};

// Stack trace support
struct CallFrame {
    std::string name;
    int line;
    int column = 0;
};

class CallFrameGuard {
    std::vector<CallFrame>& stack_;
public:
    CallFrameGuard(std::vector<CallFrame>& s, const std::string& name, int line)
        : stack_(s) { stack_.push_back({name, line}); }
    ~CallFrameGuard() { if (!stack_.empty()) stack_.pop_back(); }
    CallFrameGuard(const CallFrameGuard&) = delete;
    CallFrameGuard& operator=(const CallFrameGuard&) = delete;
};

class Interpreter {
    friend struct PraiaFunction;
    friend struct PraiaLambda;
    friend struct PraiaMethod;
    friend struct PraiaGeneratorFunction;
    friend struct PraiaGeneratorLambda;
    friend Value makeGeneratorFromEnv(
        std::shared_ptr<Environment>, std::shared_ptr<Environment>,
        const std::vector<StmtPtr>&, std::shared_ptr<PraiaGenerator>);
public:
    Interpreter();
    // Lightweight constructor for async tasks — shares globals, owns env
    explicit Interpreter(std::shared_ptr<Environment> sharedGlobals)
        : globals(sharedGlobals), env(sharedGlobals) {}
    bool interpret(const std::vector<StmtPtr>& program);
    void interpretRepl(const std::vector<StmtPtr>& program);
    void setArgs(const std::vector<std::string>& args);
    void setCurrentFile(const std::string& path);
    // Toggle --strict-tags mode (capitalized call to undefined name
    // becomes a RuntimeError instead of building a PraiaTagged).
    // Default off; main.cpp sets it from the CLI flag before
    // interpret() runs.
    void setStrictTags(bool v) { strictTags_ = v; }

    // Public so PraiaFunction::call / PraiaLambda::call can use it
    void executeBlock(const BlockStmt* block, std::shared_ptr<Environment> env);
    void checkInterrupt(int line, int column);
    std::shared_ptr<Environment> getGlobals() { return globals; }

    // Cross-thread postToEngine: a worker thread (libuv callback,
    // std::thread, etc.) enqueues a deferred call here; the engine
    // drains the queue at its next checkInterrupt yield point.
    void enqueuePosted(std::shared_ptr<Callable> fn, std::vector<Value> args);
    void drainPosted();

    // Shared function/lambda call implementation
    Value callBody(std::shared_ptr<Environment> callEnv,
                   const std::vector<std::string>& params,
                   const std::string& restParam,
                   const std::vector<Value>& args,
                   std::function<const Expr*(size_t)> getDefault,
                   std::function<void()> runBody);

private:
    Value evaluate(const Expr* expr);
    void execute(const Stmt* stmt);

    // Grain loading
    Value loadGrain(const std::string& path, int line);
    std::string resolveGrainPath(const std::string& path, int line);

    std::shared_ptr<Environment> globals;
    std::shared_ptr<Environment> env;  // current scope
    std::shared_ptr<PraiaMap> sysMap;

    // Module system state
    std::string currentFile;                              // path of currently executing file
    std::unordered_map<std::string, Value> grainCache;    // resolved path -> cached exports
    std::set<std::string> importedInCurrentFile;          // tracks per-file duplicate imports

    // AST storage — shared_ptr so callables can co-own the AST they reference
    using AstProgram = std::shared_ptr<std::vector<StmtPtr>>;
    std::vector<AstProgram> grainAsts;
    AstProgram currentAstOwner_; // set during interpret()/loadGrain() so callables can capture it

    // Saved environments for GC root tracking — the tree-walker stores caller
    // scopes on the C++ call stack, invisible to GC. This explicit stack
    // makes them reachable during mark phase.
    std::vector<std::shared_ptr<Environment>> savedEnvStack_;

    // Defer stacks — one per function call. Each is a list of expressions
    // to evaluate in reverse order when the function exits.
    std::vector<std::vector<const Expr*>> deferStacks_;

    // Pending "filled" mask for the next call. Bits set = positions actually
    // provided by the caller. Default (UINT64_MAX) means "all provided"
    // (positional call). Named-arg call sites set this before invoking, and
    // the receiving (user-function) call path consumes-and-resets it on
    // entry. Native callees are never given the mask — see the named-arg
    // dispatch in interpreter.cpp where the mask-set is skipped for natives.
    uint64_t pendingArgsFilled_ = ~0ULL;

    // Tagged-value typo defenses. `strictTags_` is the `--strict-tags`
    // CLI flag: when true, the tag-construction fallback (capitalized
    // call to an undefined name) becomes a hard RuntimeError instead of
    // silently building a PraiaTagged. `warnedTagNames_` dedupes the
    // typo warning so a typoed name inside a loop fires once, not once
    // per iteration. Both are read at the tag fallback site in
    // Interpreter::evaluate's Call handler.
    bool strictTags_ = false;
    std::unordered_set<std::string> warnedTagNames_;

    // (interpMutex removed — async tasks use task-local Interpreters instead)

    // ── Cross-thread postToEngine queue ──
    //
    // Worker threads call praia::postToEngine() to schedule a callable
    // for execution on this engine. The queue is drained at every
    // checkInterrupt() yield point (which fires once per statement
    // and once per expression). postedPending_ is a lock-free
    // fast-path: drainPosted() checks it before acquiring postedMutex_,
    // so the steady-state overhead per yield point is one atomic load.
    struct PostedCall {
        std::shared_ptr<Callable> fn;
        std::vector<Value> args;
    };
    std::mutex postedMutex_;
    std::vector<PostedCall> postedQueue_;
    std::atomic<bool> postedPending_{false};

    // Call stack for error traces
public:
    std::vector<CallFrame> callStack;
    std::string formatStackTrace() const;
    void gcMarkRoots(GcHeap& heap);
};
