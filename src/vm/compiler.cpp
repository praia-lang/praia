#include "compiler.h"
#include "vm.h"
#include <iostream>

// ── Helpers ──────────────────────────────────────────────────

Chunk& Compiler::currentChunk() { return current->function->chunk; }

void Compiler::emit(OpCode op, int line, int column) { currentChunk().write(op, line, column); }
void Compiler::emit(uint8_t byte, int line, int column) { currentChunk().write(byte, line, column); }
void Compiler::emitU16(uint16_t val, int line, int column) { currentChunk().writeU16(val, line, column); }

void Compiler::emitConstant(Value value, int line, int column) {
    uint16_t idx = currentChunk().addConstant(std::move(value));
    emit(OpCode::OP_CONSTANT, line, column);
    emitU16(idx, line, column);
}

uint16_t Compiler::identifierConstant(const std::string& name) {
    return currentChunk().addConstant(Value(name));
}

int Compiler::emitJump(OpCode op, int line, int column) {
    emit(op, line, column);
    emit(0xFF, line, column); // placeholder
    emit(0xFF, line, column);
    return currentChunk().size() - 2;
}

void Compiler::patchJump(int offset) {
    int jump = currentChunk().size() - offset - 2;
    currentChunk().patchU16(offset, static_cast<uint16_t>(jump));
}

void Compiler::emitLoop(int loopStart, int line, int column) {
    emit(OpCode::OP_JUMP_BACK, line, column);
    int offset = currentChunk().size() - loopStart + 2;
    emitU16(static_cast<uint16_t>(offset), line, column);
}

void Compiler::error(const std::string& msg, int line) {
    std::cerr << "[line " << line << "] Compile error: " << msg << std::endl;
    hadError = true;
}

// ── Scope ────────────────────────────────────────────────────

void Compiler::beginScope() { current->scopeDepth++; }

void Compiler::endScope(int line) {
    current->scopeDepth--;
    while (!current->locals.empty() &&
           current->locals.back().depth > current->scopeDepth) {
        if (current->locals.back().isCaptured) {
            emit(OpCode::OP_CLOSE_UPVALUE, line);
        } else {
            emit(OpCode::OP_POP, line);
        }
        current->locals.pop_back();
    }
}

void Compiler::addLocal(const std::string& name) {
    current->locals.push_back({name, current->scopeDepth, false});
}

int Compiler::resolveLocal(CompilerState* state, const std::string& name) {
    for (int i = static_cast<int>(state->locals.size()) - 1; i >= 0; i--) {
        if (state->locals[i].name == name) return i;
    }
    return -1;
}

int Compiler::addUpvalue(CompilerState* state, uint16_t index, bool isLocal) {
    for (int i = 0; i < static_cast<int>(state->upvalues.size()); i++) {
        auto& uv = state->upvalues[i];
        if (uv.index == index && uv.isLocal == isLocal) return i;
    }
    state->upvalues.push_back({index, isLocal});
    state->function->upvalueCount = static_cast<int>(state->upvalues.size());
    return static_cast<int>(state->upvalues.size()) - 1;
}

int Compiler::resolveUpvalue(CompilerState* state, const std::string& name) {
    if (!state->enclosing) return -1;

    int local = resolveLocal(state->enclosing, name);
    if (local != -1) {
        state->enclosing->locals[local].isCaptured = true;
        return addUpvalue(state, static_cast<uint16_t>(local), true);
    }

    int upvalue = resolveUpvalue(state->enclosing, name);
    if (upvalue != -1) {
        return addUpvalue(state, static_cast<uint16_t>(upvalue), false);
    }

    return -1;
}

// ── Compile entry point ──────────────────────────────────────

std::shared_ptr<CompiledFunction> Compiler::compile(const std::vector<StmtPtr>& program) {
    auto fn = std::make_shared<CompiledFunction>();
    fn->name = "<script>";

    CompilerState state;
    state.function = fn;
    current = &state;

    // Reserve slot 0 for the script itself (convention from Crafting Interpreters)
    addLocal("");

    for (auto& stmt : program) {
        compileStmt(stmt.get());
    }

    emit(OpCode::OP_NIL, 0);
    emit(OpCode::OP_RETURN, 0);

    current = nullptr;
    return hadError ? nullptr : fn;
}

// ── Statement compilation ────────────────────────────────────

