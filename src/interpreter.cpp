#include "builtins.h"
#include "fiber.h"
#include "gc_heap.h"
#include "grain_resolve.h"
#include "interpreter.h"
#include "signal_state.h"
#include "lexer.h"
#include "parser.h"
#include "unicode.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

// Defined in src/vm/vm.cpp. Holds an extra reference to a shared_future so a
// discarded future doesn't block the caller in its destructor — see the
// comment at the definition for the C++ standard wart this works around.
extern void retainInflightFuture(const std::shared_future<Value>& f);

// Thread-local pointer to the Interpreter that is currently executing a
// native on this thread. Used by concurrency natives (Lock.withLock,
// SharedMap.update) to route user-callback invocations through the
// caller's interpreter — critical for tree-walker async correctness.
thread_local Interpreter* g_currentInterp = nullptr;

// ── Operator overloading helpers ──
// Call a dunder method on an instance if it exists. Returns {true, result} if found.
//
// Fast path: precomputed `hasOperatorOverloads` flag on the class lets us
// skip the chain walk for the common case (no overloads). See PraiaClass
// in interpreter.h for the flag's contract.
static std::pair<bool, Value> callDunder(Interpreter& interp,
                                          const std::shared_ptr<PraiaInstance>& inst,
                                          const std::string& methodName,
                                          const std::vector<Value>& args) {
    if (!inst->klass || !inst->klass->hasOperatorOverloads) return {false, Value()};
    auto* decl = inst->klass->findMethod(methodName);
    if (!decl) return {false, Value()};
    auto bound = std::make_shared<PraiaMethod>();
    bound->methodName = methodName;
    bound->params = decl->params;
    bound->decl = decl;
    bound->instance = inst;
    auto walk = inst->klass;
    while (walk && !walk->methods.count(methodName))
        walk = walk->superclass;
    bound->definingClass = walk ? walk : inst->klass;
    bound->closure = bound->definingClass->closure;
    bound->astOwner = inst->klass->astOwner;
    return {true, bound->call(interp, args)};
}

// Map binary operator token to dunder method name.
// LTE/GTE prefer dedicated __le/__ge; the dispatch falls back to negated
// __gt/__lt when those aren't defined (see Binary handling below).
static std::string binaryDunder(TokenType op) {
    switch (op) {
        case TokenType::PLUS:    return "__add";
        case TokenType::MINUS:   return "__sub";
        case TokenType::STAR:    return "__mul";
        case TokenType::SLASH:   return "__div";
        case TokenType::PERCENT: return "__mod";
        case TokenType::EQ:      return "__eq";
        case TokenType::NEQ:     return "__eq";
        case TokenType::LT:      return "__lt";
        case TokenType::GT:      return "__gt";
        case TokenType::LTE:     return "__le";
        case TokenType::GTE:     return "__ge";
        default: return "";
    }
}

// Check if a value matches a type (reused by `is` operator and `match` type patterns).
static bool checkIs(const Value& subject, const Value& typeVal, int line, int column) {
    if (typeVal.isString()) {
        auto& tn = typeVal.asString();
        if (tn == "nil")       return subject.isNil();
        if (tn == "bool")      return subject.isBool();
        if (tn == "int")       return subject.isInt();
        if (tn == "float")     return subject.isDouble();
        if (tn == "string")    return subject.isString();
        if (tn == "array")     return subject.isArray();
        if (tn == "map")       return subject.isMap();
        if (tn == "set")       return subject.isSet();
        if (tn == "function")  return subject.isCallable();
        if (tn == "instance")  return subject.isInstance();
        if (tn == "tagged")    return subject.isTagged();
        if (tn == "future")    return subject.isFuture();
        if (tn == "generator") return subject.isGenerator();
        throw RuntimeError("Unknown type name '" + tn + "'", line, column);
    }
    if (typeVal.isCallable()) {
        auto klass = std::dynamic_pointer_cast<PraiaClass>(typeVal.asCallable());
        if (!klass) throw RuntimeError("'is' requires a class or type name string", line, column);
        if (!subject.isInstance()) return false;
        auto walk = subject.asInstance()->klass;
        while (walk) {
            if (walk == klass) return true;
            walk = walk->superclass;
        }
        return false;
    }
    throw RuntimeError("'is' requires a type name string or class", line, column);
}

// Reorder named arguments to match parameter positions.
// Returns a vector in parameter order with Value() for unfilled positions
// AND sets *outMask: bit i is 1 iff position i was actually provided. The
// caller is responsible for installing outMask into Interpreter::pendingArgsFilled_
// before invoking the callable so the receiver knows which positions to default.
static std::vector<Value> reorderNamedArgs(
    const std::shared_ptr<Callable>& callable,
    const std::vector<Value>& args,
    const std::vector<std::string>& names,
    int line,
    uint64_t* outMask) {
    const auto* params = callable->paramNames();
    if (!params)
        throw RuntimeError("Named arguments not supported for '" + callable->name() + "'", line);

    int paramCount = static_cast<int>(params->size());
    std::vector<Value> result(paramCount);
    std::vector<bool> filled(paramCount, false);
    int positionalIdx = 0;

    for (size_t i = 0; i < args.size(); i++) {
        if (names[i].empty()) {
            if (positionalIdx >= paramCount)
                throw RuntimeError(callable->name() + "() too many arguments", line);
            result[positionalIdx] = args[i];
            filled[positionalIdx] = true;
            positionalIdx++;
        } else {
            int found = -1;
            for (int p = 0; p < paramCount; p++) {
                if ((*params)[p] == names[i]) { found = p; break; }
            }
            if (found == -1)
                throw RuntimeError(callable->name() + "() unknown parameter '" + names[i] + "'", line);
            if (filled[found])
                throw RuntimeError(callable->name() + "() parameter '" + names[i] + "' specified twice", line);
            result[found] = args[i];
            filled[found] = true;
        }
    }
    if (outMask) {
        uint64_t m = 0;
        int n = std::min(paramCount, 64);
        for (int p = 0; p < n; p++) if (filled[p]) m |= (uint64_t)1 << p;
        *outMask = m;
    }
    return result;
}

// Invoke a Callable with arity check + line-context for errors.
// Native functions throw with line=0 because they don't know the caller;
// we rewrite that to the current call-site line so the user sees a real location.
static Value callWithContext(Interpreter& interp,
                             const std::shared_ptr<Callable>& func,
                             const std::vector<Value>& args,
                             int line) {
    int n = static_cast<int>(args.size());
    int a = func->arity();
    // Native functions require exact arity; user-defined functions allow
    // fewer args (missing params default to nil, like Python/JS).
    bool isNative = dynamic_cast<NativeFunction*>(func.get()) != nullptr;
    if (a != -1) {
        if (isNative && n != a) {
            throw RuntimeError(func->name() + "() expected " + std::to_string(a) +
                               " " + argStr(a) + " but got " + std::to_string(n), line);
        } else if (!isNative && n > a) {
            throw RuntimeError(func->name() + "() expected at most " + std::to_string(a) +
                               " " + argStr(a) + " but got " + std::to_string(n), line);
        }
    }
    if (interp.callStack.size() >= 256)
        throw RuntimeError("Stack overflow (too many nested calls)", line);
    interp.callStack.push_back({func->name(), line});
    try {
        Value result = func->call(interp, args);
        interp.callStack.pop_back(); // pop only on success
        return result;
    } catch (const RuntimeError& err) {
        // Leave frame on stack for trace, but fix line 0
        if (err.line == 0)
            throw RuntimeError(err.what(), line);
        throw;
    } catch (...) {
        // Leave frame on stack for trace
        throw;
    }
}

// ── Grain (module) loading ───────────────────────────────────

std::string Interpreter::resolveGrainPath(const std::string& path, int line) {
    try {
        return ::resolveGrainPath(path, currentFile);
    } catch (const std::runtime_error& e) {
        throw RuntimeError(e.what(), line);
    }
}

Value Interpreter::loadGrain(const std::string& importPath, int line) {
    std::string resolved = resolveGrainPath(importPath, line);

    // Check for duplicate import in the same file
    if (importedInCurrentFile.count(resolved))
        throw RuntimeError("Grain '" + importPath + "' is already imported in this file", line);
    importedInCurrentFile.insert(resolved);

    // Return cached grain if already loaded by another file
    auto cached = grainCache.find(resolved);
    if (cached != grainCache.end()) return cached->second;

    // Read source
    std::ifstream f(resolved);
    if (!f.is_open())
        throw RuntimeError("Cannot read grain: " + resolved, line);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();

    // Lex + parse
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    if (lexer.hasError())
        throw RuntimeError("Syntax error in grain: " + importPath, line);

    Parser parser(tokens);
    auto program = parser.parse();
    if (parser.hasError())
        throw RuntimeError("Parse error in grain: " + importPath, line);

    // Wrap the AST in a shared_ptr so callables created during execution
    // can co-own it (prevents dangling if grainAsts is ever modified).
    auto ast = std::make_shared<std::vector<StmtPtr>>(std::move(program));

    // Execute in isolated scope
    auto grainEnv = gcNew<Environment>(globals);
    auto prevEnv = env;
    auto prevFile = currentFile;
    auto prevImports = importedInCurrentFile;
    auto prevAstOwner = currentAstOwner_;
    env = grainEnv;
    currentFile = resolved;
    importedInCurrentFile.clear();
    currentAstOwner_ = ast;

    Value exports;
    try {
        for (const auto& stmt : *ast)
            execute(stmt.get());
        // If no export statement was hit, export nothing
        exports = Value(gcNew<PraiaMap>());
    } catch (const ExportSignal& es) {
        exports = Value(es.exports);
    } catch (...) {
        env = prevEnv;
        currentFile = prevFile;
        importedInCurrentFile = prevImports;
        currentAstOwner_ = prevAstOwner;
        throw;
    }
    env = prevEnv;
    currentFile = prevFile;
    importedInCurrentFile = prevImports;
    currentAstOwner_ = prevAstOwner;

    // Keep the AST alive and cache the result
    grainAsts.push_back(std::move(ast));
    grainCache[resolved] = exports;
    return exports;
}

