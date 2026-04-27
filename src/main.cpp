#include "ast.h"
#include "grain_resolve.h"
#include "interpreter.h"
#include "signal_state.h"
#include "lexer.h"
#include "parser.h"
#include "vm/compiler.h"
#include "vm/vm.h"
#include "vm/debug.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <vector>
#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

// ── AST printer ──────────────────────────────────────────────

static void printIndent(int level) {
    for (int i = 0; i < level; i++) std::cout << "  ";
}

static void printExpr(const Expr* expr, int level);
static void printStmt(const Stmt* stmt, int level);

static void printExpr(const Expr* expr, int level) {
    if (!expr) { printIndent(level); std::cout << "<null>\n"; return; }

    switch (expr->type) {
    case ExprType::Number: {
        auto* e = static_cast<const NumberExpr*>(expr);
        printIndent(level);
        if (e->isInt) std::cout << "Int(" << e->intValue << ")\n";
        else std::cout << "Float(" << e->floatValue << ")\n";
        break;
    }
    case ExprType::String: {
        auto* e = static_cast<const StringExpr*>(expr);
        printIndent(level); std::cout << "String(\"" << e->value << "\")\n";
        break;
    }
    case ExprType::Bool: {
        auto* e = static_cast<const BoolExpr*>(expr);
        printIndent(level); std::cout << "Bool(" << (e->value ? "true" : "false") << ")\n";
        break;
    }
    case ExprType::Nil:
        printIndent(level); std::cout << "Nil\n";
        break;
    case ExprType::Identifier: {
        auto* e = static_cast<const IdentifierExpr*>(expr);
        printIndent(level); std::cout << "Ident(" << e->name << ")\n";
        break;
    }
    case ExprType::Unary: {
        auto* e = static_cast<const UnaryExpr*>(expr);
        printIndent(level); std::cout << "Unary(" << tokenTypeToString(e->op) << ")\n";
        printExpr(e->operand.get(), level + 1);
        break;
    }
    case ExprType::Binary: {
        auto* e = static_cast<const BinaryExpr*>(expr);
        printIndent(level); std::cout << "Binary(" << tokenTypeToString(e->op) << ")\n";
        printExpr(e->left.get(), level + 1);
        printExpr(e->right.get(), level + 1);
        break;
    }
    case ExprType::Postfix: {
        auto* e = static_cast<const PostfixExpr*>(expr);
        printIndent(level); std::cout << "Postfix(" << tokenTypeToString(e->op) << ")\n";
        printExpr(e->operand.get(), level + 1);
        break;
    }
    case ExprType::Assign: {
        auto* e = static_cast<const AssignExpr*>(expr);
        printIndent(level); std::cout << "Assign(" << e->name << ")\n";
        printExpr(e->value.get(), level + 1);
        break;
    }
    case ExprType::Call: {
        auto* e = static_cast<const CallExpr*>(expr);
        printIndent(level); std::cout << "Call\n";
        printExpr(e->callee.get(), level + 1);
        for (const auto& arg : e->args)
            printExpr(arg.get(), level + 1);
        break;
    }
    case ExprType::Ternary: {
        auto* e = static_cast<const TernaryExpr*>(expr);
        printIndent(level); std::cout << "Ternary\n";
        printExpr(e->condition.get(), level + 1);
        printExpr(e->thenExpr.get(), level + 1);
        printExpr(e->elseExpr.get(), level + 1);
        break;
    }
    case ExprType::Pipe: {
        auto* e = static_cast<const PipeExpr*>(expr);
        printIndent(level); std::cout << "Pipe\n";
        printExpr(e->left.get(), level + 1);
        printExpr(e->right.get(), level + 1);
        break;
    }
    case ExprType::Async: {
        auto* e = static_cast<const AsyncExpr*>(expr);
        printIndent(level); std::cout << "Async\n";
        printExpr(e->expr.get(), level + 1);
        break;
    }
    case ExprType::Await: {
        auto* e = static_cast<const AwaitExpr*>(expr);
        printIndent(level); std::cout << "Await\n";
        printExpr(e->expr.get(), level + 1);
        break;
    }
    case ExprType::This:
        printIndent(level); std::cout << "This\n";
        break;
    case ExprType::Super: {
        auto* e = static_cast<const SuperExpr*>(expr);
        printIndent(level); std::cout << "Super." << e->method << "\n";
        break;
    }
    case ExprType::InterpolatedString: {
        auto* e = static_cast<const InterpolatedStringExpr*>(expr);
        printIndent(level); std::cout << "InterpString\n";
        for (const auto& part : e->parts)
            printExpr(part.get(), level + 1);
        break;
    }
    case ExprType::Lambda: {
        auto* e = static_cast<const LambdaExpr*>(expr);
        printIndent(level); std::cout << "Lambda";
        if (!e->params.empty()) {
            std::cout << "(";
            for (size_t i = 0; i < e->params.size(); i++) {
                if (i > 0) std::cout << ", ";
                std::cout << e->params[i];
            }
            std::cout << ")";
        }
        std::cout << "\n";
        for (const auto& s : e->body)
            printStmt(s.get(), level + 1);
        break;
    }
    case ExprType::ArrayLiteral: {
        auto* e = static_cast<const ArrayLiteralExpr*>(expr);
        printIndent(level); std::cout << "ArrayLiteral\n";
        for (const auto& elem : e->elements)
            printExpr(elem.get(), level + 1);
        break;
    }
    case ExprType::Index: {
        auto* e = static_cast<const IndexExpr*>(expr);
        printIndent(level); std::cout << "Index\n";
        printExpr(e->object.get(), level + 1);
        printExpr(e->index.get(), level + 1);
        break;
    }
    case ExprType::IndexAssign: {
        auto* e = static_cast<const IndexAssignExpr*>(expr);
        printIndent(level); std::cout << "IndexAssign\n";
        printExpr(e->object.get(), level + 1);
        printExpr(e->index.get(), level + 1);
        printExpr(e->value.get(), level + 1);
        break;
    }
    case ExprType::MapLiteral: {
        auto* e = static_cast<const MapLiteralExpr*>(expr);
        printIndent(level); std::cout << "MapLiteral\n";
        for (size_t i = 0; i < e->keys.size(); i++) {
            printIndent(level + 1); std::cout << "key:\n";
            if (e->keys[i]) printExpr(e->keys[i].get(), level + 2);
            else { printIndent(level + 2); std::cout << "<spread>\n"; }
            printIndent(level + 1); std::cout << "value:\n";
            printExpr(e->values[i].get(), level + 2);
        }
        break;
    }
    case ExprType::Dot: {
        auto* e = static_cast<const DotExpr*>(expr);
        printIndent(level); std::cout << "Dot(." << e->field << ")\n";
        printExpr(e->object.get(), level + 1);
        break;
    }
    case ExprType::DotAssign: {
        auto* e = static_cast<const DotAssignExpr*>(expr);
        printIndent(level); std::cout << "DotAssign(." << e->field << ")\n";
        printExpr(e->object.get(), level + 1);
        printExpr(e->value.get(), level + 1);
        break;
    }
    case ExprType::Spread: {
        auto* e = static_cast<const SpreadExpr*>(expr);
        printIndent(level); std::cout << "Spread\n";
        printExpr(e->expr.get(), level + 1);
        break;
    }
    case ExprType::Yield: {
        auto* e = static_cast<const YieldExpr*>(expr);
        printIndent(level); std::cout << "Yield\n";
        if (e->value) printExpr(e->value.get(), level + 1);
        break;
    }
    default:
        printIndent(level); std::cout << "Unknown\n";
        break;
    }
}