void Compiler::compileStmt(const Stmt* stmt) {
    switch (stmt->type) {
    case StmtType::Expr:    compileExprStmt(static_cast<const ExprStmt*>(stmt)); break;
    case StmtType::Let:     compileLetStmt(static_cast<const LetStmt*>(stmt)); break;
    case StmtType::Block:   compileBlockStmt(static_cast<const BlockStmt*>(stmt)); break;
    case StmtType::If:      compileIfStmt(static_cast<const IfStmt*>(stmt)); break;
    case StmtType::While:   compileWhileStmt(static_cast<const WhileStmt*>(stmt)); break;
    case StmtType::For:     compileForStmt(static_cast<const ForStmt*>(stmt)); break;
    case StmtType::ForIn:   compileForInStmt(static_cast<const ForInStmt*>(stmt)); break;
    case StmtType::Func:    compileFuncStmt(static_cast<const FuncStmt*>(stmt)); break;
    case StmtType::Return:  compileReturnStmt(static_cast<const ReturnStmt*>(stmt)); break;
    case StmtType::Break:   compileBreakStmt(static_cast<const BreakStmt*>(stmt)); break;
    case StmtType::Continue:compileContinueStmt(static_cast<const ContinueStmt*>(stmt)); break;
    case StmtType::Class:   compileClassStmt(static_cast<const ClassStmt*>(stmt)); break;
    case StmtType::Enum:    compileEnumStmt(static_cast<const EnumStmt*>(stmt)); break;
    case StmtType::Throw:   compileThrowStmt(static_cast<const ThrowStmt*>(stmt)); break;
    case StmtType::TryCatch:compileTryCatchStmt(static_cast<const TryCatchStmt*>(stmt)); break;
    case StmtType::Ensure:  compileEnsureStmt(static_cast<const EnsureStmt*>(stmt)); break;
    case StmtType::Defer: {
        auto* s = static_cast<const DeferStmt*>(stmt);
        // Wrap the deferred expression in a 0-arg closure and push it
        // onto the VM's per-frame defer stack via OP_DEFER.
        auto fn = std::make_shared<CompiledFunction>();
        fn->name = "<defer>";
        fn->arity = 0;

        CompilerState deferState;
        deferState.function = fn;
        deferState.enclosing = current;
        deferState.scopeDepth = 1;
        current = &deferState;

        compileExpr(s->expr.get());
        emit(OpCode::OP_POP, s->line, s->column);
        emit(OpCode::OP_NIL, s->line, s->column);
        emit(OpCode::OP_RETURN, s->line, s->column);

        current = deferState.enclosing;
        fn->upvalueCount = static_cast<int>(deferState.upvalues.size());

        // Create closure constant (same pattern as compileFuncStmt)
        auto proto = std::make_shared<ObjClosure>(fn);
        auto wrapper = std::make_shared<VMClosureCallable>(proto.get());
        wrapper->ownedPrototype = proto;
        uint16_t fnIdx = currentChunk().addConstant(
            Value(std::static_pointer_cast<Callable>(wrapper)));

        emit(OpCode::OP_CLOSURE, s->line, s->column);
        emitU16(fnIdx, s->line, s->column);
        for (auto& uv : deferState.upvalues) {
            emit(uv.isLocal ? 1 : 0, s->line, s->column);
            emitU16(uv.index, s->line, s->column);
        }
        emit(OpCode::OP_DEFER, s->line, s->column);
        break;
    }
    case StmtType::Use:     compileUseStmt(static_cast<const UseStmt*>(stmt)); break;
    case StmtType::Export:  compileExportStmt(static_cast<const ExportStmt*>(stmt)); break;
    default: error("Unknown statement type", stmt->line); break;
    }
}

void Compiler::compileExprStmt(const ExprStmt* stmt) {
    compileExpr(stmt->expr.get());
    emit(OpCode::OP_POP, stmt->line, stmt->column);
}