std::string Interpreter::formatStackTrace() const {
    if (callStack.empty()) return "";
    std::string trace;
    for (int i = static_cast<int>(callStack.size()) - 1; i >= 0; --i) {
        auto& f = callStack[i];
        trace += "  at " + f.name + "()";
        if (f.line > 0) trace += " line " + std::to_string(f.line);
        trace += "\n";
    }
    return trace;
}

bool Interpreter::interpret(const std::vector<StmtPtr>& program) {
    GcHeap::current().setRootMarker([this](GcHeap& h) { gcMarkRoots(h); });
    try {
        for (const auto& stmt : program) {
            execute(stmt.get());
            GcHeap::current().collectIfNeeded();
        }
        return true;
    } catch (const ThrowSignal& t) {
        std::cerr << formatLocation(t.line, t.column) << " Uncaught error: "
                  << t.value.toString() << std::endl;
        std::cerr << formatStackTrace();
        callStack.clear();
        return false;
    } catch (const RuntimeError& e) {
        std::cerr << formatLocation(e.line, e.column) << " Runtime error: " << e.what() << std::endl;
        std::cerr << formatStackTrace();
        callStack.clear();
        return false;
    }
}

void Interpreter::interpretRepl(const std::vector<StmtPtr>& program) {
    GcHeap::current().setRootMarker([this](GcHeap& h) { gcMarkRoots(h); });
    try {
        for (const auto& stmt : program) {
            if (stmt->type == StmtType::Expr) {
                auto* es = static_cast<const ExprStmt*>(stmt.get());
                Value val = evaluate(es->expr.get());
                if (!val.isNil())
                    std::cout << val.toString() << "\n";
            } else {
                execute(stmt.get());
            }
            GcHeap::current().collectIfNeeded();
        }
    } catch (const ThrowSignal& t) {
        std::cerr << formatLocation(t.line, t.column) << " Uncaught error: "
                  << t.value.toString() << std::endl;
        std::cerr << formatStackTrace();
        callStack.clear();
    } catch (const RuntimeError& e) {
        std::cerr << formatLocation(e.line, e.column) << " Runtime error: " << e.what() << std::endl;
        std::cerr << formatStackTrace();
        callStack.clear();
    }
}

void Interpreter::executeBlock(const BlockStmt* block,
                                std::shared_ptr<Environment> newEnv) {
    savedEnvStack_.push_back(env);
    env = newEnv;
    try {
        for (const auto& stmt : block->statements)
            execute(stmt.get());
    } catch (...) {
        env = savedEnvStack_.back();
        savedEnvStack_.pop_back();
        throw;
    }
    env = savedEnvStack_.back();
    savedEnvStack_.pop_back();
}

// ── SIGINT check — called from execute() and tight loops ──

void Interpreter::enqueuePosted(std::shared_ptr<Callable> fn,
                                std::vector<Value> args) {
    std::lock_guard<std::mutex> lock(postedMutex_);
    postedQueue_.push_back({std::move(fn), std::move(args)});
    // Release so the engine thread's acquire-load in drainPosted sees
    // the push before the flag flip; together they form the
    // happens-before edge that lets the engine skip the mutex when
    // the queue is empty.
    postedPending_.store(true, std::memory_order_release);
}

void Interpreter::drainPosted() {
    if (!postedPending_.load(std::memory_order_acquire)) return;
    std::vector<PostedCall> work;
    {
        std::lock_guard<std::mutex> lock(postedMutex_);
        work.swap(postedQueue_);
        postedPending_.store(false, std::memory_order_relaxed);
    }
    for (auto& pc : work) {
        // Posted calls are fire-and-forget — no user-code call site
        // to surface the exception to. Log and continue draining;
        // one bad callback shouldn't poison the queue or propagate
        // up through checkInterrupt into unrelated user code.
        try {
            callSafe(*this, pc.fn, pc.args);
        } catch (const RuntimeError& e) {
            std::fprintf(stderr,
                "[praia::postToEngine] callback raised: %s\n", e.what());
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[praia::postToEngine] callback raised non-Praia exception: %s\n",
                e.what());
        } catch (...) {
            // Raw throw of a non-exception value. Rare in practice
            // (Praia code only throws RuntimeError) but possible
            // through a misbehaving plugin native.
            std::fprintf(stderr,
                "[praia::postToEngine] callback raised an unknown exception\n");
        }
    }
}

void Interpreter::checkInterrupt(int line, int column) {
    drainPosted();
    if (g_pendingSignals.load(std::memory_order_relaxed) & (1u << SIGINT)) {
        g_pendingSignals.fetch_and(~(1u << SIGINT));
        std::shared_ptr<Callable> handler;
        {
            std::lock_guard<std::mutex> lock(g_signalMutex);
            auto it = g_signalHandlers.find(SIGINT);
            if (it != g_signalHandlers.end()) handler = it->second;
        }
        if (handler) {
            handler->call(*this, {Value("SIGINT")});
        } else {
            throw ThrowSignal{Value("Interrupted"), line, column};
        }
    }
}

// ── Statement execution ──────────────────────────────────────