static void printStmt(const Stmt* stmt, int level) {
    if (!stmt) { printIndent(level); std::cout << "<null>\n"; return; }

    switch (stmt->type) {
    case StmtType::Expr: {
        auto* s = static_cast<const ExprStmt*>(stmt);
        printIndent(level); std::cout << "ExprStmt\n";
        printExpr(s->expr.get(), level + 1);
        break;
    }
    case StmtType::Let: {
        auto* s = static_cast<const LetStmt*>(stmt);
        printIndent(level); std::cout << "Let(" << s->name << ")\n";
        if (s->initializer) printExpr(s->initializer.get(), level + 1);
        break;
    }
    case StmtType::Block: {
        auto* s = static_cast<const BlockStmt*>(stmt);
        printIndent(level); std::cout << "Block\n";
        for (const auto& child : s->statements)
            printStmt(child.get(), level + 1);
        break;
    }
    case StmtType::If: {
        auto* s = static_cast<const IfStmt*>(stmt);
        printIndent(level); std::cout << "If\n";
        printIndent(level + 1); std::cout << "cond:\n";
        printExpr(s->condition.get(), level + 2);
        printIndent(level + 1); std::cout << "then:\n";
        printStmt(s->thenBranch.get(), level + 2);
        for (const auto& elif : s->elifBranches) {
            printIndent(level + 1); std::cout << "elif:\n";
            printExpr(elif.condition.get(), level + 2);
            printStmt(elif.body.get(), level + 2);
        }
        if (s->elseBranch) {
            printIndent(level + 1); std::cout << "else:\n";
            printStmt(s->elseBranch.get(), level + 2);
        }
        break;
    }
    case StmtType::Match: {
        auto* s = static_cast<const MatchStmt*>(stmt);
        printIndent(level); std::cout << "Match\n";
        printExpr(s->subject.get(), level + 1);
        break;
    }
    case StmtType::While: {
        auto* s = static_cast<const WhileStmt*>(stmt);
        printIndent(level); std::cout << "While\n";
        printIndent(level + 1); std::cout << "cond:\n";
        printExpr(s->condition.get(), level + 2);
        printIndent(level + 1); std::cout << "body:\n";
        printStmt(s->body.get(), level + 2);
        break;
    }
    case StmtType::For: {
        auto* s = static_cast<const ForStmt*>(stmt);
        printIndent(level); std::cout << "For(" << s->varName << ")\n";
        printIndent(level + 1); std::cout << "from:\n";
        printExpr(s->start.get(), level + 2);
        printIndent(level + 1); std::cout << "to:\n";
        printExpr(s->end.get(), level + 2);
        printIndent(level + 1); std::cout << "body:\n";
        printStmt(s->body.get(), level + 2);
        break;
    }
    case StmtType::ForIn: {
        auto* s = static_cast<const ForInStmt*>(stmt);
        printIndent(level); std::cout << "ForIn(" << s->varName << ")\n";
        printIndent(level + 1); std::cout << "iterable:\n";
        printExpr(s->iterable.get(), level + 2);
        printIndent(level + 1); std::cout << "body:\n";
        printStmt(s->body.get(), level + 2);
        break;
    }
    case StmtType::Func: {
        auto* s = static_cast<const FuncStmt*>(stmt);
        printIndent(level); std::cout << "Func(" << s->name << ")";
        if (!s->params.empty()) {
            std::cout << " params:";
            for (const auto& p : s->params) std::cout << " " << p;
        }
        std::cout << "\n";
        printStmt(s->body.get(), level + 1);
        break;
    }
    case StmtType::Enum: {
        auto* s = static_cast<const EnumStmt*>(stmt);
        printIndent(level); std::cout << "Enum(" << s->name << ")\n";
        break;
    }
    case StmtType::Class: {
        auto* s = static_cast<const ClassStmt*>(stmt);
        printIndent(level); std::cout << "Class(" << s->name << ")";
        if (!s->superclass.empty()) std::cout << " extends " << s->superclass;
        std::cout << "\n";
        for (const auto& m : s->methods) {
            printIndent(level + 1); std::cout << "Method(" << m.name << ")";
            if (!m.params.empty()) {
                std::cout << " params:";
                for (const auto& p : m.params) std::cout << " " << p;
            }
            std::cout << "\n";
        }
        break;
    }
    case StmtType::Return: {
        auto* s = static_cast<const ReturnStmt*>(stmt);
        printIndent(level); std::cout << "Return\n";
        if (s->value) printExpr(s->value.get(), level + 1);
        break;
    }
    case StmtType::Break:
        printIndent(level); std::cout << "Break\n";
        break;
    case StmtType::Continue:
        printIndent(level); std::cout << "Continue\n";
        break;
    case StmtType::Throw: {
        auto* s = static_cast<const ThrowStmt*>(stmt);
        printIndent(level); std::cout << "Throw\n";
        printExpr(s->value.get(), level + 1);
        break;
    }
    case StmtType::TryCatch: {
        auto* s = static_cast<const TryCatchStmt*>(stmt);
        printIndent(level); std::cout << "TryCatch(" << s->errorVar << ")\n";
        printIndent(level + 1); std::cout << "try:\n";
        printStmt(s->tryBody.get(), level + 2);
        printIndent(level + 1); std::cout << "catch:\n";
        printStmt(s->catchBody.get(), level + 2);
        break;
    }
    case StmtType::Ensure: {
        auto* s = static_cast<const EnsureStmt*>(stmt);
        printIndent(level); std::cout << "Ensure\n";
        printIndent(level + 1); std::cout << "cond:\n";
        printExpr(s->condition.get(), level + 2);
        printIndent(level + 1); std::cout << "else:\n";
        printStmt(s->elseBody.get(), level + 2);
        break;
    }
    case StmtType::Use: {
        auto* s = static_cast<const UseStmt*>(stmt);
        printIndent(level); std::cout << "Use(\"" << s->path << "\") as " << s->alias << "\n";
        break;
    }
    case StmtType::Export: {
        auto* s = static_cast<const ExportStmt*>(stmt);
        printIndent(level); std::cout << "Export {";
        for (size_t i = 0; i < s->names.size(); i++) {
            if (i > 0) std::cout << ", ";
            std::cout << s->names[i];
        }
        std::cout << "}\n";
        break;
    }
    default:
        printIndent(level); std::cout << "Unknown\n";
        break;
    }
}

