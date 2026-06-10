#pragma once

#include "token.h"
#include <memory>
#include <string>
#include <vector>

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// --- Type tags for dispatch without dynamic_cast ---

enum class ExprType {
    Number, String, Bool, Nil, Identifier,
    Unary, Binary, Postfix, Assign, Call,
    InterpolatedString, Lambda, Ternary, Spread,
    ArrayLiteral, Index, IndexAssign, MapLiteral, SetLiteral,
    Dot, DotAssign, Pipe, PipeTry, Async, Await, Yield,
    This, Super, Match
};

enum class StmtType {
    Expr, Let, Block, If, While,
    For, ForIn, Func, Return, Class, Enum,
    Break, Continue, Throw, TryCatch, Ensure,
    Defer, Use, Export
};

// --- Expressions ---

struct Expr {
    ExprType type;
    int line = 0;
    int column = 0;
    Expr(ExprType t) : type(t) {}
    virtual ~Expr() = default;
};

struct NumberExpr : Expr {
    NumberExpr() : Expr(ExprType::Number) {}
    double floatValue = 0;
    int64_t intValue = 0;
    bool isInt = false;
};

struct StringExpr : Expr {
    StringExpr() : Expr(ExprType::String) {}
    std::string value;
};

struct BoolExpr : Expr {
    BoolExpr() : Expr(ExprType::Bool) {}
    bool value;
};

struct NilExpr : Expr {
    NilExpr() : Expr(ExprType::Nil) {}
};

struct IdentifierExpr : Expr {
    IdentifierExpr() : Expr(ExprType::Identifier) {}
    std::string name;
};

struct UnaryExpr : Expr {
    UnaryExpr() : Expr(ExprType::Unary) {}
    TokenType op;
    ExprPtr operand;
};

struct BinaryExpr : Expr {
    BinaryExpr() : Expr(ExprType::Binary) {}
    ExprPtr left;
    TokenType op;
    ExprPtr right;
};

struct PostfixExpr : Expr {
    PostfixExpr() : Expr(ExprType::Postfix) {}
    ExprPtr operand;
    TokenType op;
};

struct AssignExpr : Expr {
    AssignExpr() : Expr(ExprType::Assign) {}
    std::string name;
    ExprPtr value;
};

struct CallExpr : Expr {
    CallExpr() : Expr(ExprType::Call) {}
    ExprPtr callee;
    std::vector<ExprPtr> args;
    std::vector<std::string> argNames; // parallel to args; empty string = positional
};

// Parts alternate: string literal, expression, string literal, expression, ..., string literal
struct InterpolatedStringExpr : Expr {
    InterpolatedStringExpr() : Expr(ExprType::InterpolatedString) {}
    std::vector<ExprPtr> parts;
};

// lam{ params in body }
struct LambdaExpr : Expr {
    LambdaExpr() : Expr(ExprType::Lambda) {}
    std::vector<std::string> params;
    std::vector<ExprPtr> defaults; // parallel to params, nullptr if no default
    std::string restParam;         // empty = no rest param
    std::vector<StmtPtr> body;
    bool isGenerator = false;
};

struct TernaryExpr : Expr {
    TernaryExpr() : Expr(ExprType::Ternary) {}
    ExprPtr condition;
    ExprPtr thenExpr;
    ExprPtr elseExpr;
};

struct SpreadExpr : Expr {
    SpreadExpr() : Expr(ExprType::Spread) {}
    ExprPtr expr;
};

struct ArrayLiteralExpr : Expr {
    ArrayLiteralExpr() : Expr(ExprType::ArrayLiteral) {}
    std::vector<ExprPtr> elements; // may contain SpreadExpr nodes
};

struct IndexExpr : Expr {
    IndexExpr() : Expr(ExprType::Index) {}
    ExprPtr object;
    ExprPtr index;
    bool isOptional = false;
};

struct IndexAssignExpr : Expr {
    IndexAssignExpr() : Expr(ExprType::IndexAssign) {}
    ExprPtr object;
    ExprPtr index;
    ExprPtr value;
};