void Interpreter::execute(const Stmt* stmt) {
    checkInterrupt(stmt->line, stmt->column);

    switch (stmt->type) {
    case StmtType::Expr: {
        auto* s = static_cast<const ExprStmt*>(stmt);
        evaluate(s->expr.get());
        break;
    }
    case StmtType::Let: {
        auto* s = static_cast<const LetStmt*>(stmt);
        if (!s->pattern.empty()) {
            // Destructuring
            Value val = evaluate(s->initializer.get());
            if (s->isArrayPattern) {
                if (!val.isArray())
                    throw RuntimeError("Cannot destructure non-array value", s->line, s->column);
                auto& elems = val.asArray()->elements;
                for (size_t i = 0; i < s->pattern.size(); i++) {
                    auto& p = s->pattern[i];
                    if (p.isRest) {
                        auto rest = gcNew<PraiaArray>();
                        for (size_t j = i; j < elems.size(); j++)
                            rest->elements.push_back(elems[j]);
                        env->define(p.name, Value(rest));
                        break;
                    }
                    env->define(p.name, i < elems.size() ? elems[i] : Value());
                }
            } else {
                // Map destructuring
                if (!val.isMap())
                    throw RuntimeError("Cannot destructure non-map value", s->line, s->column);
                auto& entries = val.asMap()->entries;
                std::set<std::string> extracted;
                for (auto& p : s->pattern) {
                    if (p.isRest) {
                        auto rest = gcNew<PraiaMap>();
                        for (auto& [k, v] : entries) {
                            if (!k.isString() || !extracted.count(k.asString()))
                                rest->entries[k] = v;
                        }
                        env->define(p.name, Value(rest));
                        break;
                    }
                    std::string key = p.key.empty() ? p.name : p.key;
                    extracted.insert(key);
                    auto it = entries.find(Value(key));
                    env->define(p.name, it != entries.end() ? it->second : Value());
                }
            }
        } else {
            // Simple let
            Value val;
            if (s->initializer) val = evaluate(s->initializer.get());
            env->define(s->name, std::move(val));
        }
        break;
    }
    case StmtType::Block: {
        auto* s = static_cast<const BlockStmt*>(stmt);
        executeBlock(s, gcNew<Environment>(env));
        break;
    }
    case StmtType::If: {
        auto* s = static_cast<const IfStmt*>(stmt);
        if (evaluate(s->condition.get()).isTruthy()) {
            execute(s->thenBranch.get());
        } else {
            bool handled = false;
            for (const auto& elif : s->elifBranches) {
                if (evaluate(elif.condition.get()).isTruthy()) {
                    execute(elif.body.get());
                    handled = true;
                    break;
                }
            }
            if (!handled && s->elseBranch)
                execute(s->elseBranch.get());
        }
        break;
    }
    case StmtType::Match: {
        auto* s = static_cast<const MatchStmt*>(stmt);
        Value subject = evaluate(s->subject.get());
        for (auto& c : s->cases) {
            bool matched = false;
            if (!c.pattern && !c.isType && !c.guard) {
                // Default case
                matched = true;
            } else if (c.isType) {
                // Type pattern: is "typename" or is ClassName
                Value typeVal = evaluate(c.isType.get());
                matched = checkIs(subject, typeVal, s->line, s->column);
            } else if (c.guard) {
                // Guard clause: when condition
                matched = evaluate(c.guard.get()).isTruthy();
            } else if (c.pattern && c.pattern->type == ExprType::Call) {
                auto* call = static_cast<const CallExpr*>(c.pattern.get());
                bool isTagPattern = false;
                if (call->callee->type == ExprType::Identifier) {
                    auto* id = static_cast<const IdentifierExpr*>(call->callee.get());
                    if (!id->name.empty() && std::isupper(id->name[0]))
                        isTagPattern = true;
                }
                if (isTagPattern && subject.isTagged()) {
                    auto* id = static_cast<const IdentifierExpr*>(call->callee.get());
                    auto tag = subject.asTagged();
                    if (tag->tag == id->name && call->args.size() == tag->values.size()) {
                        // Check if all args are identifiers (binding pattern) vs values
                        bool allIdents = true;
                        for (auto& arg : call->args)
                            if (arg->type != ExprType::Identifier) { allIdents = false; break; }

                        if (allIdents) {
                            // Binding pattern: Ok(val), Point(x, y)
                            matched = true;
                            auto matchEnv = gcNew<Environment>(env);
                            auto prevEnv = env;
                            env = matchEnv;
                            for (size_t i = 0; i < call->args.size(); i++) {
                                auto* argId = static_cast<const IdentifierExpr*>(call->args[i].get());
                                matchEnv->define(argId->name, tag->values[i]);
                            }
                            // Restore env on throw so the outer scope isn't
                            // left pointing at matchEnv. (Outer executeBlock
                            // calls also restore via savedEnvStack_, but
                            // relying on that coupling is fragile.)
                            try {
                                execute(c.body.get());
                            } catch (...) {
                                env = prevEnv;
                                throw;
                            }
                            env = prevEnv;
                            break;
                        } else {
                            // Value pattern: Ok(42) — construct tagged and compare
                            Value pattern = evaluate(c.pattern.get());
                            matched = (subject == pattern);
                        }
                    }
                    // else: tag mismatch or arity mismatch → matched stays false
                } else {
                    // Not a tagged match (either lowercase or subject isn't tagged)
                    // Evaluate pattern and compare, supporting __eq for instances
                    Value pattern = evaluate(c.pattern.get());
                    if (subject.isInstance()) {
                        auto [found, result] = callDunder(*this, subject.asInstance(), "__eq", {pattern});
                        matched = found ? result.isTruthy() : (subject == pattern);
                    } else {
                        matched = (subject == pattern);
                    }
                }
            } else {
                // Equality pattern
                Value pattern = evaluate(c.pattern.get());
                if (subject.isInstance()) {
                    auto [found, result] = callDunder(*this, subject.asInstance(), "__eq", {pattern});
                    matched = found ? result.isTruthy() : (subject == pattern);
                } else {
                    matched = (subject == pattern);
                }
            }
            if (matched) {
                execute(c.body.get());
                break;
            }
        }
        break;
    }
    case StmtType::While: {
        auto* s = static_cast<const WhileStmt*>(stmt);
        try {
            while (evaluate(s->condition.get()).isTruthy()) {
                try { execute(s->body.get()); }
                catch (const ContinueSignal&) { /* skip to next iteration */ }
            }
        } catch (const BreakSignal&) { /* exit loop */ }
        break;
    }
    case StmtType::For: {
        auto* s = static_cast<const ForStmt*>(stmt);
        Value startVal = evaluate(s->start.get());
        Value endVal   = evaluate(s->end.get());
        if (!startVal.isNumber() || !endVal.isNumber())
            throw RuntimeError("Range bounds must be numbers", s->line, s->column);

        int64_t from = static_cast<int64_t>(startVal.asNumber());
        int64_t to   = static_cast<int64_t>(endVal.asNumber());
        auto* bodyBlock = static_cast<const BlockStmt*>(s->body.get());

        try {
            for (int64_t i = from; i < to; i++) {
                checkInterrupt(s->line, s->column);
                auto iterEnv = gcNew<Environment>(env);
                iterEnv->define(s->varName, Value(i));
                try { executeBlock(bodyBlock, iterEnv); }
                catch (const ContinueSignal&) { /* skip to next iteration */ }
            }
        } catch (const BreakSignal&) { /* exit loop */ }
        break;
    }
    case StmtType::ForIn: {
        auto* s = static_cast<const ForInStmt*>(stmt);
        Value iterable = evaluate(s->iterable.get());
        auto* bodyBlock = static_cast<const BlockStmt*>(s->body.get());
        bool hasDestructure = !s->destructureKeys.empty();

        // Helper: define loop variables in the iteration env
        auto defineLoopVar = [&](std::shared_ptr<Environment> iterEnv, const Value& elem) {
            if (hasDestructure && elem.isMap()) {
                auto& entries = elem.asMap()->entries;
                for (auto& dk : s->destructureKeys) {
                    auto it = entries.find(Value(dk));
                    iterEnv->define(dk, it != entries.end() ? it->second : Value());
                }
            } else {
                iterEnv->define(s->varName, elem);
            }
        };

        if (iterable.isArray()) {
            // Snapshot elements to protect against mutation during iteration
            auto snapshot = iterable.asArray()->elements;
            try {
                for (const auto& elem : snapshot) {
                    auto iterEnv = gcNew<Environment>(env);
                    defineLoopVar(iterEnv, elem);
                    try { executeBlock(bodyBlock, iterEnv); }
                    catch (const ContinueSignal&) {}
                }
            } catch (const BreakSignal&) {}
        } else if (iterable.isMap()) {
            // Snapshot entries to protect against mutation during iteration
            std::vector<std::pair<Value, Value>> snapshot(
                iterable.asMap()->entries.begin(), iterable.asMap()->entries.end());
            try {
                for (auto& [k, v] : snapshot) {
                    auto entry = gcNew<PraiaMap>();
                    entry->entries[Value("key")] = k;
                    entry->entries[Value("value")] = v;
                    auto iterEnv = gcNew<Environment>(env);
                    defineLoopVar(iterEnv, Value(entry));
                    try { executeBlock(bodyBlock, iterEnv); }
                    catch (const ContinueSignal&) {}
                }
            } catch (const BreakSignal&) {}
        } else if (iterable.isSet()) {
            // Snapshot elements to protect against mutation during
            // iteration (set elements are immutable values; the
            // snapshot is just defensive against .add/.remove inside
            // the loop body). Order is unspecified — same as map.
            std::vector<Value> snapshot(
                iterable.asSet()->elements.begin(),
                iterable.asSet()->elements.end());
            try {
                for (const auto& elem : snapshot) {
                    auto iterEnv = gcNew<Environment>(env);
                    defineLoopVar(iterEnv, elem);
                    try { executeBlock(bodyBlock, iterEnv); }
                    catch (const ContinueSignal&) {}
                }
            } catch (const BreakSignal&) {}
        } else if (iterable.isString()) {
            try {
#ifdef HAVE_UTF8PROC
                for (auto& g : utf8_graphemes(iterable.asString())) {
                    auto iterEnv = gcNew<Environment>(env);
                    defineLoopVar(iterEnv, Value(g));
                    try { executeBlock(bodyBlock, iterEnv); }
                    catch (const ContinueSignal&) {}
                }
#else
                for (size_t i = 0; i < iterable.asString().size(); i++) {
                    auto iterEnv = gcNew<Environment>(env);
                    defineLoopVar(iterEnv, Value(std::string(1, iterable.asString()[i])));
                    try { executeBlock(bodyBlock, iterEnv); }
                    catch (const ContinueSignal&) {}
                }
#endif
            } catch (const BreakSignal&) {}
        } else if (iterable.isGenerator()) {
            auto gen = iterable.asGenerator();
            try {
                while (true) {
                    if (gen->state == PraiaGenerator::State::COMPLETED) break;
                    gen->sendValue = Value();
                    gen->fiber->resume();

                    if (!gen->errorMessage.empty()) {
                        std::string msg = gen->errorMessage;
                        gen->releaseAfterCompletion();
                        throw RuntimeError(msg, s->line, s->column);
                    }
                    if (gen->state == PraiaGenerator::State::COMPLETED) {
                        gen->releaseAfterCompletion();
                    }
                    if (gen->done) break;

                    auto iterEnv = gcNew<Environment>(env);
                    defineLoopVar(iterEnv, gen->lastYielded);
                    try { executeBlock(bodyBlock, iterEnv); }
                    catch (const ContinueSignal&) {}
                }
            } catch (const BreakSignal&) {}
        } else {
            throw RuntimeError("for-in requires an array, map, set, string, or generator", s->line, s->column);
        }
        break;
    }
    case StmtType::Func: {
        auto* s = static_cast<const FuncStmt*>(stmt);
        if (s->isGenerator) {
            auto func = std::make_shared<PraiaGeneratorFunction>();
            func->funcName = s->name;
            func->params = s->params;
            func->defaults = &s->defaults;
            func->restParam = s->restParam;
            func->body = static_cast<const BlockStmt*>(s->body.get());
            func->closure = env;
            func->astOwner = currentAstOwner_;
            env->define(s->name, Value(std::static_pointer_cast<Callable>(func)));
        } else {
            auto func = std::make_shared<PraiaFunction>();
            func->funcName = s->name;
            func->params = s->params;
            func->defaults = &s->defaults;
            func->restParam = s->restParam;
            func->body = static_cast<const BlockStmt*>(s->body.get());
            func->closure = env;
            func->astOwner = currentAstOwner_;
            env->define(s->name, Value(std::static_pointer_cast<Callable>(func)));
        }
        break;
    }
    case StmtType::Enum: {
        auto* s = static_cast<const EnumStmt*>(stmt);
        auto enumMap = gcNew<PraiaMap>();
        int64_t nextVal = 0;
        for (size_t i = 0; i < s->members.size(); i++) {
            if (s->values[i]) {
                Value v = evaluate(s->values[i].get());
                if (!v.isNumber())
                    throw RuntimeError("Enum value must be a number", s->line, s->column);
                nextVal = v.isInt() ? v.asInt() : static_cast<int64_t>(v.asNumber());
            }
            enumMap->entries[Value(s->members[i])] = Value(nextVal);
            nextVal++;
        }
        env->define(s->name, Value(enumMap));
        break;
    }
    case StmtType::Class: {
        auto* s = static_cast<const ClassStmt*>(stmt);
        std::shared_ptr<PraiaClass> superclass;
        if (!s->superclass.empty()) {
            Value superVal = env->get(s->superclass, s->line);
            if (!superVal.isCallable())
                throw RuntimeError("Superclass must be a class", s->line, s->column);
            superclass = std::dynamic_pointer_cast<PraiaClass>(superVal.asCallable());
            if (!superclass)
                throw RuntimeError("'" + s->superclass + "' is not a class", s->line, s->column);
        }

        auto klass = gcNew<PraiaClass>();
        klass->className = s->name;
        klass->superclass = superclass;
        klass->closure = env;
        klass->astOwner = currentAstOwner_;
        // Inherit the dunder flag from the superclass — if any ancestor
        // declared overloads, the dispatch must walk the chain.
        if (superclass && superclass->hasOperatorOverloads)
            klass->hasOperatorOverloads = true;
        for (auto& m : s->methods) {
            if (m.isStatic) {
                klass->staticMethods[m.name] = &m;
            } else {
                klass->methods[m.name] = &m;
                // Track dunder presence for the operator-dispatch fast path.
                if (m.name.size() >= 2 && m.name[0] == '_' && m.name[1] == '_')
                    klass->hasOperatorOverloads = true;
            }
        }
        env->define(s->name, Value(std::static_pointer_cast<Callable>(klass)));
        break;
    }
    case StmtType::Return: {
        auto* s = static_cast<const ReturnStmt*>(stmt);
        Value val;
        if (s->value) val = evaluate(s->value.get());
        throw ReturnSignal{std::move(val)};
    }
    case StmtType::Break: {
        throw BreakSignal{};
    }
    case StmtType::Continue: {
        throw ContinueSignal{};
    }
    case StmtType::Throw: {
        auto* s = static_cast<const ThrowStmt*>(stmt);
        Value val = evaluate(s->value.get());
        throw ThrowSignal{std::move(val), s->line};
    }
    case StmtType::TryCatch: {
        auto* s = static_cast<const TryCatchStmt*>(stmt);
        size_t savedStackSize = callStack.size();
        std::exception_ptr pendingException;
        try {
            execute(s->tryBody.get());
        } catch (const ThrowSignal& ts) {
            callStack.resize(savedStackSize);
            auto catchEnv = gcNew<Environment>(env);
            catchEnv->define(s->errorVar, ts.value);
            try {
                executeBlock(static_cast<const BlockStmt*>(s->catchBody.get()), catchEnv);
            } catch (...) {
                pendingException = std::current_exception();
            }
        } catch (const RuntimeError& re) {
            callStack.resize(savedStackSize);
            auto catchEnv = gcNew<Environment>(env);
            catchEnv->define(s->errorVar, Value(std::string(re.what())));
            try {
                executeBlock(static_cast<const BlockStmt*>(s->catchBody.get()), catchEnv);
            } catch (...) {
                pendingException = std::current_exception();
            }
        } catch (...) {
            pendingException = std::current_exception();
        }
        if (s->finallyBody) execute(s->finallyBody.get());
        if (pendingException) std::rethrow_exception(pendingException);
        break;
    }
    case StmtType::Ensure: {
        auto* s = static_cast<const EnsureStmt*>(stmt);
        Value cond = evaluate(s->condition.get());
        if (!cond.isTruthy())
            execute(s->elseBody.get());
        break;
    }
    case StmtType::Defer: {
        auto* s = static_cast<const DeferStmt*>(stmt);
        if (!deferStacks_.empty())
            deferStacks_.back().push_back(s->expr.get());
        break;
    }
    case StmtType::Use: {
        auto* s = static_cast<const UseStmt*>(stmt);
        Value grain = loadGrain(s->path, s->line);
        env->define(s->alias, grain);
        break;
    }
    case StmtType::Export: {
        auto* s = static_cast<const ExportStmt*>(stmt);
        auto exports = gcNew<PraiaMap>();
        for (auto& name : s->names) {
            exports->entries[Value(name)] = env->get(name, s->line);
        }
        throw ExportSignal{exports};
    }
    default:
        break;
    }
}