void Compiler::compileLetStmt(const LetStmt* stmt) {
    if (!stmt->pattern.empty()) {
        // Destructuring
        compileExpr(stmt->initializer.get());

        if (stmt->isArrayPattern) {
            // Array destructuring: let [a, b, ...rest] = expr
            // Value is on top of stack. Store it in a temp local.
            addLocal("__destr__");
            int arrSlot = resolveLocal(current, "__destr__");

            for (size_t i = 0; i < stmt->pattern.size(); i++) {
                auto& p = stmt->pattern[i];
                if (p.isRest) {
                    // rest = __arraySlice(arr, i)
                    emit(OpCode::OP_GET_GLOBAL, stmt->line, stmt->column);
                    emitU16(identifierConstant("__arraySlice"), stmt->line, stmt->column);
                    emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
                    emitU16(static_cast<uint16_t>(arrSlot), stmt->line, stmt->column);
                    emitConstant(Value(static_cast<int64_t>(i)), stmt->line, stmt->column);
                    emit(OpCode::OP_CALL, stmt->line, stmt->column);
                    emit(2, stmt->line, stmt->column);
                } else {
                    emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
                    emitU16(static_cast<uint16_t>(arrSlot), stmt->line, stmt->column);
                    emitConstant(Value(static_cast<int64_t>(i)), stmt->line, stmt->column);
                    emit(OpCode::OP_INDEX_GET, stmt->line, stmt->column);
                }
                if (current->scopeDepth > 0) {
                    addLocal(p.name);
                } else {
                    emit(OpCode::OP_DEFINE_GLOBAL, stmt->line, stmt->column);
                    emitU16(identifierConstant(p.name), stmt->line, stmt->column);
                }
            }
            return;
        } else {
            // Map destructuring: let {name, age, ...rest} = expr
            addLocal("__destr__");
            int mapSlot = resolveLocal(current, "__destr__");

            // Collect extracted keys for rest computation
            std::vector<std::string> extractedKeys;
            for (auto& p : stmt->pattern) {
                if (!p.isRest) {
                    std::string key = p.key.empty() ? p.name : p.key;
                    extractedKeys.push_back(key);
                }
            }

            for (auto& p : stmt->pattern) {
                if (p.isRest) {
                    // rest = __mapRest(map, [extracted_keys...])
                    emit(OpCode::OP_GET_GLOBAL, stmt->line, stmt->column);
                    emitU16(identifierConstant("__mapRest"), stmt->line, stmt->column);
                    emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
                    emitU16(static_cast<uint16_t>(mapSlot), stmt->line, stmt->column);
                    // Build array of extracted key names
                    for (auto& k : extractedKeys)
                        emitConstant(Value(k), stmt->line, stmt->column);
                    emit(OpCode::OP_BUILD_ARRAY, stmt->line, stmt->column);
                    emitU16(static_cast<uint16_t>(extractedKeys.size()), stmt->line, stmt->column);
                    emit(OpCode::OP_CALL, stmt->line, stmt->column);
                    emit(2, stmt->line, stmt->column);
                } else {
                    emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
                    emitU16(static_cast<uint16_t>(mapSlot), stmt->line, stmt->column);
                    std::string key = p.key.empty() ? p.name : p.key;
                    emitConstant(Value(key), stmt->line, stmt->column);
                    emit(OpCode::OP_INDEX_GET, stmt->line, stmt->column);
                }
                if (current->scopeDepth > 0) {
                    addLocal(p.name);
                } else {
                    emit(OpCode::OP_DEFINE_GLOBAL, stmt->line, stmt->column);
                    emitU16(identifierConstant(p.name), stmt->line, stmt->column);
                }
            }
            return;
        }
    }

    // Simple let
    if (stmt->initializer) {
        compileExpr(stmt->initializer.get());
    } else {
        emit(OpCode::OP_NIL, stmt->line, stmt->column);
    }

    if (current->scopeDepth > 0) {
        addLocal(stmt->name);
    } else {
        uint16_t nameIdx = identifierConstant(stmt->name);
        emit(OpCode::OP_DEFINE_GLOBAL, stmt->line, stmt->column);
        emitU16(nameIdx, stmt->line, stmt->column);
    }
}

void Compiler::compileBlockStmt(const BlockStmt* stmt) {
    beginScope();
    for (auto& s : stmt->statements) {
        compileStmt(s.get());
    }
    endScope(stmt->line);
}

// ── Phase 2+ stubs (will be implemented incrementally) ──────

void Compiler::compileIfStmt(const IfStmt* stmt) {
    // Condition
    compileExpr(stmt->condition.get());
    int thenJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line, stmt->column);

    // Then branch
    compileStmt(stmt->thenBranch.get());
    int elseJump = emitJump(OpCode::OP_JUMP, stmt->line, stmt->column);

    patchJump(thenJump);

    // Elif branches
    std::vector<int> elifEndJumps;
    for (auto& elif : stmt->elifBranches) {
        compileExpr(elif.condition.get());
        int elifJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line, stmt->column);
        compileStmt(elif.body.get());
        elifEndJumps.push_back(emitJump(OpCode::OP_JUMP, stmt->line, stmt->column));
        patchJump(elifJump);
    }

    // Else branch
    if (stmt->elseBranch) {
        compileStmt(stmt->elseBranch.get());
    }

    patchJump(elseJump);
    for (int j : elifEndJumps) patchJump(j);
}


void Compiler::compileWhileStmt(const WhileStmt* stmt) {
    int loopStart = currentChunk().size();

    current->loops.push_back({loopStart, loopStart, {}, {}, current->scopeDepth, current->tryDepth});

    compileExpr(stmt->condition.get());
    int exitJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line, stmt->column);

    compileStmt(stmt->body.get());

    // Patch continue jumps to here (just before the loop-back)
    auto& loop = current->loops.back();
    for (int j : loop.continueJumps) patchJump(j);

    emitLoop(loopStart, stmt->line, stmt->column);

    patchJump(exitJump);

    for (int j : loop.breakJumps) patchJump(j);
    current->loops.pop_back();
}