struct MapLiteralExpr : Expr {
    MapLiteralExpr() : Expr(ExprType::MapLiteral) {}
    std::vector<ExprPtr> keys;   // StringExpr for name:/string, expression for [expr]:, nullptr for spread
    std::vector<ExprPtr> values;
};

struct SetLiteralExpr : Expr {
    SetLiteralExpr() : Expr(ExprType::SetLiteral) {}
    std::vector<ExprPtr> elements; // may contain SpreadExpr nodes
};

struct DotExpr : Expr {
    DotExpr() : Expr(ExprType::Dot) {}
    ExprPtr object;
    std::string field;
    bool isOptional = false;
};

struct DotAssignExpr : Expr {
    DotAssignExpr() : Expr(ExprType::DotAssign) {}
    ExprPtr object;
    std::string field;
    ExprPtr value;
};

// a |> f becomes f(a), a |> f(x) becomes f(a, x)
struct PipeExpr : Expr {
    PipeExpr() : Expr(ExprType::Pipe) {}
    ExprPtr left;
    ExprPtr right;  // function or call expression
};

// a |?> f — if left throws, pass error to f; otherwise pass through
struct PipeTryExpr : Expr {
    PipeTryExpr() : Expr(ExprType::PipeTry) {}
    ExprPtr left;
    ExprPtr right;
};

struct AsyncExpr : Expr {
    AsyncExpr() : Expr(ExprType::Async) {}
    ExprPtr expr;  // the call expression to run async
};

struct AwaitExpr : Expr {
    AwaitExpr() : Expr(ExprType::Await) {}
    ExprPtr expr;  // the future to await
};

struct YieldExpr : Expr {
    YieldExpr() : Expr(ExprType::Yield) {}
    ExprPtr value;  // nullptr = yield nil
};

struct ThisExpr : Expr {
    ThisExpr() : Expr(ExprType::This) {}
};

struct SuperExpr : Expr {
    SuperExpr() : Expr(ExprType::Super) {}
    std::string method; // super.method
};

// --- Statements ---

struct Stmt {
    StmtType type;
    int line = 0;
    int column = 0;
    Stmt(StmtType t) : type(t) {}
    virtual ~Stmt() = default;
};

struct ExprStmt : Stmt {
    ExprStmt() : Stmt(StmtType::Expr) {}
    ExprPtr expr;
};

// Destructuring pattern element
struct PatternEntry {
    std::string name;       // variable to bind
    std::string key;        // for maps: the key to extract (empty = same as name)
    bool isRest = false;    // ...rest
};

struct LetStmt : Stmt {
    LetStmt() : Stmt(StmtType::Let) {}
    // Simple: name is set, pattern is empty
    // Array destructuring: pattern has entries, isArrayPattern = true
    // Map destructuring: pattern has entries, isArrayPattern = false
    std::string name;
    std::vector<PatternEntry> pattern;
    bool isArrayPattern = false;
    ExprPtr initializer;
};

struct BlockStmt : Stmt {
    BlockStmt() : Stmt(StmtType::Block) {}
    std::vector<StmtPtr> statements;
};

struct IfStmt : Stmt {
    IfStmt() : Stmt(StmtType::If) {}
    ExprPtr condition;
    StmtPtr thenBranch;

    struct ElifBranch {
        ExprPtr condition;
        StmtPtr body;
    };
    std::vector<ElifBranch> elifBranches;

    StmtPtr elseBranch; // nullptr if no else
};

// `match` always parses to a MatchExpr. In expression position
// (`let r = match ...`, return value, function arg, etc.) the
// parser enforces a mandatory `_` arm so the expression cannot
// fall through with no value. In statement position the parser
// wraps a MatchExpr in an ExprStmt (its value is discarded) and
// does not require a default — preserving the original silent-
// fallthrough semantics. Arm bodies are statement blocks; in
// expression position the trailing ExpressionStatement's value
// is the arm's value (same rule lambda bodies use).
struct MatchExpr : Expr {
    MatchExpr() : Expr(ExprType::Match) {}
    ExprPtr subject;
    struct CaseBranch {
        ExprPtr pattern; // equality pattern (nullptr if not equality)
        ExprPtr guard;   // when condition (nullptr if not guard)
        ExprPtr isType;  // is type pattern (nullptr if not type)
        StmtPtr body;
        // All three nullptr = default (_)
    };
    std::vector<CaseBranch> cases;
};