static void printAst(const std::vector<StmtPtr>& program) {
    for (const auto& stmt : program)
        printStmt(stmt.get(), 0);
}

// ── Main ─────────────────────────────────────────────────────

static constexpr const char* PRAIA_VERSION = "0.4.2";

static std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file '" << path << "'" << std::endl;
        exit(1);
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Lex + parse source into an AST. Returns empty vector on error or
// when --tokens/--ast consumed the output. Sets hadError on failures.
static std::vector<StmtPtr> compile(const std::string& source,
                                     bool showTokens, bool showAst,
                                     bool& hadError) {
    hadError = false;

    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    if (showTokens) {
        for (const auto& token : tokens)
            std::cout << token << std::endl;
        hadError = lexer.hasError();
        return {};
    }

    if (lexer.hasError()) { hadError = true; return {}; }

    Parser parser(tokens);
    auto program = parser.parse();

    if (parser.hasError()) { hadError = true; return {}; }

    if (showAst) {
        printAst(program);
        return {};
    }

    return program;
}

// readline wrapper — returns empty optional on EOF
static std::optional<std::string> readLine(const char* prompt) {
#ifdef HAVE_READLINE
    char* input = readline(prompt);
    if (!input) return std::nullopt;
    std::string result(input);
    free(input);
    return result;
#else
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
#endif
}

