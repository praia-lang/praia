#pragma once

#include "../ast.h"
#include "../value.h"
#include "chunk.h"
#include <memory>
#include <string>
#include <vector>

struct CompiledFunction {
    std::string name;
    int arity = 0;
    int upvalueCount = 0;
    Chunk chunk;
    std::vector<std::string> paramNames; // for named argument support
    std::string restParam;               // empty = no rest param
    bool isGenerator = false;
};

class Compiler {
public:
    std::shared_ptr<CompiledFunction> compile(const std::vector<StmtPtr>& program);
    bool hasError() const { return hadError; }

private:
    struct Local {
        std::string name;
        int depth;
        bool isCaptured = false;
    };

    struct Upvalue {
        uint16_t index;
        bool isLocal;
    };

    struct LoopContext {
        int loopStart;         // condition check
        int continueTarget;    // where continue should jump (increment section)
        std::vector<int> breakJumps;
        std::vector<int> continueJumps;
        int scopeDepthAtLoop;
        int tryDepthAtLoop;    // try nesting depth when the loop started
    };

    struct CompilerState {
        CompilerState* enclosing = nullptr;
        std::shared_ptr<CompiledFunction> function;
        std::vector<Local> locals;
        std::vector<Upvalue> upvalues;
        int scopeDepth = 0;
        int tryDepth = 0;      // number of active try handlers in this function
        bool isGenerator = false;
        std::vector<LoopContext> loops;
        std::vector<const Stmt*> finallyStack; // enclosing finally bodies for return/break/continue
    };

    CompilerState* current = nullptr;

    // Statements
    void compileStmt(const Stmt* stmt);
    void compileExprStmt(const ExprStmt* stmt);
    void compileLetStmt(const LetStmt* stmt);
    void compileBlockStmt(const BlockStmt* stmt);
    void compileIfStmt(const IfStmt* stmt);
    void compileMatchStmt(const MatchStmt* stmt);
    void compileWhileStmt(const WhileStmt* stmt);
    void compileForStmt(const ForStmt* stmt);
    void compileForInStmt(const ForInStmt* stmt);
    void compileFuncStmt(const FuncStmt* stmt);
    void compileReturnStmt(const ReturnStmt* stmt);
    void compileBreakStmt(const BreakStmt* stmt);
    void compileContinueStmt(const ContinueStmt* stmt);
    void compileClassStmt(const ClassStmt* stmt);
    void compileEnumStmt(const EnumStmt* stmt);
    void compileThrowStmt(const ThrowStmt* stmt);
    void compileTryCatchStmt(const TryCatchStmt* stmt);
    void compileEnsureStmt(const EnsureStmt* stmt);
    void compileUseStmt(const UseStmt* stmt);
    void compileExportStmt(const ExportStmt* stmt);
    // Expressions
    void compileExpr(const Expr* expr);
    void compileNumberExpr(const NumberExpr* expr);
    void compileStringExpr(const StringExpr* expr);
    void compileBoolExpr(const BoolExpr* expr);
    void compileNilExpr(const NilExpr* expr);
    void compileIdentifierExpr(const IdentifierExpr* expr);
    void compileAssignExpr(const AssignExpr* expr);
    void compileUnaryExpr(const UnaryExpr* expr);
    void compilePostfixExpr(const PostfixExpr* expr);
    void compileBinaryExpr(const BinaryExpr* expr);
    void compileCallExpr(const CallExpr* expr);
    void compileTernaryExpr(const TernaryExpr* expr);
    void compilePipeExpr(const PipeExpr* expr);
    void compilePipeTryExpr(const PipeTryExpr* expr);
    void compileLambdaExpr(const LambdaExpr* expr);
    void compileArrayLiteralExpr(const ArrayLiteralExpr* expr);
    void compileMapLiteralExpr(const MapLiteralExpr* expr);
    void compileIndexExpr(const IndexExpr* expr);
    void compileIndexAssignExpr(const IndexAssignExpr* expr);
    void compileDotExpr(const DotExpr* expr);
    void compileDotAssignExpr(const DotAssignExpr* expr);
    void compileInterpolatedStringExpr(const InterpolatedStringExpr* expr);
    void compileThisExpr(const ThisExpr* expr);
    void compileSuperExpr(const SuperExpr* expr);
    void compileAsyncExpr(const AsyncExpr* expr);
    void compileAwaitExpr(const AwaitExpr* expr);
    void compileYieldExpr(const YieldExpr* expr);

    // Scope
    void beginScope();
    void endScope(int line);
    void addLocal(const std::string& name);
    int resolveLocal(CompilerState* state, const std::string& name);
    int resolveUpvalue(CompilerState* state, const std::string& name);
    int addUpvalue(CompilerState* state, uint16_t index, bool isLocal);

    // Emit helpers
    Chunk& currentChunk();
    void emit(OpCode op, int line, int column = 0);
    void emit(uint8_t byte, int line, int column = 0);
    void emitU16(uint16_t val, int line, int column = 0);
    void emitConstant(Value value, int line, int column = 0);
    int emitJump(OpCode op, int line, int column = 0);
    void patchJump(int offset);
    void emitLoop(int loopStart, int line, int column = 0);
    uint16_t identifierConstant(const std::string& name);

    void error(const std::string& msg, int line);
    bool hadError = false;
};