void Compiler::compileForStmt(const ForStmt* stmt) {
    beginScope();

    // Evaluate bounds
    compileExpr(stmt->start.get());
    addLocal(stmt->varName);  // loop var

    compileExpr(stmt->end.get());
    addLocal("__end__");  // hidden end bound

    int loopStart = currentChunk().size();
    current->loops.push_back({loopStart, loopStart, {}, {}, current->scopeDepth, current->tryDepth});

    // Condition: i < end
    int iSlot = resolveLocal(current, stmt->varName);
    int endSlot = resolveLocal(current, "__end__");
    emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(iSlot), stmt->line, stmt->column);
    emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(endSlot), stmt->line, stmt->column);
    emit(OpCode::OP_LESS, stmt->line, stmt->column);
    int exitJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line, stmt->column);

    // Body
    compileStmt(stmt->body.get());

    // Continue target: increment section
    current->loops.back().continueTarget = currentChunk().size();
    // Patch any continue jumps to here
    for (int j : current->loops.back().continueJumps) patchJump(j);

    // Increment: i = i + 1
    emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(iSlot), stmt->line, stmt->column);
    emitConstant(Value(static_cast<int64_t>(1)), stmt->line, stmt->column);
    emit(OpCode::OP_ADD, stmt->line, stmt->column);
    emit(OpCode::OP_SET_LOCAL, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(iSlot), stmt->line, stmt->column);
    emit(OpCode::OP_POP, stmt->line, stmt->column);

    emitLoop(loopStart, stmt->line, stmt->column);
    patchJump(exitJump);

    auto& loop = current->loops.back();
    for (int j : loop.breakJumps) patchJump(j);
    current->loops.pop_back();

    endScope(stmt->line);
}

void Compiler::compileForInStmt(const ForInStmt* stmt) {
    // Compiled as:
    // let __iter__ = __iterEntries(<iterable>)   // normalizes maps/strings to arrays
    // let __idx__ = 0
    // while (__idx__ < len(__iter__)) {
    //     let varName = __iter__[__idx__]
    //     <body>
    //     __idx__ = __idx__ + 1
    // }

    beginScope();

    // Normalize iterable via __iterEntries (arrays pass through,
    // maps become [{key,value},...], strings become char arrays,
    // generators pass through as-is)
    emit(OpCode::OP_GET_GLOBAL, stmt->line, stmt->column);
    emitU16(identifierConstant("__iterEntries"), stmt->line, stmt->column);
    compileExpr(stmt->iterable.get());
    emit(OpCode::OP_CALL, stmt->line, stmt->column);
    emit(1, stmt->line, stmt->column);
    addLocal("__iter__");
    int iterSlot = resolveLocal(current, "__iter__");

    // Index counter (only meaningful for arrays, ignored for generators)
    emitConstant(Value(static_cast<int64_t>(0)), stmt->line, stmt->column);
    addLocal("__idx__");
    int idxSlot = resolveLocal(current, "__idx__");

    // Placeholder for __result__ (reused each iteration)
    emit(OpCode::OP_NIL, stmt->line, stmt->column);
    addLocal("__result__");
    int resultSlot = resolveLocal(current, "__result__");

    int loopStart = currentChunk().size();
    current->loops.push_back({loopStart, loopStart, {}, {}, current->scopeDepth, current->tryDepth});

    // Call __iterNext(__iter__, __idx__) — returns {value, done}
    emit(OpCode::OP_GET_GLOBAL, stmt->line, stmt->column);
    emitU16(identifierConstant("__iterNext"), stmt->line, stmt->column);
    emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(iterSlot), stmt->line, stmt->column);
    emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(idxSlot), stmt->line, stmt->column);
    emit(OpCode::OP_CALL, stmt->line, stmt->column);
    emit(2, stmt->line, stmt->column);
    // Store result in pre-allocated local
    emit(OpCode::OP_SET_LOCAL, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(resultSlot), stmt->line, stmt->column);
    emit(OpCode::OP_POP, stmt->line, stmt->column); // pop SET_LOCAL's leftover

    // Check __result__.done — exit loop if true
    emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(resultSlot), stmt->line, stmt->column);
    emitConstant(Value("done"), stmt->line, stmt->column);
    emit(OpCode::OP_INDEX_GET, stmt->line, stmt->column);
    emit(OpCode::OP_NOT, stmt->line, stmt->column);
    int exitJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line, stmt->column);

    // Loop var = __result__.value
    beginScope();
    emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(resultSlot), stmt->line, stmt->column);
    emitConstant(Value("value"), stmt->line, stmt->column);
    emit(OpCode::OP_INDEX_GET, stmt->line, stmt->column);

    if (!stmt->destructureKeys.empty()) {
        addLocal("__entry__");
        int entrySlot = resolveLocal(current, "__entry__");
        for (auto& dk : stmt->destructureKeys) {
            emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
            emitU16(static_cast<uint16_t>(entrySlot), stmt->line, stmt->column);
            emitConstant(Value(dk), stmt->line, stmt->column);
            emit(OpCode::OP_INDEX_GET, stmt->line, stmt->column);
            addLocal(dk);
        }
    } else {
        addLocal(stmt->varName);
    }

    // Body
    compileStmt(stmt->body.get());

    endScope(stmt->line); // pops loop vars

    // Continue target
    current->loops.back().continueTarget = currentChunk().size();
    for (int j : current->loops.back().continueJumps) patchJump(j);

    // Increment: __idx__ += 1
    emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(idxSlot), stmt->line, stmt->column);
    emitConstant(Value(static_cast<int64_t>(1)), stmt->line, stmt->column);
    emit(OpCode::OP_ADD, stmt->line, stmt->column);
    emit(OpCode::OP_SET_LOCAL, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(idxSlot), stmt->line, stmt->column);
    emit(OpCode::OP_POP, stmt->line, stmt->column);

    emitLoop(loopStart, stmt->line, stmt->column);

    // Exit jump from done check lands here
    patchJump(exitJump);

    auto& loop = current->loops.back();
    for (int j : loop.breakJumps) patchJump(j);
    current->loops.pop_back();

    endScope(stmt->line); // pops __iter__, __idx__, __result__
}