static void addHistory([[maybe_unused]] const std::string& line) {
#ifdef HAVE_READLINE
    add_history(line.c_str());
#endif
}

static void repl(bool showTokens, bool showAst) {
    std::cout << "Praia " << PRAIA_VERSION << " REPL (type 'exit' to quit)" << std::endl;
    Interpreter interpreter;

    // Keep all ASTs alive so function bodies (raw pointers) remain valid
    std::vector<std::vector<StmtPtr>> astStore;

    for (;;) {
        auto input = readLine(">> ");
        if (!input) break;              // Ctrl-D
        std::string line = *input;
        if (line == "exit") break;
        if (line.empty()) continue;

        // Multi-line: keep reading while braces are unbalanced.
        // Tracks braces correctly by skipping strings and comments.
        auto countBraces = [](const std::string& s) -> int {
            int depth = 0;
            char stringQuote = 0;  // tracks which quote opened the string
            bool inLineComment = false;
            bool inBlockComment = false;
            for (size_t i = 0; i < s.size(); i++) {
                char c = s[i];
                char next = (i + 1 < s.size()) ? s[i + 1] : '\0';

                if (inLineComment) {
                    if (c == '\n') inLineComment = false;
                    continue;
                }
                if (inBlockComment) {
                    if (c == '*' && next == '/') { inBlockComment = false; i++; }
                    continue;
                }
                if (stringQuote) {
                    if (c == '\\') { i++; continue; } // skip escaped char
                    if (c == stringQuote) stringQuote = 0;
                    continue;
                }

                if (c == '/' && next == '/') { inLineComment = true; i++; continue; }
                if (c == '/' && next == '*') { inBlockComment = true; i++; continue; }
                if (c == '"' || c == '\'') { stringQuote = c; continue; }
                if (c == '{') depth++;
                if (c == '}') depth--;
            }
            return depth;
        };

        int braces = countBraces(line);
        while (braces > 0) {
            auto cont = readLine(".. ");
            if (!cont) break;
            line += "\n" + *cont;
            braces = countBraces(line);
        }

        addHistory(line);

        bool hadError = false;
        auto program = compile(line, showTokens, showAst, hadError);
        if (!program.empty()) {
            interpreter.interpretRepl(program);
            astStore.push_back(std::move(program));
        }
    }
}