// ── Expression evaluation ────────────────────────────────────

Value Interpreter::evaluate(const Expr* expr) {
    checkInterrupt(expr->line, expr->column);

    switch (expr->type) {
    // ── Literals ──

    case ExprType::Number: {
        auto* e = static_cast<const NumberExpr*>(expr);
        return e->isInt ? Value(e->intValue) : Value(e->floatValue);
    }
    case ExprType::String: {
        auto* e = static_cast<const StringExpr*>(expr);
        return Value(e->value);
    }
    case ExprType::Bool: {
        auto* e = static_cast<const BoolExpr*>(expr);
        return Value(e->value);
    }
    case ExprType::Nil: {
        return Value();
    }

    // ── Variables ──

    case ExprType::Identifier: {
        auto* e = static_cast<const IdentifierExpr*>(expr);
        return env->get(e->name, e->line);
    }
    case ExprType::This: {
        auto* e = static_cast<const ThisExpr*>(expr);
        return env->get("this", e->line);
    }
    case ExprType::Super: {
        auto* e = static_cast<const SuperExpr*>(expr);
        // Get the instance
        Value thisVal = env->get("this", e->line);
        if (!thisVal.isInstance())
            throw RuntimeError("'super' used outside of a method", e->line, e->column);
        auto instance = thisVal.asInstance();

        // Get the superclass from the defining class (not the instance's class)
        Value superVal = env->get("__super__", e->line);
        if (!superVal.isCallable())
            throw RuntimeError("'super' used in a class with no superclass", e->line, e->column);
        auto super = std::dynamic_pointer_cast<PraiaClass>(superVal.asCallable());
        if (!super)
            throw RuntimeError("Class has no superclass", e->line, e->column);

        // Look up the method on the superclass
        auto* methodDecl = super->findMethod(e->method);
        if (!methodDecl)
            throw RuntimeError("Superclass has no method '" + e->method + "'", e->line, e->column);

        // Bind it to the current instance, with the super's class as defining class
        auto bound = std::make_shared<PraiaMethod>();
        bound->methodName = e->method;
        bound->params = methodDecl->params;
        bound->decl = methodDecl;
        bound->closure = super->closure;
        bound->instance = instance;
        // Find which class actually defines this method (for correct super chaining)
        bound->definingClass = super;
        auto* check = super->methods.count(e->method) ? super.get() : nullptr;
        if (!check && super->superclass) {
            // The method is inherited further up — find the actual defining class
            auto walk = super;
            while (walk && !walk->methods.count(e->method))
                walk = walk->superclass;
            if (walk) bound->definingClass = walk;
        }
        bound->astOwner = super->astOwner;
        return Value(std::static_pointer_cast<Callable>(bound));
    }
    case ExprType::Assign: {
        auto* e = static_cast<const AssignExpr*>(expr);
        Value val = evaluate(e->value.get());
        env->set(e->name, val, e->line);
        return val;
    }

    // ── Unary ──

    case ExprType::Unary: {
        auto* e = static_cast<const UnaryExpr*>(expr);
        Value operand = evaluate(e->operand.get());
        if (e->op == TokenType::MINUS) {
            if (operand.isInstance()) {
                auto [found, result] = callDunder(*this, operand.asInstance(), "__neg", {});
                if (found) return result;
            }
            if (!operand.isNumber())
                throw RuntimeError("Operand of '-' must be a number", e->line, e->column);
            if (operand.isInt()) {
                int64_t v = operand.asInt();
                if (v == INT64_MIN) return Value(-static_cast<double>(v));
                return Value(-v);
            }
            return Value(-operand.asNumber());
        }
        if (e->op == TokenType::NOT)
            return Value(!operand.isTruthy());
        if (e->op == TokenType::BIT_NOT) {
            if (!operand.isNumber())
                throw RuntimeError("Operand of '~' must be a number", e->line, e->column);
            return Value(~static_cast<int64_t>(operand.asNumber()));
        }
        throw RuntimeError("Unknown unary operator", e->line, e->column);
    }

    // ── Postfix (i++, i--) ──

    case ExprType::Postfix: {
        auto* e = static_cast<const PostfixExpr*>(expr);
        if (e->operand->type != ExprType::Identifier)
            throw RuntimeError("Postfix operator requires a variable", e->line, e->column);
        auto* ident = static_cast<const IdentifierExpr*>(e->operand.get());

        Value cur = env->get(ident->name, e->line);
        if (!cur.isNumber())
            throw RuntimeError("Postfix operator requires a number", e->line, e->column);

        if (cur.isInt()) {
            int64_t old = cur.asInt();
            int64_t next;
            bool overflow = (e->op == TokenType::INCREMENT)
                ? __builtin_add_overflow(old, (int64_t)1, &next)
                : __builtin_sub_overflow(old, (int64_t)1, &next);
            if (overflow)
                env->set(ident->name, Value(static_cast<double>(old) + (e->op == TokenType::INCREMENT ? 1 : -1)), e->line);
            else
                env->set(ident->name, Value(next), e->line);
            return Value(old);
        }
        double old = cur.asNumber();
        double next = (e->op == TokenType::INCREMENT) ? old + 1 : old - 1;
        env->set(ident->name, Value(next), e->line);
        return Value(old);
    }

    // ── Binary ──

    case ExprType::Binary: {
        auto* e = static_cast<const BinaryExpr*>(expr);
        if (e->op == TokenType::NIL_COALESCE) {
            Value left = evaluate(e->left.get());
            return left.isNil() ? evaluate(e->right.get()) : left;
        }
        if (e->op == TokenType::OR) {
            Value left = evaluate(e->left.get());
            return left.isTruthy() ? left : evaluate(e->right.get());
        }
        if (e->op == TokenType::AND) {
            Value left = evaluate(e->left.get());
            return !left.isTruthy() ? left : evaluate(e->right.get());
        }

        Value left  = evaluate(e->left.get());
        Value right = evaluate(e->right.get());

        // Operator overloading: check for dunder methods on instances
        if (left.isInstance()) {
            std::string dunder = binaryDunder(e->op);
            if (!dunder.empty()) {
                auto [found, result] = callDunder(*this, left.asInstance(), dunder, {right});
                if (found) {
                    // NEQ negates the result of __eq
                    if (e->op == TokenType::NEQ)
                        return Value(!result.isTruthy());
                    return result;
                }
                // Fallback: __le/__ge default to !__gt / !__lt when the
                // dedicated dunder isn't defined.
                if (e->op == TokenType::LTE) {
                    auto [ok2, r2] = callDunder(*this, left.asInstance(), "__gt", {right});
                    if (ok2) return Value(!r2.isTruthy());
                } else if (e->op == TokenType::GTE) {
                    auto [ok2, r2] = callDunder(*this, left.asInstance(), "__lt", {right});
                    if (ok2) return Value(!r2.isTruthy());
                }
            }
        }

        switch (e->op) {
        case TokenType::PLUS:
            if (left.isInt() && right.isInt()) {
                int64_t result;
                if (!__builtin_add_overflow(left.asInt(), right.asInt(), &result))
                    return Value(result);
                return Value(left.asNumber() + right.asNumber()); // overflow → promote to double
            }
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() + right.asNumber());
            if (left.isArray() && right.isArray()) {
                auto result = gcNew<PraiaArray>();
                for (auto& el : left.asArray()->elements) result->elements.push_back(el);
                for (auto& el : right.asArray()->elements) result->elements.push_back(el);
                return Value(result);
            }
            if (left.isMap() && right.isMap()) {
                auto result = gcNew<PraiaMap>();
                for (auto& [k, v] : left.asMap()->entries) result->entries[k] = v;
                for (auto& [k, v] : right.asMap()->entries) result->entries[k] = v;
                return Value(result);
            }
            // Set + set = union (parallels array concat and map merge).
            if (left.isSet() && right.isSet()) {
                auto result = gcNew<PraiaSet>();
                result->elements = left.asSet()->elements;
                for (auto& e : right.asSet()->elements) result->elements.insert(e);
                return Value(result);
            }
            if (left.isString() || right.isString())
                return Value(left.toString() + right.toString());
            throw RuntimeError("Operands of '+' must be numbers, strings, arrays, maps, or sets", e->line, e->column);
        case TokenType::MINUS:
            if (left.isInt() && right.isInt()) {
                int64_t result;
                if (!__builtin_sub_overflow(left.asInt(), right.asInt(), &result))
                    return Value(result);
                return Value(left.asNumber() - right.asNumber());
            }
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() - right.asNumber());
            throw RuntimeError("Operands of '-' must be numbers", e->line, e->column);
        case TokenType::STAR:
            if (left.isInt() && right.isInt()) {
                int64_t result;
                if (!__builtin_mul_overflow(left.asInt(), right.asInt(), &result))
                    return Value(result);
                return Value(left.asNumber() * right.asNumber());
            }
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() * right.asNumber());
            if (left.isString() && right.isInt()) {
                int64_t n = right.asInt();
                if (n < 0) throw RuntimeError("String repeat count cannot be negative", e->line, e->column);
                std::string result;
                result.reserve(left.asString().size() * n);
                for (int64_t i = 0; i < n; i++) result += left.asString();
                return Value(std::move(result));
            }
            if (left.isInt() && right.isString()) {
                int64_t n = left.asInt();
                if (n < 0) throw RuntimeError("String repeat count cannot be negative", e->line, e->column);
                std::string result;
                result.reserve(right.asString().size() * n);
                for (int64_t i = 0; i < n; i++) result += right.asString();
                return Value(std::move(result));
            }
            throw RuntimeError("Operands of '*' must be numbers, or string * int", e->line, e->column);
        case TokenType::SLASH:
            if (left.isNumber() && right.isNumber()) {
                if (right.asNumber() == 0)
                    throw RuntimeError("Division by zero", e->line, e->column);
                if (left.isInt() && right.isInt()) {
                    int64_t a = left.asInt(), b = right.asInt();
                    if (a % b == 0) return Value(a / b);
                }
                return Value(left.asNumber() / right.asNumber());
            }
            throw RuntimeError("Operands of '/' must be numbers", e->line, e->column);
        case TokenType::PERCENT:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0)
                    throw RuntimeError("Modulo by zero", e->line, e->column);
                return Value(left.asInt() % right.asInt());
            }
            if (left.isNumber() && right.isNumber()) {
                if (right.asNumber() == 0)
                    throw RuntimeError("Modulo by zero", e->line, e->column);
                return Value(std::fmod(left.asNumber(), right.asNumber()));
            }
            throw RuntimeError("Operands of '%' must be numbers", e->line, e->column);
        case TokenType::LT:
            if (left.isNumber() && right.isNumber())
                return Value(numbersLess(left, right));
            if (left.isString() && right.isString())
                return Value(left.asString() < right.asString());
            throw RuntimeError("Operands of '<' must be numbers or strings", e->line, e->column);
        case TokenType::GT:
            if (left.isNumber() && right.isNumber())
                return Value(numbersLess(right, left));
            if (left.isString() && right.isString())
                return Value(left.asString() > right.asString());
            throw RuntimeError("Operands of '>' must be numbers or strings", e->line, e->column);
        case TokenType::LTE:
            if (left.isNumber() && right.isNumber())
                return Value(!numbersLess(right, left));
            if (left.isString() && right.isString())
                return Value(left.asString() <= right.asString());
            throw RuntimeError("Operands of '<=' must be numbers or strings", e->line, e->column);
        case TokenType::GTE:
            if (left.isNumber() && right.isNumber())
                return Value(!numbersLess(left, right));
            if (left.isString() && right.isString())
                return Value(left.asString() >= right.asString());
            throw RuntimeError("Operands of '>=' must be numbers or strings", e->line, e->column);
        case TokenType::EQ:  return Value(left == right);
        case TokenType::NEQ: return Value(left != right);
        case TokenType::IS:
            return Value(checkIs(left, right, e->line, e->column));

        // For bitwise ops, take int operands directly. For doubles, fall
        // back to the double→int64 cast (lossy for |n| > 2^53 but the
        // user explicitly opted into a double operand). Avoids the
        // previous always-via-asNumber round-trip that truncated big ints.
        case TokenType::BIT_AND:
            if (left.isNumber() && right.isNumber())
                return Value(left.toInt64ForBitwise() & right.toInt64ForBitwise());
            throw RuntimeError("Operands of '&' must be numbers", e->line, e->column);
        case TokenType::BIT_OR:
            if (left.isNumber() && right.isNumber())
                return Value(left.toInt64ForBitwise() | right.toInt64ForBitwise());
            throw RuntimeError("Operands of '|' must be numbers", e->line, e->column);
        case TokenType::BIT_XOR:
            if (left.isNumber() && right.isNumber())
                return Value(left.toInt64ForBitwise() ^ right.toInt64ForBitwise());
            throw RuntimeError("Operands of '^' must be numbers", e->line, e->column);
        case TokenType::SHL:
            if (left.isNumber() && right.isNumber())
                return Value(left.toInt64ForBitwise() << right.toInt64ForBitwise());
            throw RuntimeError("Operands of '<<' must be numbers", e->line, e->column);
        case TokenType::SHR:
            if (left.isNumber() && right.isNumber())
                return Value(left.toInt64ForBitwise() >> right.toInt64ForBitwise());
            throw RuntimeError("Operands of '>>' must be numbers", e->line, e->column);

        default:
            throw RuntimeError("Unknown binary operator", e->line, e->column);
        }
    }

    // ── Function call ──

    case ExprType::Call: {
        auto* e = static_cast<const CallExpr*>(expr);

        // Tagged value construction: capitalized identifier that doesn't resolve to a callable
        if (e->callee->type == ExprType::Identifier) {
            auto* id = static_cast<const IdentifierExpr*>(e->callee.get());
            if (!id->name.empty() && std::isupper(id->name[0])) {
                // Try to resolve — only catch "Undefined variable", not other errors
                bool resolved = false;
                Value callee;
                try {
                    callee = evaluate(e->callee.get());
                    resolved = true;
                } catch (const RuntimeError& err) {
                    if (std::string(err.what()).find("Undefined variable") == std::string::npos)
                        throw; // re-throw non-resolution errors
                }
                if (!resolved || !callee.isCallable()) {
                    auto tagged = gcNew<PraiaTagged>();
                    tagged->tag = id->name;
                    for (const auto& arg : e->args)
                        tagged->values.push_back(evaluate(arg.get()));
                    return Value(tagged);
                }
                // Callable — fall through to normal call path
            }
        }

        Value callee = evaluate(e->callee.get());
        if (!callee.isCallable())
            throw RuntimeError("Can only call functions", e->line, e->column);

        std::vector<Value> args;
        for (const auto& arg : e->args) {
            if (arg->type == ExprType::Spread) {
                auto* spread = static_cast<const SpreadExpr*>(arg.get());
                Value val = evaluate(spread->expr.get());
                if (!val.isArray())
                    throw RuntimeError("Spread argument must be an array", spread->line);
                for (auto& item : val.asArray()->elements)
                    args.push_back(item);
            } else {
                args.push_back(evaluate(arg.get()));
            }
        }

        // Reorder named arguments if present
        bool hasNamed = false;
        for (auto& n : e->argNames) { if (!n.empty()) { hasNamed = true; break; } }
        if (hasNamed) {
            uint64_t mask = 0;
            args = reorderNamedArgs(callee.asCallable(), args, e->argNames, e->line, &mask);
            pendingArgsFilled_ = mask;
        }

        return callWithContext(*this, callee.asCallable(), args, e->line);
    }

    // ── Array literal ──

    case ExprType::ArrayLiteral: {
        auto* e = static_cast<const ArrayLiteralExpr*>(expr);
        auto arr = gcNew<PraiaArray>();
        for (const auto& elem : e->elements) {
            if (elem->type == ExprType::Spread) {
                auto* spread = static_cast<const SpreadExpr*>(elem.get());
                Value val = evaluate(spread->expr.get());
                if (!val.isArray())
                    throw RuntimeError("Spread requires an array", spread->line);
                for (auto& item : val.asArray()->elements)
                    arr->elements.push_back(item);
            } else {
                arr->elements.push_back(evaluate(elem.get()));
            }
        }
        return Value(arr);
    }

    // ── Set literal ──
    //
    // #{a, b, c} → PraiaSet with each element evaluated and inserted
    // via the hash-set's unique-insert semantics (so `#{1, 1, 2}` is
    // `#{1, 2}`). Each element must be hashable; nested sets/maps/
    // arrays are rejected via the same isHashable predicate that
    // gates map keys. Spread (`...other`) accepts either a set
    // (insert each element) or an array (insert each element).
    case ExprType::SetLiteral: {
        auto* e = static_cast<const SetLiteralExpr*>(expr);
        auto set = gcNew<PraiaSet>();
        for (const auto& elem : e->elements) {
            if (elem->type == ExprType::Spread) {
                auto* spread = static_cast<const SpreadExpr*>(elem.get());
                Value val = evaluate(spread->expr.get());
                if (val.isSet()) {
                    for (auto& item : val.asSet()->elements) set->elements.insert(item);
                } else if (val.isArray()) {
                    for (auto& item : val.asArray()->elements) {
                        if (!isHashable(item))
                            throw RuntimeError("Set element must be a primitive (nil, bool, number, string)", spread->line);
                        set->elements.insert(item);
                    }
                } else {
                    throw RuntimeError("Spread into set requires a set or array", spread->line);
                }
            } else {
                Value v = evaluate(elem.get());
                if (!isHashable(v))
                    throw RuntimeError("Set element must be a primitive (nil, bool, number, string)", elem->line);
                set->elements.insert(std::move(v));
            }
        }
        return Value(set);
    }

    // ── Ternary ──

    case ExprType::Ternary: {
        auto* e = static_cast<const TernaryExpr*>(expr);
        Value cond = evaluate(e->condition.get());
        return cond.isTruthy() ? evaluate(e->thenExpr.get()) : evaluate(e->elseExpr.get());
    }

    // ── Pipe ──

    case ExprType::Pipe: {
        auto* e = static_cast<const PipeExpr*>(expr);
        Value leftVal = evaluate(e->left.get());

        // If right side is a call: f(x, y) → f(leftVal, x, y)
        if (e->right->type == ExprType::Call) {
            auto* call = static_cast<const CallExpr*>(e->right.get());
            Value callee = evaluate(call->callee.get());
            if (!callee.isCallable())
                throw RuntimeError("Pipe target must be a function", e->line, e->column);

            std::vector<Value> args;
            args.push_back(leftVal);
            for (const auto& arg : call->args)
                args.push_back(evaluate(arg.get()));

            // Reorder named args: prepend "" for the piped positional arg
            bool hasNamed = false;
            for (auto& n : call->argNames) { if (!n.empty()) { hasNamed = true; break; } }
            if (hasNamed) {
                std::vector<std::string> names;
                names.push_back("");
                for (auto& n : call->argNames) names.push_back(n);
                uint64_t mask = 0;
                args = reorderNamedArgs(callee.asCallable(), args, names, e->line, &mask);
                pendingArgsFilled_ = mask;
            }

            return callWithContext(*this, callee.asCallable(), args, e->line);
        }

        // Right side is just a function name: f → f(leftVal)
        Value callee = evaluate(e->right.get());
        if (!callee.isCallable())
            throw RuntimeError("Pipe target must be a function", e->line, e->column);

        return callWithContext(*this, callee.asCallable(), {leftVal}, e->line);
    }

    case ExprType::PipeTry: {
        auto* e = static_cast<const PipeTryExpr*>(expr);
        Value leftVal;
        bool caught = false;
        Value error;
        try {
            leftVal = evaluate(e->left.get());
        } catch (const RuntimeError& err) {
            caught = true;
            error = Value(std::string(err.what()));
        } catch (const ThrowSignal& ts) {
            caught = true;
            error = ts.value;
        }
        if (!caught) return leftVal;

        // Pass error to handler (same dispatch as |>)
        if (e->right->type == ExprType::Call) {
            auto* call = static_cast<const CallExpr*>(e->right.get());
            Value callee = evaluate(call->callee.get());
            if (!callee.isCallable())
                throw RuntimeError("Pipe error handler must be a function", e->line, e->column);
            std::vector<Value> args;
            args.push_back(error);
            for (const auto& arg : call->args)
                args.push_back(evaluate(arg.get()));
            return callWithContext(*this, callee.asCallable(), args, e->line);
        }
        Value callee = evaluate(e->right.get());
        if (!callee.isCallable())
            throw RuntimeError("Pipe error handler must be a function", e->line, e->column);
        return callWithContext(*this, callee.asCallable(), {error}, e->line);
    }

    // ── Async / Await ──

    case ExprType::Async: {
        auto* e = static_cast<const AsyncExpr*>(expr);
        // The inner expression should be a function call
        if (e->expr->type != ExprType::Call)
            throw RuntimeError("async requires a function call", e->line, e->column);
        auto* call = static_cast<const CallExpr*>(e->expr.get());

        // Evaluate callee and args on the current thread
        Value callee = evaluate(call->callee.get());
        if (!callee.isCallable())
            throw RuntimeError("Can only call functions", e->line, e->column);

        std::vector<Value> args;
        for (const auto& arg : call->args)
            args.push_back(evaluate(arg.get()));

        // Reorder named arguments if present
        bool hasNamed = false;
        for (auto& n : call->argNames) { if (!n.empty()) { hasNamed = true; break; } }
        if (hasNamed) {
            uint64_t mask = 0;
            args = reorderNamedArgs(callee.asCallable(), args, call->argNames, e->line, &mask);
            pendingArgsFilled_ = mask;
        }

        auto callable = callee.asCallable();
        int arity = callable->arity();
        bool isNative = dynamic_cast<NativeFunction*>(callable.get()) != nullptr;
        if (isNative) {
            // Natives have no defaults — require exact arity
            if (arity != -1 && static_cast<int>(args.size()) != arity)
                throw RuntimeError(callable->name() + "() expected " + std::to_string(arity) +
                    " " + argStr(arity) + " but got " + std::to_string(args.size()), e->line);
        } else {
            // Praia functions: reject too many args, fewer is fine (defaults/nil fill the rest)
            if (arity != -1 && static_cast<int>(args.size()) > arity)
                throw RuntimeError(callable->name() + "() expected at most " + std::to_string(arity) +
                    " " + argStr(arity) + " but got " + std::to_string(args.size()), e->line);
        }

        // Deep-copy heap values so the async task can't observe or corrupt
        // the caller's mutable state. Mirrors the VM's OP_ASYNC isolation
        // (vm.cpp:1860+); without it `async` in tree-walker mode would
        // silently violate the documented "no shared mutable state" contract.
        //
        // Tree-walker callables use captured `closure` Environment chains for
        // free-variable lookup. To get true isolation we must deep-copy those
        // too — otherwise `func writer(){m[k]=v}` resolves `m` through
        // writer.closure (the caller's globals) and mutates the caller's map
        // directly. We pre-register the caller globals → fresh task globals
        // mapping so any closure chain encountered during the copy gets
        // rerooted at the task globals.
        //
        // NativeFunction values are shared as-is — their state is
        // intentionally cross-VM via their own captured shared_ptrs (this is
        // how Channel/SharedMap/CancellationToken survive isolation).
        // Cycles handled via visited maps keyed on raw pointer identity.
        std::unordered_map<void*, Value> valueVisited;
        std::unordered_map<void*, std::shared_ptr<Environment>> envVisited;
        auto taskGlobals = gcNew<Environment>();
        envVisited[static_cast<void*>(globals.get())] = taskGlobals;

        std::function<Value(const Value&)> deepCopy;
        std::function<std::shared_ptr<Environment>(const std::shared_ptr<Environment>&)> copyEnv;

        copyEnv = [&](const std::shared_ptr<Environment>& src) -> std::shared_ptr<Environment> {
            if (!src) return nullptr;
            void* key = static_cast<void*>(src.get());
            auto it = envVisited.find(key);
            if (it != envVisited.end()) return it->second;
            auto fresh = gcNew<Environment>();
            envVisited[key] = fresh; // register before recursing
            fresh->parent = copyEnv(src->parent);
            for (auto& [name, val] : src->variables)
                fresh->variables[name] = deepCopy(val);
            return fresh;
        };

        // Helper: clone a Praia callable with a deep-copied closure chain.
        // NativeFunctions return as-is (NativeFunction has no closure to copy).
        // Class / Method instances clone their closure but keep class identity
        // (classes are treated as immutable).
        auto cloneCallable = [&](const std::shared_ptr<Callable>& c) -> Value {
            if (!c) return Value();
            if (auto fn = std::dynamic_pointer_cast<PraiaFunction>(c)) {
                auto clone = std::make_shared<PraiaFunction>(*fn);
                clone->closure = copyEnv(fn->closure);
                return Value(std::static_pointer_cast<Callable>(clone));
            }
            if (auto lam = std::dynamic_pointer_cast<PraiaLambda>(c)) {
                auto clone = std::make_shared<PraiaLambda>(*lam);
                clone->closure = copyEnv(lam->closure);
                return Value(std::static_pointer_cast<Callable>(clone));
            }
            if (auto gf = std::dynamic_pointer_cast<PraiaGeneratorFunction>(c)) {
                auto clone = std::make_shared<PraiaGeneratorFunction>(*gf);
                clone->closure = copyEnv(gf->closure);
                return Value(std::static_pointer_cast<Callable>(clone));
            }
            if (auto gl = std::dynamic_pointer_cast<PraiaGeneratorLambda>(c)) {
                auto clone = std::make_shared<PraiaGeneratorLambda>(*gl);
                clone->closure = copyEnv(gl->closure);
                return Value(std::static_pointer_cast<Callable>(clone));
            }
            // PraiaMethod, PraiaClass, NativeFunction: share. Methods are
            // bound at call time; classes are effectively immutable; natives
            // intentionally share (their state crosses VMs by design).
            return Value(c);
        };

        deepCopy = [&](const Value& v) -> Value {
            if (v.isMap()) {
                void* key = static_cast<void*>(v.asMap().get());
                auto it = valueVisited.find(key);
                if (it != valueVisited.end()) return it->second;
                auto copy = gcNew<PraiaMap>();
                Value result(copy);
                valueVisited[key] = result;
                for (auto& [k, val] : v.asMap()->entries)
                    copy->entries[k] = deepCopy(val);
                return result;
            }
            if (v.isArray()) {
                void* key = static_cast<void*>(v.asArray().get());
                auto it = valueVisited.find(key);
                if (it != valueVisited.end()) return it->second;
                auto copy = gcNew<PraiaArray>();
                Value result(copy);
                valueVisited[key] = result;
                for (auto& el : v.asArray()->elements)
                    copy->elements.push_back(deepCopy(el));
                return result;
            }
            if (v.isInstance()) {
                void* key = static_cast<void*>(v.asInstance().get());
                auto it = valueVisited.find(key);
                if (it != valueVisited.end()) return it->second;
                auto copy = gcNew<PraiaInstance>();
                copy->klass = v.asInstance()->klass; // class is immutable
                Value result(copy);
                valueVisited[key] = result;
                for (auto& [k, fv] : v.asInstance()->fields)
                    copy->fields[k] = deepCopy(fv);
                return result;
            }
            if (v.isCallable()) {
                void* key = static_cast<void*>(v.asCallable().get());
                auto it = valueVisited.find(key);
                if (it != valueVisited.end()) return it->second;
                Value result = cloneCallable(v.asCallable());
                valueVisited[key] = result;
                return result;
            }
            // Primitives, strings, futures share by value/ref.
            return v;
        };

        // Populate the fresh task globals from the caller's globals.
        for (auto& [name, val] : globals->variables)
            taskGlobals->variables[name] = deepCopy(val);

        // Deep-copy the callable so its closure chain is rooted at task globals.
        Value calleeCopy = deepCopy(Value(callable));
        callable = calleeCopy.asCallable();

        // Deep-copy arg values so the task can't mutate the caller's objects.
        std::vector<Value> argsCopy;
        argsCopy.reserve(args.size());
        for (auto& a : args) argsCopy.push_back(deepCopy(a));

        // Spawn the call in a background thread with a task-local Interpreter.
        auto sharedFuture = std::async(std::launch::async,
            [callable, argsCopy = std::move(argsCopy), taskGlobals]() -> Value {
                Interpreter taskInterp(taskGlobals);
                GcHeap::current().disable(); // task interpreters are short-lived
                return callable->call(taskInterp, argsCopy);
            }).share();

        auto fut = std::make_shared<PraiaFuture>();
        fut->future = sharedFuture;
        // Keep an extra reference so a discarded future (`async fn()` with no
        // binding) doesn't block on its destructor — see the matching call
        // in vm.cpp's OP_ASYNC for the C++ standard wart this works around.
        retainInflightFuture(sharedFuture);
        return Value(fut);
    }
    case ExprType::Await: {
        auto* e = static_cast<const AwaitExpr*>(expr);
        Value val = evaluate(e->expr.get());
        if (!val.isFuture())
            throw RuntimeError("Can only await a future", e->line, e->column);

        try {
            return val.asFuture()->future.get();
        } catch (const RuntimeError&) {
            throw;
        } catch (const std::exception& ex) {
            throw RuntimeError(std::string("Async task failed: ") + ex.what(), e->line);
        }
    }

    // ── Yield ──

    case ExprType::Yield: {
        auto* e = static_cast<const YieldExpr*>(expr);
        Value val;
        if (e->value) val = evaluate(e->value.get());

        // Find the generator object stored in the environment
        Value genVal;
        try { genVal = env->get("__gen__", e->line); }
        catch (...) { throw RuntimeError("yield outside of generator", e->line, e->column); }
        if (!genVal.isGenerator())
            throw RuntimeError("yield outside of generator", e->line, e->column);
        auto gen = genVal.asGenerator();

        gen->lastYielded = val;
        gen->state = PraiaGenerator::State::SUSPENDED;
        Fiber::suspend();  // return control to .next() caller
        if (gen->done) throw ReturnSignal{Value()}; // generator was abandoned
        gen->state = PraiaGenerator::State::RUNNING;
        return gen->sendValue;
    }

    // ── Lambda ──

    case ExprType::Lambda: {
        auto* e = static_cast<const LambdaExpr*>(expr);
        if (e->isGenerator) {
            auto lam = std::make_shared<PraiaGeneratorLambda>();
            lam->params = e->params;
            lam->restParam = e->restParam;
            lam->expr = e;
            lam->closure = env;
            lam->astOwner = currentAstOwner_;
            return Value(std::static_pointer_cast<Callable>(lam));
        }
        auto lam = std::make_shared<PraiaLambda>();
        lam->params = e->params;
        lam->restParam = e->restParam;
        lam->expr = e;
        lam->closure = env;
        lam->astOwner = currentAstOwner_;
        return Value(std::static_pointer_cast<Callable>(lam));
    }

    // ── Map literal ──

    case ExprType::MapLiteral: {
        auto* e = static_cast<const MapLiteralExpr*>(expr);
        auto map = gcNew<PraiaMap>();
        for (size_t i = 0; i < e->keys.size(); i++) {
            if (!e->keys[i] && e->values[i]->type == ExprType::Spread) {
                // Spread: {...other}
                auto* spread = static_cast<const SpreadExpr*>(e->values[i].get());
                Value val = evaluate(spread->expr.get());
                if (!val.isMap())
                    throw RuntimeError("Spread in map requires a map", spread->line);
                for (auto& [k, v] : val.asMap()->entries)
                    map->entries[k] = v;
            } else {
                Value key = evaluate(e->keys[i].get());
                if (!isHashable(key))
                    throw RuntimeError("Unhashable type used as map key", e->line, e->column);
                map->entries[key] = evaluate(e->values[i].get());
            }
        }
        return Value(map);
    }

    // ── Index access ──

    case ExprType::Index: {
        auto* e = static_cast<const IndexExpr*>(expr);
        Value obj = evaluate(e->object.get());
        if (e->isOptional && obj.isNil()) return Value();
        Value idx = evaluate(e->index.get());
        // Optional access (`?[`) suppresses "missing value" failures (out-of-
        // bounds, missing map key) but still throws on type errors (e.g.
        // string index on an array) — those indicate a programming bug, not
        // an absent element.
        if (obj.isArray()) {
            if (!idx.isNumber())
                throw RuntimeError("Array index must be a number", e->line, e->column);
            auto& elems = obj.asArray()->elements;
            int i = static_cast<int>(idx.asNumber());
            if (i < 0) i += static_cast<int>(elems.size());
            if (i < 0 || i >= static_cast<int>(elems.size())) {
                if (e->isOptional) return Value();
                throw RuntimeError("Array index out of bounds", e->line, e->column);
            }
            return elems[i];
        }
        if (obj.isString()) {
            if (!idx.isNumber())
                throw RuntimeError("String index must be a number", e->line, e->column);
            auto& str = obj.asString();
            int i = static_cast<int>(idx.asNumber());
#ifdef HAVE_UTF8PROC
            auto gs = utf8_graphemes(str);
            int len = static_cast<int>(gs.size());
            if (i < 0) i += len;
            if (i < 0 || i >= len) {
                if (e->isOptional) return Value();
                throw RuntimeError("String index out of bounds", e->line, e->column);
            }
            return Value(gs[i]);
#else
            if (i < 0) i += static_cast<int>(str.size());
            if (i < 0 || i >= static_cast<int>(str.size())) {
                if (e->isOptional) return Value();
                throw RuntimeError("String index out of bounds", e->line, e->column);
            }
            return Value(std::string(1, str[i]));
#endif
        }
        if (obj.isMap()) {
            auto& entries = obj.asMap()->entries;
            auto it = entries.find(idx);
            // Maps are dynamic key-value collections; missing keys return nil
            // rather than throwing, matching Python's defaultdict, JS objects,
            // and Lua tables. Use `m.has(k)` to distinguish "absent" from
            // "present with value nil" if it matters.
            if (it == entries.end()) return Value();
            return it->second;
        }
        if (obj.isInstance()) {
            auto [found, result] = callDunder(*this, obj.asInstance(), "__index", {idx});
            if (found) return result;
        }
        throw RuntimeError("Can only index into arrays, strings, and maps", e->line, e->column);
    }

    // ── Index assignment ──

    case ExprType::IndexAssign: {
        auto* e = static_cast<const IndexAssignExpr*>(expr);
        Value obj = evaluate(e->object.get());
        Value idx = evaluate(e->index.get());
        Value val = evaluate(e->value.get());
        if (obj.isArray()) {
            if (!idx.isNumber())
                throw RuntimeError("Array index must be a number", e->line, e->column);
            auto& elems = obj.asArray()->elements;
            int i = static_cast<int>(idx.asNumber());
            if (i < 0) i += static_cast<int>(elems.size());
            if (i < 0 || i >= static_cast<int>(elems.size()))
                throw RuntimeError("Array index out of bounds", e->line, e->column);
            elems[i] = val;
            return val;
        }
        if (obj.isMap()) {
            if (!isHashable(idx))
                throw RuntimeError("Unhashable type used as map key", e->line, e->column);
            obj.asMap()->entries[idx] = val;
            return val;
        }
        if (obj.isInstance()) {
            auto [found, result] = callDunder(*this, obj.asInstance(), "__indexSet", {idx, val});
            if (found) return result;
        }
        if (obj.isString())
            throw RuntimeError("Strings are immutable — cannot assign to index", e->line, e->column);
        throw RuntimeError("Can only assign to array or map indices", e->line, e->column);
    }

    // ── Dot access ──

    case ExprType::Dot: {
        auto* e = static_cast<const DotExpr*>(expr);
        Value obj = evaluate(e->object.get());
        if (e->isOptional && obj.isNil()) return Value();

        if (obj.isTagged()) {
            auto t = obj.asTagged();
            if (e->field == "tag") return Value(t->tag);
            if (e->field == "values") {
                auto arr = gcNew<PraiaArray>();
                arr->elements = t->values;
                return Value(arr);
            }
            throw RuntimeError("Tagged value has no field '" + e->field + "'", e->line, e->column);
        }

        if (obj.isGenerator()) {
            auto gen = obj.asGenerator();
            if (e->field == "next") {
                return Value(makeNative("next", -1,
                    [gen](const std::vector<Value>& args) -> Value {
                        if (gen->state == PraiaGenerator::State::COMPLETED) {
                            auto result = gcNew<PraiaMap>();
                            result->entries[Value("value")] = Value();
                            result->entries[Value("done")] = Value(true);
                            return Value(result);
                        }
                        gen->sendValue = args.empty() ? Value() : args[0];
                        gen->fiber->resume();

                        if (!gen->errorMessage.empty()) {
                            std::string msg = gen->errorMessage;
                            gen->releaseAfterCompletion();
                            throw RuntimeError(msg, 0);
                        }

                        auto result = gcNew<PraiaMap>();
                        result->entries[Value("value")] = gen->lastYielded;
                        result->entries[Value("done")] = Value(gen->done);
                        // If the generator just completed, drop the 256KB fiber
                        // stack and captured env immediately rather than waiting
                        // for GC.
                        if (gen->state == PraiaGenerator::State::COMPLETED) {
                            gen->releaseAfterCompletion();
                        }
                        return Value(result);
                    }));
            }
            if (e->field == "done") {
                return Value(gen->state == PraiaGenerator::State::COMPLETED);
            }
            throw RuntimeError("Generator has no property '" + e->field + "'", e->line, e->column);
        }

        if (obj.isInstance()) {
            auto inst = obj.asInstance();
            // Fields first
            auto fit = inst->fields.find(e->field);
            if (fit != inst->fields.end()) return fit->second;
            // Then methods
            auto* methodDecl = inst->klass->findMethod(e->field);
            if (methodDecl) {
                auto bound = std::make_shared<PraiaMethod>();
                bound->methodName = e->field;
                bound->params = methodDecl->params;
                bound->decl = methodDecl;
                bound->instance = inst;
                auto walk = inst->klass;
                while (walk && !walk->methods.count(e->field))
                    walk = walk->superclass;
                bound->definingClass = walk ? walk : inst->klass;
                bound->closure = bound->definingClass->closure;
                bound->astOwner = inst->klass->astOwner;
                Value result = Value(std::static_pointer_cast<Callable>(bound));
                // Apply decorators if present
                if (!methodDecl->decorators.empty()) {
                    for (int i = static_cast<int>(methodDecl->decorators.size()) - 1; i >= 0; i--) {
                        Value deco = evaluate(methodDecl->decorators[i].get());
                        result = callWithContext(*this, deco.asCallable(),
                                                {result}, methodDecl->line);
                    }
                }
                return result;
            }
            // Fall through to universal methods below
        }

        // Map fields take priority over methods
        if (obj.isMap()) {
            auto& entries = obj.asMap()->entries;
            auto it = entries.find(Value(e->field));
            if (it != entries.end()) return it->second;
            if (e->field == "has" || e->field == "get" || e->field == "delete" || e->field == "merge" ||
                e->field == "entries" || e->field == "clear")
                return getMapMethod(obj.asMap(), e->field, e->line);
            // Fall through to universal methods below
        }

        // Universal methods — work on any type, but instance
        // fields/methods and map keys take priority (checked above).
        if (e->field == "toString") {
            Value captured = obj;
            return Value(makeNative("toString", 0,
                [captured](const std::vector<Value>&) -> Value {
                    return Value(captured.toString());
                }));
        }
        if (e->field == "toNum") {
            Value captured = obj;
            return Value(makeNative("toNum", 0,
                [captured](const std::vector<Value>&) -> Value {
                    if (captured.isNumber()) return captured;
                    if (captured.isBool()) return Value(captured.asBool() ? 1.0 : 0.0);
                    if (captured.isString()) {
                        auto& s = captured.asString();
                        // Case-insensitive bool strings
                        std::string lower = s;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                        if (lower == "true") return Value(1.0);
                        if (lower == "false") return Value(0.0);
                        // Try numeric parse
                        try {
                            size_t pos = 0;
                            double d = std::stod(s, &pos);
                            if (pos == s.size()) return Value(d);
                        } catch (...) {}
                        throw RuntimeError("toNum: cannot parse \"" + s + "\" as a number", 0);
                    }
                    throw RuntimeError("toNum: cannot convert " + captured.toString() +
                                       " to a number", 0);
                }));
        }

        // Instance/map with no matching field and no universal method match
        if (obj.isInstance()) {
            if (e->isOptional) return Value();
            throw RuntimeError("Instance has no property '" + e->field + "'", e->line, e->column);
        }
        if (obj.isMap()) {
            // Maps return nil for missing fields; see the matching IndexGet
            // path above for rationale.
            return Value();
        }
        // Static methods on classes
        if (obj.isCallable()) {
            auto klass = std::dynamic_pointer_cast<PraiaClass>(obj.asCallable());
            if (klass) {
                auto walk = klass;
                while (walk) {
                    auto it = walk->staticMethods.find(e->field);
                    if (it != walk->staticMethods.end()) {
                        auto* decl = it->second;
                        auto method = std::make_shared<PraiaMethod>();
                        method->methodName = decl->name;
                        method->params = decl->params;
                        method->decl = decl;
                        method->closure = walk->closure;
                        method->instance = nullptr;
                        method->definingClass = walk;
                        method->astOwner = walk->astOwner;
                        Value result = Value(std::static_pointer_cast<Callable>(method));
                        if (!decl->decorators.empty()) {
                            for (int i = static_cast<int>(decl->decorators.size()) - 1; i >= 0; i--) {
                                Value deco = evaluate(decl->decorators[i].get());
                                result = callWithContext(*this, deco.asCallable(), {result}, decl->line);
                            }
                        }
                        return result;
                    }
                    walk = walk->superclass;
                }
            }
        }

        if (obj.isString())
            return getStringMethod(obj.asString(), e->field, e->line);
        if (obj.isArray())
            return getArrayMethod(obj.asArray(), e->field, e->line, this);
        if (obj.isSet())
            return getSetMethod(obj.asSet(), e->field, e->line);

        throw RuntimeError("Cannot access field '" + e->field + "' on this type", e->line, e->column);
    }

    // ── Dot assignment ──

    case ExprType::DotAssign: {
        auto* e = static_cast<const DotAssignExpr*>(expr);
        Value obj = evaluate(e->object.get());
        Value val = evaluate(e->value.get());
        if (obj.isInstance()) {
            obj.asInstance()->fields[e->field] = val;
            return val;
        }
        if (obj.isMap()) {
            obj.asMap()->entries[Value(e->field)] = val;
            return val;
        }
        throw RuntimeError("Can only set fields on instances and maps", e->line, e->column);
    }

    // ── String interpolation ──

    case ExprType::InterpolatedString: {
        auto* e = static_cast<const InterpolatedStringExpr*>(expr);
        std::string result;
        for (const auto& part : e->parts)
            result += evaluate(part.get()).toString();
        return Value(std::move(result));
    }

    case ExprType::Spread: {
        throw RuntimeError("Unexpected spread expression", expr->line);
    }

    default:
        throw RuntimeError("Unknown expression type", expr->line);
    }
}

// ── GC root marking ──

void Interpreter::gcMarkRoots(GcHeap& heap) {
    heap.markEnvironment(globals.get());
    heap.markEnvironment(env.get());
    // Mark caller scopes that are on the C++ call stack (invisible to GC)
    for (auto& savedEnv : savedEnvStack_)
        heap.markEnvironment(savedEnv.get());
    if (sysMap) heap.markValue(Value(sysMap));
    for (auto& [k, v] : grainCache) heap.markValue(v);
}