void Compiler::compileFuncStmt(const FuncStmt* stmt) {
    // Compile the function body into a new CompiledFunction
    auto fn = std::make_shared<CompiledFunction>();
    fn->name = stmt->name;
    fn->arity = static_cast<int>(stmt->params.size());
    fn->paramNames = stmt->params;
    fn->restParam = stmt->restParam;
    fn->isGenerator = stmt->isGenerator;

    CompilerState funcState;
    funcState.enclosing = current;
    funcState.function = fn;
    funcState.isGenerator = stmt->isGenerator;
    funcState.scopeDepth = current->scopeDepth + 1;
    current = &funcState;

    // Slot 0 = the function itself (convention)
    addLocal(stmt->name);

    // Parameters become locals
    for (auto& param : stmt->params) {
        addLocal(param);
    }

    // Rest param local (filled at runtime by VM::callClosure)
    if (!stmt->restParam.empty()) {
        addLocal(stmt->restParam);
    }

    // Emit default parameter evaluation: only when the caller did NOT provide
    // an argument for this parameter. Explicit nil from the caller is preserved.
    for (size_t i = 0; i < stmt->defaults.size(); i++) {
        if (stmt->defaults[i]) {
            int slot = static_cast<int>(i) + 1; // +1 for slot 0 (function itself)
            emit(OpCode::OP_IS_MISSING_ARG, stmt->line, stmt->column);
            emitU16(static_cast<uint16_t>(slot), stmt->line, stmt->column);
            int skipJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line, stmt->column);
            // Replace with default value
            compileExpr(stmt->defaults[i].get());
            emit(OpCode::OP_SET_LOCAL, stmt->line, stmt->column);
            emitU16(static_cast<uint16_t>(slot), stmt->line, stmt->column);
            emit(OpCode::OP_POP, stmt->line, stmt->column);
            patchJump(skipJump);
        }
    }

    // Compile body
    auto* body = static_cast<const BlockStmt*>(stmt->body.get());
    if (body) {
        for (auto& s : body->statements) compileStmt(s.get());
    }

    // Implicit nil return
    emit(OpCode::OP_NIL, stmt->line, stmt->column);
    emit(OpCode::OP_RETURN, stmt->line, stmt->column);

    current = funcState.enclosing;
    fn->upvalueCount = static_cast<int>(funcState.upvalues.size());

    // Create a prototype closure to store in the constant pool.
    // The shared_ptr keeps it alive as long as the constant pool entry exists.
    auto proto = std::make_shared<ObjClosure>(fn);
    auto wrapper = std::make_shared<VMClosureCallable>(proto.get());
    wrapper->ownedPrototype = proto;

    uint16_t fnIdx = currentChunk().addConstant(
        Value(std::static_pointer_cast<Callable>(wrapper)));

    // Emit OP_CLOSURE
    emit(OpCode::OP_CLOSURE, stmt->line, stmt->column);
    emitU16(fnIdx, stmt->line, stmt->column);

    // Emit upvalue descriptors
    for (auto& uv : funcState.upvalues) {
        emit(uv.isLocal ? 1 : 0, stmt->line, stmt->column);
        emitU16(uv.index, stmt->line, stmt->column);
    }

    // Define in current scope
    if (current->scopeDepth > 0) {
        addLocal(stmt->name);
    } else {
        uint16_t nameIdx = identifierConstant(stmt->name);
        emit(OpCode::OP_DEFINE_GLOBAL, stmt->line, stmt->column);
        emitU16(nameIdx, stmt->line, stmt->column);
    }
}

void Compiler::compileReturnStmt(const ReturnStmt* stmt) {
    if (stmt->value) {
        compileExpr(stmt->value.get());
    } else {
        emit(OpCode::OP_NIL, stmt->line, stmt->column);
    }
    // Close any active try handlers before returning from this function
    for (int i = 0; i < current->tryDepth; i++) {
        emit(OpCode::OP_TRY_END, stmt->line, stmt->column);
    }
    // Run any enclosing finally blocks (outermost last)
    for (auto* finallyBody : current->finallyStack) {
        compileStmt(finallyBody);
    }
    emit(OpCode::OP_RETURN, stmt->line, stmt->column);
}

void Compiler::compileBreakStmt(const BreakStmt* stmt) {
    if (current->loops.empty()) {
        error("'break' outside of loop", stmt->line);
        return;
    }
    auto& loop = current->loops.back();
    // Close try handlers that are active inside this loop
    for (int i = loop.tryDepthAtLoop; i < current->tryDepth; i++) {
        emit(OpCode::OP_TRY_END, stmt->line, stmt->column);
    }
    // Pop locals in the loop scope
    for (int i = static_cast<int>(current->locals.size()) - 1; i >= 0; i--) {
        if (current->locals[i].depth <= loop.scopeDepthAtLoop) break;
        emit(OpCode::OP_POP, stmt->line, stmt->column);
    }
    loop.breakJumps.push_back(emitJump(OpCode::OP_JUMP, stmt->line, stmt->column));
}

