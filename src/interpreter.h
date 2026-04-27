#pragma once

#include "ast.h"
#include "environment.h"
#include "value.h"
#include <functional>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class GcHeap;

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

    Value call(Interpreter&, const std::vector<Value>& args) override { return fn(args); }
    int arity() const override { return numArgs; }
    std::string name() const override { return funcName; }
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

    // Public so PraiaFunction::call can use it
    void executeBlock(const BlockStmt* block, std::shared_ptr<Environment> env);
    void checkInterrupt(int line, int column);
    std::shared_ptr<Environment> getGlobals() { return globals; }

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

    // (interpMutex removed — async tasks use task-local Interpreters instead)

    // Call stack for error traces
public:
    std::vector<CallFrame> callStack;
    std::string formatStackTrace() const;
    void gcMarkRoots(GcHeap& heap);
};