static void vmRepl(bool showTokens, bool showAst) {
    std::cout << "Praia " << PRAIA_VERSION << " REPL (type 'exit' to quit)" << std::endl;

    extern void vmRegisterNatives(VM& vm);
    VM vm;
    vmRegisterNatives(vm);

    std::vector<std::vector<StmtPtr>> astStore;

    auto countBraces = [](const std::string& s) -> int {
        int depth = 0;
        char stringQuote = 0;
        bool inLineComment = false, inBlockComment = false;
        for (size_t i = 0; i < s.size(); i++) {
            char c = s[i], next = (i + 1 < s.size()) ? s[i + 1] : '\0';
            if (inLineComment) { if (c == '\n') inLineComment = false; continue; }
            if (inBlockComment) { if (c == '*' && next == '/') { inBlockComment = false; i++; } continue; }
            if (stringQuote) { if (c == '\\') { i++; continue; } if (c == stringQuote) stringQuote = 0; continue; }
            if (c == '/' && next == '/') { inLineComment = true; i++; continue; }
            if (c == '/' && next == '*') { inBlockComment = true; i++; continue; }
            if (c == '"' || c == '\'') { stringQuote = c; continue; }
            if (c == '{') depth++;
            if (c == '}') depth--;
        }
        return depth;
    };

    for (;;) {
        auto input = readLine(">> ");
        if (!input) break;
        std::string line = *input;
        if (line == "exit") break;
        if (line.empty()) continue;

        int braces = countBraces(line);
        while (braces > 0) {
            auto cont = readLine(".. ");
            if (!cont) break;
            line += "\n" + *cont;
            braces = countBraces(line);
        }

        addHistory(line);

        bool hadError = false;
        auto program = compile(line, showTokens, showAst, hadError);
        if (program.empty()) continue;

        // Check if the last statement is a bare expression
        bool lastIsExpr = static_cast<const ExprStmt*>(program.back().get()) != nullptr;

        Compiler compiler;
        auto script = compiler.compile(program);
        if (!script) continue;

        // If the last statement is an expression, patch the bytecode:
        // The compiler emits: <expr> OP_POP ... OP_NIL OP_RETURN
        // We remove the OP_POP and OP_NIL so the expression value
        // flows directly into OP_RETURN as the script's result.
        if (lastIsExpr) {
            auto& code = script->chunk.code;
            // The epilogue is always the last 2 bytes: OP_NIL OP_RETURN
            // Before that, the ExprStmt's OP_POP is the byte just before OP_NIL
            int sz = static_cast<int>(code.size());
            if (sz >= 3 &&
                code[sz - 1] == static_cast<uint8_t>(OpCode::OP_RETURN) &&
                code[sz - 2] == static_cast<uint8_t>(OpCode::OP_NIL) &&
                code[sz - 3] == static_cast<uint8_t>(OpCode::OP_POP)) {
                // Remove OP_POP and OP_NIL, keep OP_RETURN
                code.erase(code.begin() + (sz - 3), code.begin() + (sz - 1));
            }
        }

        try {
            auto result = vm.runRepl(script);
            if (result == VM::Result::OK && lastIsExpr) {
                Value val = vm.pop();
                if (!val.isNil())
                    std::cout << val.toString() << "\n";
            }
        } catch (const ExitSignal&) {
            throw;
        }

        astStore.push_back(std::move(program));
    }
}

/* Run a single test file in its own interpreter. Returns 0 if the file
called sys.exit(0) (convention: testing.done() when all asserts passed),
anything else counts as a failure. Errors are kept on stderr so the user
can see them in the run log. */
static int runTestFile(const std::string& path, bool useVm) {
    std::string source = readFile(path);
    bool hadError = false;
    auto program = compile(source, /*showTokens*/false, /*showAst*/false, hadError);
    if (program.empty()) return 1; // parse/lex error

    if (useVm) {
        Compiler compiler;
        auto script = compiler.compile(program);
        if (!script) return 1;
        extern void vmRegisterNatives(VM& vm);
        VM vm;
        vmRegisterNatives(vm);
        vm.setCurrentFile(path);
        try {
            return vm.run(script) == VM::Result::OK ? 2 : 1;
        } catch (const ExitSignal& e) {
            return e.code;
        }
    } else {
        Interpreter interp;
        interp.setCurrentFile(path);
        try {
            interp.interpret(program);
        } catch (const ExitSignal& e) {
            return e.code;
        }
    }
    // No sys.exit means the file didn't call testing.done() — treat as fail
    return 2;
}