void Compiler::compileContinueStmt(const ContinueStmt* stmt) {
    if (current->loops.empty()) {
        error("'continue' outside of loop", stmt->line);
        return;
    }
    auto& loop = current->loops.back();
    // Close try handlers that are active inside this loop body
    for (int i = loop.tryDepthAtLoop; i < current->tryDepth; i++) {
        emit(OpCode::OP_TRY_END, stmt->line, stmt->column);
    }
    // Pop locals inside the loop body
    for (int i = static_cast<int>(current->locals.size()) - 1; i >= 0; i--) {
        if (current->locals[i].depth <= loop.scopeDepthAtLoop) break;
        emit(OpCode::OP_POP, stmt->line, stmt->column);
    }
    // Jump forward to the increment section (will be patched later)
    loop.continueJumps.push_back(emitJump(OpCode::OP_JUMP, stmt->line, stmt->column));
}

void Compiler::compileClassStmt(const ClassStmt* stmt) {
    uint16_t nameIdx = identifierConstant(stmt->name);

    emit(OpCode::OP_CLASS, stmt->line, stmt->column);
    emitU16(nameIdx, stmt->line, stmt->column);

    // Define the class name immediately so methods can reference it
    if (current->scopeDepth > 0) {
        addLocal(stmt->name);
    } else {
        emit(OpCode::OP_DEFINE_GLOBAL, stmt->line, stmt->column);
        emitU16(identifierConstant(stmt->name), stmt->line, stmt->column);
    }

    // Inheritance
    if (!stmt->superclass.empty()) {
        // Push superclass (resolve as local first, then global)
        int superSlot = resolveLocal(current, stmt->superclass);
        if (superSlot != -1) {
            emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
            emitU16(static_cast<uint16_t>(superSlot), stmt->line, stmt->column);
        } else {
            int upval = resolveUpvalue(current, stmt->superclass);
            if (upval != -1) {
                emit(OpCode::OP_GET_UPVALUE, stmt->line, stmt->column);
                emitU16(static_cast<uint16_t>(upval), stmt->line, stmt->column);
            } else {
                emit(OpCode::OP_GET_GLOBAL, stmt->line, stmt->column);
                emitU16(identifierConstant(stmt->superclass), stmt->line, stmt->column);
            }
        }

        // Push the class again for INHERIT
        if (current->scopeDepth > 0) {
            int slot = resolveLocal(current, stmt->name);
            emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
            emitU16(static_cast<uint16_t>(slot), stmt->line, stmt->column);
        } else {
            emit(OpCode::OP_GET_GLOBAL, stmt->line, stmt->column);
            emitU16(identifierConstant(stmt->name), stmt->line, stmt->column);
        }

        emit(OpCode::OP_INHERIT, stmt->line, stmt->column);
    }

    // Push class onto stack for METHOD opcodes
    if (current->scopeDepth > 0) {
        int slot = resolveLocal(current, stmt->name);
        emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
        emitU16(static_cast<uint16_t>(slot), stmt->line, stmt->column);
    } else {
        emit(OpCode::OP_GET_GLOBAL, stmt->line, stmt->column);
        emitU16(identifierConstant(stmt->name), stmt->line, stmt->column);
    }

    // Compile each method
    for (auto& method : stmt->methods) {
        auto fn = std::make_shared<CompiledFunction>();
        fn->name = method.name;
        fn->arity = static_cast<int>(method.params.size());
        fn->paramNames = method.params;
        fn->restParam = method.restParam;
        fn->isGenerator = method.isGenerator;

        CompilerState methodState;
        methodState.enclosing = current;
        methodState.function = fn;
        methodState.isGenerator = method.isGenerator;
        methodState.scopeDepth = 0;
        current = &methodState;

        // Slot 0 = "this"
        addLocal("this");

        // Parameters
        for (auto& param : method.params) {
            addLocal(param);
        }

        if (!method.restParam.empty()) {
            addLocal(method.restParam);
        }

        // Default parameter evaluation: if param is nil and default exists, replace it
        for (size_t i = 0; i < method.defaults.size(); i++) {
            if (method.defaults[i]) {
                int slot = static_cast<int>(i) + 1; // +1 for slot 0 (this)
                emit(OpCode::OP_GET_LOCAL, method.line);
                emitU16(static_cast<uint16_t>(slot), method.line);
                emit(OpCode::OP_NIL, method.line);
                emit(OpCode::OP_EQUAL, method.line);
                int skipJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, method.line);
                compileExpr(method.defaults[i].get());
                emit(OpCode::OP_SET_LOCAL, method.line);
                emitU16(static_cast<uint16_t>(slot), method.line);
                emit(OpCode::OP_POP, method.line);
                patchJump(skipJump);
            }
        }

        // Body
        for (auto& s : method.body) {
            compileStmt(s.get());
        }

        // Implicit nil return (init returns this)
        if (method.name == "init") {
            emit(OpCode::OP_GET_LOCAL, method.line);
            emitU16(0, method.line); // slot 0 = this
        } else {
            emit(OpCode::OP_NIL, method.line);
        }
        emit(OpCode::OP_RETURN, method.line);

        current = methodState.enclosing;
        fn->upvalueCount = static_cast<int>(methodState.upvalues.size());

        // Create closure for the method
        auto proto = std::make_shared<ObjClosure>(fn);
        auto wrapper = std::make_shared<VMClosureCallable>(proto.get());
        wrapper->ownedPrototype = proto;
        uint16_t fnIdx = currentChunk().addConstant(
            Value(std::static_pointer_cast<Callable>(wrapper)));

        emit(OpCode::OP_CLOSURE, method.line);
        emitU16(fnIdx, method.line);

        for (auto& uv : methodState.upvalues) {
            emit(uv.isLocal ? 1 : 0, method.line);
            emitU16(uv.index, method.line);
        }

        // Apply decorators and store method
        if (!method.decorators.empty() && method.isStatic) {
            // Static: apply decorators at compile time (no this binding)
            for (int i = static_cast<int>(method.decorators.size()) - 1; i >= 0; i--) {
                compileExpr(method.decorators[i].get());
                emit(OpCode::OP_SWAP, method.line);
                emit(OpCode::OP_CALL, method.line);
                emit(1, method.line);
            }
            emit(OpCode::OP_STATIC_METHOD, method.line);
            emitU16(identifierConstant(method.name), method.line);
        } else if (!method.decorators.empty()) {
            // Instance: store method, then store decorators for runtime application
            emit(OpCode::OP_METHOD, method.line);
            emitU16(identifierConstant(method.name), method.line);
            for (int i = static_cast<int>(method.decorators.size()) - 1; i >= 0; i--) {
                compileExpr(method.decorators[i].get());
                emit(OpCode::OP_METHOD_DECORATOR, method.line);
                emitU16(identifierConstant(method.name), method.line);
            }
        } else {
            emit(method.isStatic ? OpCode::OP_STATIC_METHOD : OpCode::OP_METHOD, method.line);
            emitU16(identifierConstant(method.name), method.line);
        }
    }

    // Pop the class from the stack
    emit(OpCode::OP_POP, stmt->line, stmt->column);
}
void Compiler::compileEnumStmt(const EnumStmt* stmt) {
    // Compile as a map: {Name1: 0, Name2: 1, ...}
    int64_t nextVal = 0;
    int count = 0;
    for (size_t i = 0; i < stmt->members.size(); i++) {
        emitConstant(Value(stmt->members[i]), stmt->line, stmt->column); // key
        if (stmt->values[i]) {
            // Custom value — compile the expression
            compileExpr(stmt->values[i].get());
            // Try to extract compile-time constant for auto-increment tracking
            auto* numExpr = static_cast<const NumberExpr*>(stmt->values[i].get());
            if (numExpr && numExpr->isInt) {
                nextVal = numExpr->intValue + 1;
            } else {
                nextVal++; // best guess if not a constant
            }
        } else {
            emitConstant(Value(nextVal), stmt->line, stmt->column);
            nextVal++;
        }
        count++;
    }
    emit(OpCode::OP_BUILD_MAP, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(count), stmt->line, stmt->column);

    if (current->scopeDepth > 0) {
        addLocal(stmt->name);
    } else {
        emit(OpCode::OP_DEFINE_GLOBAL, stmt->line, stmt->column);
        emitU16(identifierConstant(stmt->name), stmt->line, stmt->column);
    }
}
void Compiler::compileThrowStmt(const ThrowStmt* stmt) {
    compileExpr(stmt->value.get());
    emit(OpCode::OP_THROW, stmt->line, stmt->column);
}