struct WhileStmt : Stmt {
    WhileStmt() : Stmt(StmtType::While) {}
    ExprPtr condition;
    StmtPtr body;
};

// for (varName in start..end) { body }
struct ForStmt : Stmt {
    ForStmt() : Stmt(StmtType::For) {}
    std::string varName;
    ExprPtr start;
    ExprPtr end;
    StmtPtr body;
};

// for (varName in iterable) { body }
// for ({key, value} in iterable) { body }  — map destructuring
struct ForInStmt : Stmt {
    ForInStmt() : Stmt(StmtType::ForIn) {}
    std::string varName;                    // simple: "entry"
    std::vector<std::string> destructureKeys; // destructuring: ["key", "value"]
    ExprPtr iterable;
    StmtPtr body;
};

struct FuncStmt : Stmt {
    FuncStmt() : Stmt(StmtType::Func) {}
    std::string name;
    std::vector<std::string> params;
    std::vector<ExprPtr> defaults; // parallel to params, nullptr if no default
    std::string restParam;         // empty = no rest param
    StmtPtr body;
    bool isGenerator = false;
};

struct ReturnStmt : Stmt {
    ReturnStmt() : Stmt(StmtType::Return) {}
    ExprPtr value; // nullptr for bare return
};

struct ClassMethod {
    std::string name;
    std::vector<std::string> params;
    std::vector<ExprPtr> defaults; // parallel to params, nullptr if no default
    std::string restParam;         // empty = no rest param
    std::vector<ExprPtr> decorators;
    std::vector<StmtPtr> body;
    int line = 0;
    int column = 0;
    bool isGenerator = false;
    bool isStatic = false;
};

struct ClassStmt : Stmt {
    ClassStmt() : Stmt(StmtType::Class) {}
    std::string name;
    std::string superclass; // empty if no superclass
    std::vector<ClassMethod> methods;
};

// enum Name { A, B = 5, C }
struct EnumStmt : Stmt {
    EnumStmt() : Stmt(StmtType::Enum) {}
    std::string name;
    std::vector<std::string> members;
    std::vector<ExprPtr> values; // nullptr = auto-increment
};

struct BreakStmt : Stmt {
    BreakStmt() : Stmt(StmtType::Break) {}
};
struct ContinueStmt : Stmt {
    ContinueStmt() : Stmt(StmtType::Continue) {}
};

struct ThrowStmt : Stmt {
    ThrowStmt() : Stmt(StmtType::Throw) {}
    ExprPtr value;
};

// try { body } catch (varName) { handler } finally { cleanup }
struct TryCatchStmt : Stmt {
    TryCatchStmt() : Stmt(StmtType::TryCatch) {}
    StmtPtr tryBody;
    std::string errorVar;
    StmtPtr catchBody;
    StmtPtr finallyBody; // nullptr if no finally
};

// Two shapes share this node:
//   • Conditional: `ensure (cond) else { body }` — elseBody runs
//     when `condition` is falsy. `bindingName` is empty.
//   • Optional-unwrap (Swift `guard let`-style):
//     `ensure let <name> = <expr> else { body }` — `condition`
//     holds the expression; when its value is nil, elseBody runs.
//     Otherwise the value is bound to `<name>` in the enclosing
//     scope. `bindingName` is the identifier.
struct EnsureStmt : Stmt {
    EnsureStmt() : Stmt(StmtType::Ensure) {}
    ExprPtr condition;
    StmtPtr elseBody;
    std::string bindingName;   // empty for the conditional form
};

struct DeferStmt : Stmt {
    DeferStmt() : Stmt(StmtType::Defer) {}
    ExprPtr expr;
};

// use "path"  — imports a grain, binds to variable named after the file
struct UseStmt : Stmt {
    UseStmt() : Stmt(StmtType::Use) {}
    std::string path;    // the string literal from use "path"
    std::string alias;   // derived variable name (e.g. "math" from "utils/math")
};

// export { name1, name2, ... }
struct ExportStmt : Stmt {
    ExportStmt() : Stmt(StmtType::Export) {}
    std::vector<std::string> names;
};