static int runTestsCommand(const std::string& dir, bool useVm) {
    namespace fs = std::filesystem;
    if (!fs::exists(dir)) {
        std::cerr << "praia test: directory not found: " << dir << std::endl;
        return 2;
    }

    std::vector<std::string> files;
    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto& p = entry.path();
        if (p.extension() == ".praia" &&
            p.filename().string().starts_with("test_")) {
            files.push_back(p.string());
        }
    }
    std::sort(files.begin(), files.end());

    if (files.empty()) {
        std::cerr << "praia test: no test_*.praia files in " << dir << std::endl;
        return 1;
    }

    int passed = 0, failed = 0;
    int total = static_cast<int>(files.size());
    std::vector<std::string> failedFiles;
    for (int fi = 0; fi < total; fi++) {
        auto& file = files[fi];

        // Progress bar: [####....] 3/10 test_foo.praia
        {
            int barWidth = 20;
            int filled = (fi * barWidth) / total;
            std::string bar(filled, '#');
            bar += std::string(barWidth - filled, '.');
            // Extract just the filename
            std::string fname = file;
            auto slash = fname.rfind('/');
            if (slash != std::string::npos) fname = fname.substr(slash + 1);
            std::cout << "\r\033[K[" << bar << "] " << fi << "/" << total
                      << " " << fname << std::flush;
        }

        std::cout << "\r\033[K── " << file << " ───────────────────────" << std::endl;
        int code = runTestFile(file, useVm);
        if (code == 0) {
            passed++;
        } else {
            failed++;
            failedFiles.push_back(file);
        }
        std::cout << std::endl;
    }
    // Final progress bar: complete
    {
        std::string bar(20, '#');
        std::cout << "\r\033[K[" << bar << "] " << total << "/" << total << " done" << std::endl;
    }

    std::cout << "═══ " << passed << "/" << files.size()
              << " test files passed ═══" << std::endl;
    if (!failedFiles.empty()) {
        std::cout << "Failed:" << std::endl;
        for (auto& f : failedFiles) std::cout << "  " << f << std::endl;
    }
    return failed == 0 ? 0 : 1;
}