void Compiler::compileTryCatchStmt(const TryCatchStmt* stmt) {
    if (stmt->finallyBody) {
        // Track this finally block so return/break/continue can emit it
        current->finallyStack.push_back(stmt->finallyBody.get());

        // Outer try to guarantee finally runs even if catch throws
        int outerTry = emitJump(OpCode::OP_TRY_BEGIN, stmt->line, stmt->column);
        current->tryDepth++;

        // Inner try/catch
        {
            int tryBegin = emitJump(OpCode::OP_TRY_BEGIN, stmt->line, stmt->column);
            current->tryDepth++;
            compileStmt(stmt->tryBody.get());
            current->tryDepth--;
            emit(OpCode::OP_TRY_END, stmt->line, stmt->column);
            int endJump = emitJump(OpCode::OP_JUMP, stmt->line, stmt->column);

            patchJump(tryBegin);
            beginScope();
            addLocal(stmt->errorVar);
            compileStmt(stmt->catchBody.get());
            endScope(stmt->line);

            patchJump(endJump);
        }

        current->tryDepth--;
        emit(OpCode::OP_TRY_END, stmt->line, stmt->column);

        current->finallyStack.pop_back();

        // Finally after normal completion
        compileStmt(stmt->finallyBody.get());
        int skipJump = emitJump(OpCode::OP_JUMP, stmt->line, stmt->column);

        // Outer catch: run finally then rethrow
        patchJump(outerTry);
        beginScope();
        addLocal("__err__");
        compileStmt(stmt->finallyBody.get());
        emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
        emitU16(static_cast<uint16_t>(resolveLocal(current, "__err__")), stmt->line, stmt->column);
        emit(OpCode::OP_THROW, stmt->line, stmt->column);
        endScope(stmt->line);

        patchJump(skipJump);
    } else {
        int tryBegin = emitJump(OpCode::OP_TRY_BEGIN, stmt->line, stmt->column);
        current->tryDepth++;
        compileStmt(stmt->tryBody.get());
        current->tryDepth--;
        emit(OpCode::OP_TRY_END, stmt->line, stmt->column);
        int endJump = emitJump(OpCode::OP_JUMP, stmt->line, stmt->column);

        patchJump(tryBegin);
        beginScope();
        addLocal(stmt->errorVar);
        compileStmt(stmt->catchBody.get());
        endScope(stmt->line);

        patchJump(endJump);
    }
}

void Compiler::compileEnsureStmt(const EnsureStmt* stmt) {
    if (!stmt->bindingName.empty()) {
        // ensure let <name> = <expr> else { body }
        //
        // Stack-machine shape (mirrors the `??` nil-coalesce
        // pattern in compileExpr): push the value, branch
        // OP_JUMP_IF_NOT_NIL — the value STAYS on the stack on
        // the taken branch — so when we patch the jump to the
        // bind site we have the unwrapped value right where
        // addLocal / OP_DEFINE_GLOBAL expects it.
        //
        // Falls through into the else body iff the value is nil;
        // POP discards the nil first so the stack is balanced
        // before the body's statements run. The body must
        // terminate (return/throw/break/continue) — the parser
        // already enforced that, so we don't need an explicit
        // post-body jump back over the bind path.
        compileExpr(stmt->condition.get());
        int bindJump = emitJump(OpCode::OP_JUMP_IF_NOT_NIL, stmt->line, stmt->column);
        emit(OpCode::OP_POP, stmt->line, stmt->column);  // discard the nil
        compileStmt(stmt->elseBody.get());
        patchJump(bindJump);
        if (current->scopeDepth > 0) {
            addLocal(stmt->bindingName);
        } else {
            emit(OpCode::OP_DEFINE_GLOBAL, stmt->line, stmt->column);
            emitU16(identifierConstant(stmt->bindingName), stmt->line, stmt->column);
        }
        return;
    }

    // ensure (condition) else { body }
    // Compiled as: if (!condition) { body }
    compileExpr(stmt->condition.get());
    int elseJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line, stmt->column);
    int endJump = emitJump(OpCode::OP_JUMP, stmt->line, stmt->column);
    patchJump(elseJump);

    compileStmt(stmt->elseBody.get());

    patchJump(endJump);
}
void Compiler::compileUseStmt(const UseStmt* stmt) {
    // OP_IMPORT [path_idx] [alias_idx]
    // VM resolves, compiles, and executes the grain, pushes the exports map
    uint16_t pathIdx = identifierConstant(stmt->path);
    uint16_t aliasIdx = identifierConstant(stmt->alias);
    emit(OpCode::OP_IMPORT, stmt->line, stmt->column);
    emitU16(pathIdx, stmt->line, stmt->column);
    emitU16(aliasIdx, stmt->line, stmt->column);

    // The import pushes the exports map — define as a variable
    if (current->scopeDepth > 0) {
        addLocal(stmt->alias);
    } else {
        emit(OpCode::OP_DEFINE_GLOBAL, stmt->line, stmt->column);
        emitU16(identifierConstant(stmt->alias), stmt->line, stmt->column);
    }
}

void Compiler::compileExportStmt(const ExportStmt* stmt) {
    // Build a map from exported names, then signal export
    // Push each name as key and its value
    for (auto& name : stmt->names) {
        emitConstant(Value(name), stmt->line, stmt->column); // key

        // Look up the name
        int slot = resolveLocal(current, name);
        if (slot != -1) {
            emit(OpCode::OP_GET_LOCAL, stmt->line, stmt->column);
            emitU16(static_cast<uint16_t>(slot), stmt->line, stmt->column);
        } else {
            emit(OpCode::OP_GET_GLOBAL, stmt->line, stmt->column);
            emitU16(identifierConstant(name), stmt->line, stmt->column);
        }
    }
    emit(OpCode::OP_BUILD_MAP, stmt->line, stmt->column);
    emitU16(static_cast<uint16_t>(stmt->names.size()), stmt->line, stmt->column);
    emit(OpCode::OP_EXPORT, stmt->line, stmt->column);
    emit(static_cast<uint8_t>(0), stmt->line, stmt->column); // unused count byte
}