int main(int argc, char* argv[]) {
    installDefaultSignalHandlers();

    // Resolve the directory where the praia binary lives.
    // Used by grain resolution to find bundled stdlib grains.
    {
        namespace fs = std::filesystem;
        fs::path arg0(argv[0]);
        if (arg0.is_absolute() || arg0.string().find('/') != std::string::npos) {
            // Absolute or relative path — resolve directly
            try { g_praiaInstallDir = fs::canonical(arg0).parent_path().string(); }
            catch (...) {}
        } else {
            // Bare name (e.g. "praia") — search PATH
            const char* pathEnv = std::getenv("PATH");
            if (pathEnv) {
                std::istringstream paths(pathEnv);
                std::string dir;
                while (std::getline(paths, dir, ':')) {
                    auto candidate = fs::path(dir) / arg0;
                    if (fs::exists(candidate)) {
                        g_praiaInstallDir = fs::canonical(candidate).parent_path().string();
                        break;
                    }
                }
            }
        }
    }

    bool showTokens = false;
    bool showAst = false;
    bool useVm = true; // VM is the default
    std::string filename;
    int fileArgIndex = -1;

    // `praia -v` / `praia --version`
    if (argc >= 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        std::cout << "Praia Version " << PRAIA_VERSION << std::endl;
        return 0;
    }

    // `praia --include-path` — print the header directory for native plugins
    if (argc >= 2 && std::string(argv[1]) == "--include-path") {
#ifdef PRAIA_LIBDIR
        std::cout << PRAIA_LIBDIR << "/include" << std::endl;
#else
        // Development mode: headers are in src/ relative to the binary
        std::cout << fs::canonical(fs::path(argv[0]).parent_path() / "src").string() << std::endl;
#endif
        return 0;
    }

    // `praia -h` / `praia --help`
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::cout << "Praia " << PRAIA_VERSION << "\n\n"
                  << "Usage:\n"
                  << "  praia                          Start the REPL\n"
                  << "  praia <file>                   Run a script\n"
                  << "  praia <file> [args...]          Run a script with arguments (sys.args)\n"
                  << "  praia -c '<code>' [args...]     Run a one-liner\n"
                  << "  praia test [dir]                Run test suite (default: tests/)\n"
                  << "\n"
                  << "Options:\n"
                  << "  -h, --help       Show this help message\n"
                  << "  -v, --version    Print version\n"
                  << "  -c <code>        Evaluate a string as code\n"
                  << "  --tree           Use tree-walker interpreter instead of VM\n"
                  << "  --tokens         Print lexer tokens and exit\n"
                  << "  --ast            Print parse tree and exit\n"
                  << "  --include-path   Print the header path for native plugins\n";
        return 0;
    }

    // `praia test [dir]` subcommand — scan past flags to find it
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--tree") useVm = false;
        else if (arg == "--vm") useVm = true;
        else if (arg == "test") {
            // Scan remaining args for flags and an optional directory
            std::string dir = "tests";
            for (int j = i + 1; j < argc; j++) {
                std::string a = argv[j];
                if (a == "--tree") useVm = false;
                else if (a == "--vm") useVm = true;
                else if (a[0] != '-') { dir = a; break; }
            }
            return runTestsCommand(dir, useVm);
        }
        else if (arg[0] != '-') break; // hit a non-flag, non-"test" arg (filename)
    }

    // Parse all flags first
    std::string cCode;
    bool hasCFlag = false;
    int cArgStart = -1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--tokens")        showTokens = true;
        else if (arg == "--ast")      showAst = true;
        else if (arg == "--vm")       useVm = true;
        else if (arg == "--tree")     useVm = false;
        else if (arg == "-c" && i + 1 < argc) {
            hasCFlag = true;
            cCode = argv[++i];
            cArgStart = i + 1;
            break;
        }
        else if (filename.empty()) { filename = arg; fileArgIndex = i; break; }
    }

    // `praia [-vm] -c "code"` — run a one-liner
    if (hasCFlag) {
        bool hadError = false;
        auto program = compile(cCode, showTokens, showAst, hadError);
        if (hadError) return 1;
        if (!program.empty()) {
            if (useVm) {
                Compiler compiler;
                auto script = compiler.compile(program);
                if (!script) return 1;
                extern void vmRegisterNatives(VM& vm);
                VM vm;
                vmRegisterNatives(vm);
                std::vector<std::string> scriptArgs;
                for (int i = cArgStart; i < argc; i++)
                    scriptArgs.push_back(argv[i]);
                vm.setArgs(scriptArgs);
                try { return vm.run(script) == VM::Result::OK ? 0 : 1; }
                catch (const ExitSignal& e) { return e.code; }
            } else {
                Interpreter interpreter;
                std::vector<std::string> scriptArgs;
                for (int i = cArgStart; i < argc; i++)
                    scriptArgs.push_back(argv[i]);
                interpreter.setArgs(scriptArgs);
                try {
                    if (!interpreter.interpret(program)) return 1;
                }
                catch (const ExitSignal& e) { return e.code; }
            }
        }
        return 0;
    }

    if (filename.empty()) {
        try {
            if (useVm) vmRepl(showTokens, showAst);
            else repl(showTokens, showAst);
        }
        catch (const ExitSignal& e) { return e.code; }
        return 0;
    }

    // Collect script arguments (everything after the filename)
    std::vector<std::string> scriptArgs;
    for (int i = fileArgIndex + 1; i < argc; i++)
        scriptArgs.push_back(argv[i]);

    std::string source = readFile(filename);
    bool hadError = false;
    auto program = compile(source, showTokens, showAst, hadError);
    if (hadError) return 1;
    if (!program.empty()) {
        if (useVm) {
            // Bytecode VM path
            Compiler compiler;
            auto script = compiler.compile(program);
            if (!script) return 1;

            extern void vmRegisterNatives(VM& vm);
            VM vm;
            vmRegisterNatives(vm);
            vm.setArgs(scriptArgs);
            vm.setCurrentFile(filename);
            try {
                auto result = vm.run(script);
                return result == VM::Result::OK ? 0 : 1;
            } catch (const ExitSignal& e) { return e.code; }
        } else {
            // Tree-walker path
            Interpreter interpreter;
            interpreter.setArgs(scriptArgs);
            interpreter.setCurrentFile(filename);
            try {
                if (!interpreter.interpret(program)) return 1;
            }
            catch (const ExitSignal& e) { return e.code; }
        }
    }
    return 0;
}
