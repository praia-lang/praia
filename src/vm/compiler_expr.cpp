#include "compiler.h"
#include "vm.h"
#include "../environment.h"
#include "../gc_heap.h"

void Compiler::compileExpr(const Expr* expr) {
    switch (expr->type) {
    case ExprType::Number:    compileNumberExpr(static_cast<const NumberExpr*>(expr)); break;
    case ExprType::String:    compileStringExpr(static_cast<const StringExpr*>(expr)); break;
    case ExprType::Bool:      compileBoolExpr(static_cast<const BoolExpr*>(expr)); break;
    case ExprType::Nil:       compileNilExpr(static_cast<const NilExpr*>(expr)); break;
    case ExprType::Identifier:compileIdentifierExpr(static_cast<const IdentifierExpr*>(expr)); break;
    case ExprType::Assign:    compileAssignExpr(static_cast<const AssignExpr*>(expr)); break;
    case ExprType::Unary:     compileUnaryExpr(static_cast<const UnaryExpr*>(expr)); break;
    case ExprType::Postfix:   compilePostfixExpr(static_cast<const PostfixExpr*>(expr)); break;
    case ExprType::Binary:    compileBinaryExpr(static_cast<const BinaryExpr*>(expr)); break;
    case ExprType::Call:      compileCallExpr(static_cast<const CallExpr*>(expr)); break;
    case ExprType::Ternary:   compileTernaryExpr(static_cast<const TernaryExpr*>(expr)); break;
    case ExprType::Pipe:      compilePipeExpr(static_cast<const PipeExpr*>(expr)); break;
    case ExprType::PipeTry:   compilePipeTryExpr(static_cast<const PipeTryExpr*>(expr)); break;
    case ExprType::Lambda:    compileLambdaExpr(static_cast<const LambdaExpr*>(expr)); break;
    case ExprType::ArrayLiteral: compileArrayLiteralExpr(static_cast<const ArrayLiteralExpr*>(expr)); break;
    case ExprType::MapLiteral:compileMapLiteralExpr(static_cast<const MapLiteralExpr*>(expr)); break;
    case ExprType::Index:     compileIndexExpr(static_cast<const IndexExpr*>(expr)); break;
    case ExprType::IndexAssign:compileIndexAssignExpr(static_cast<const IndexAssignExpr*>(expr)); break;
    case ExprType::Dot:       compileDotExpr(static_cast<const DotExpr*>(expr)); break;
    case ExprType::DotAssign: compileDotAssignExpr(static_cast<const DotAssignExpr*>(expr)); break;
    case ExprType::InterpolatedString: compileInterpolatedStringExpr(static_cast<const InterpolatedStringExpr*>(expr)); break;
    case ExprType::This:      compileThisExpr(static_cast<const ThisExpr*>(expr)); break;
    case ExprType::Super:     compileSuperExpr(static_cast<const SuperExpr*>(expr)); break;
    case ExprType::Async:     compileAsyncExpr(static_cast<const AsyncExpr*>(expr)); break;
    case ExprType::Await:     compileAwaitExpr(static_cast<const AwaitExpr*>(expr)); break;
    case ExprType::Yield:     compileYieldExpr(static_cast<const YieldExpr*>(expr)); break;
    case ExprType::Spread:    error("Unexpected spread expression", expr->line); break;
    default: error("Unknown expression type", expr->line); break;
    }
}

// ── Phase 1: Literals, arithmetic, variables, calls ─────────

void Compiler::compileNumberExpr(const NumberExpr* expr) {
    if (expr->isInt)
        emitConstant(Value(expr->intValue), expr->line, expr->column);
    else
        emitConstant(Value(expr->floatValue), expr->line, expr->column);
}

void Compiler::compileStringExpr(const StringExpr* expr) {
    emitConstant(Value(expr->value), expr->line, expr->column);
}

void Compiler::compileBoolExpr(const BoolExpr* expr) {
    emit(expr->value ? OpCode::OP_TRUE : OpCode::OP_FALSE, expr->line, expr->column);
}

void Compiler::compileNilExpr(const NilExpr* expr) {
    emit(OpCode::OP_NIL, expr->line, expr->column);
}

void Compiler::compileIdentifierExpr(const IdentifierExpr* expr) {
    int slot = resolveLocal(current, expr->name);
    if (slot != -1) {
        emit(OpCode::OP_GET_LOCAL, expr->line, expr->column);
        emitU16(static_cast<uint16_t>(slot), expr->line, expr->column);
    } else {
        int upvalue = resolveUpvalue(current, expr->name);
        if (upvalue != -1) {
            emit(OpCode::OP_GET_UPVALUE, expr->line, expr->column);
            emitU16(static_cast<uint16_t>(upvalue), expr->line, expr->column);
        } else {
            uint16_t nameIdx = identifierConstant(expr->name);
            emit(OpCode::OP_GET_GLOBAL, expr->line, expr->column);
            emitU16(nameIdx, expr->line, expr->column);
        }
    }
}

void Compiler::compileAssignExpr(const AssignExpr* expr) {
    if (Environment::isConstantName(expr->name))
        std::cerr << "[line " << expr->line << ":col " << expr->column
                  << "] Warning: reassigning constant '" << expr->name << "'" << std::endl;

    compileExpr(expr->value.get());

    int slot = resolveLocal(current, expr->name);
    if (slot != -1) {
        emit(OpCode::OP_SET_LOCAL, expr->line, expr->column);
        emitU16(static_cast<uint16_t>(slot), expr->line, expr->column);
    } else {
        int upvalue = resolveUpvalue(current, expr->name);
        if (upvalue != -1) {
            emit(OpCode::OP_SET_UPVALUE, expr->line, expr->column);
            emitU16(static_cast<uint16_t>(upvalue), expr->line, expr->column);
        } else {
            uint16_t nameIdx = identifierConstant(expr->name);
            emit(OpCode::OP_SET_GLOBAL, expr->line, expr->column);
            emitU16(nameIdx, expr->line, expr->column);
        }
    }
}

void Compiler::compileUnaryExpr(const UnaryExpr* expr) {
    compileExpr(expr->operand.get());
    switch (expr->op) {
        case TokenType::MINUS: emit(OpCode::OP_NEGATE, expr->line, expr->column); break;
        case TokenType::NOT:   emit(OpCode::OP_NOT, expr->line, expr->column); break;
        case TokenType::BIT_NOT: emit(OpCode::OP_BIT_NOT, expr->line, expr->column); break;
        default: error("Unknown unary operator", expr->line);
    }
}

void Compiler::compilePostfixExpr(const PostfixExpr* expr) {
    if (expr->operand->type != ExprType::Identifier) { error("Postfix operator requires a variable", expr->line); return; }
    auto* ident = static_cast<const IdentifierExpr*>(expr->operand.get());
    bool inc = expr->op == TokenType::INCREMENT;

    int slot = resolveLocal(current, ident->name);
    if (slot != -1) {
        emit(inc ? OpCode::OP_POST_INC_LOCAL : OpCode::OP_POST_DEC_LOCAL, expr->line, expr->column);
        emitU16(static_cast<uint16_t>(slot), expr->line, expr->column);
        return;
    }

    int upvalue = resolveUpvalue(current, ident->name);
    if (upvalue != -1) {
        emit(inc ? OpCode::OP_POST_INC_UPVALUE : OpCode::OP_POST_DEC_UPVALUE, expr->line, expr->column);
        emitU16(static_cast<uint16_t>(upvalue), expr->line, expr->column);
        return;
    }

    uint16_t nameIdx = identifierConstant(ident->name);
    emit(inc ? OpCode::OP_POST_INC_GLOBAL : OpCode::OP_POST_DEC_GLOBAL, expr->line, expr->column);
    emitU16(nameIdx, expr->line, expr->column);
}

void Compiler::compileBinaryExpr(const BinaryExpr* expr) {
    // Short-circuit for && and ||
    if (expr->op == TokenType::AND) {
        compileExpr(expr->left.get());
        int endJump = emitJump(OpCode::OP_JUMP_IF_FALSE, expr->line, expr->column);
        emit(OpCode::OP_POP, expr->line, expr->column);
        compileExpr(expr->right.get());
        patchJump(endJump);
        return;
    }
    if (expr->op == TokenType::NIL_COALESCE) {
        compileExpr(expr->left.get());
        int endJump = emitJump(OpCode::OP_JUMP_IF_NOT_NIL, expr->line, expr->column);
        emit(OpCode::OP_POP, expr->line, expr->column);
        compileExpr(expr->right.get());
        patchJump(endJump);
        return;
    }
    if (expr->op == TokenType::OR) {
        compileExpr(expr->left.get());
        int endJump = emitJump(OpCode::OP_JUMP_IF_TRUE, expr->line, expr->column);
        emit(OpCode::OP_POP, expr->line, expr->column);
        compileExpr(expr->right.get());
        patchJump(endJump);
        return;
    }

    compileExpr(expr->left.get());
    compileExpr(expr->right.get());

    switch (expr->op) {
        case TokenType::PLUS:    emit(OpCode::OP_ADD, expr->line, expr->column); break;
        case TokenType::MINUS:   emit(OpCode::OP_SUBTRACT, expr->line, expr->column); break;
        case TokenType::STAR:    emit(OpCode::OP_MULTIPLY, expr->line, expr->column); break;
        case TokenType::SLASH:   emit(OpCode::OP_DIVIDE, expr->line, expr->column); break;
        case TokenType::PERCENT: emit(OpCode::OP_MODULO, expr->line, expr->column); break;
        case TokenType::LT:      emit(OpCode::OP_LESS, expr->line, expr->column); break;
        case TokenType::GT:      emit(OpCode::OP_GREATER, expr->line, expr->column); break;
        case TokenType::LTE:     emit(OpCode::OP_LESS_EQUAL, expr->line, expr->column); break;
        case TokenType::GTE:     emit(OpCode::OP_GREATER_EQUAL, expr->line, expr->column); break;
        case TokenType::EQ:      emit(OpCode::OP_EQUAL, expr->line, expr->column); break;
        case TokenType::NEQ:     emit(OpCode::OP_NOT_EQUAL, expr->line, expr->column); break;
        case TokenType::IS:      emit(OpCode::OP_IS, expr->line, expr->column); break;
        case TokenType::BIT_AND: emit(OpCode::OP_BIT_AND, expr->line, expr->column); break;
        case TokenType::BIT_OR:  emit(OpCode::OP_BIT_OR, expr->line, expr->column); break;
        case TokenType::BIT_XOR: emit(OpCode::OP_BIT_XOR, expr->line, expr->column); break;
        case TokenType::SHL:     emit(OpCode::OP_SHL, expr->line, expr->column); break;
        case TokenType::SHR:     emit(OpCode::OP_SHR, expr->line, expr->column); break;
        default: error("Unknown binary operator", expr->line);
    }
}

void Compiler::compileCallExpr(const CallExpr* expr) {
    // Tagged value construction: capitalized identifier that doesn't resolve locally
    if (expr->callee->type == ExprType::Identifier) {
        auto* id = static_cast<const IdentifierExpr*>(expr->callee.get());
        if (!id->name.empty() && std::isupper(id->name[0])) {
            int slot = resolveLocal(current, id->name);
            int upIdx = (slot == -1) ? resolveUpvalue(current, id->name) : -1;
            if (slot == -1 && upIdx == -1) {
                // Not a local/upvalue — could be a global class or a tagged value.
                // Compile args first, then OP_TAG_OR_CALL (tries global, falls back to tagged)
                for (auto& arg : expr->args) compileExpr(arg.get());
                uint16_t nameIdx = currentChunk().addConstant(Value(id->name));
                emit(OpCode::OP_TAG_OR_CALL, expr->line, expr->column);
                emitU16(nameIdx, expr->line, expr->column);
                emit(static_cast<uint8_t>(expr->args.size()), expr->line, expr->column);
                return;
            }
        }
    }

    // Check for spread args
    bool hasSpread = false;
    for (auto& arg : expr->args) {
        if (arg->type == ExprType::Spread) { hasSpread = true; break; }
    }

    compileExpr(expr->callee.get());

    if (hasSpread) {
        emit(OpCode::OP_BUILD_ARRAY, expr->line, expr->column);
        emitU16(0, expr->line, expr->column);

        for (auto& arg : expr->args) {
            if (arg->type == ExprType::Spread) {
                auto* spread = static_cast<const SpreadExpr*>(arg.get());
                compileExpr(spread->expr.get());
                emit(OpCode::OP_ADD, expr->line, expr->column);
            } else {
                compileExpr(arg.get());
                emit(OpCode::OP_BUILD_ARRAY, expr->line, expr->column);
                emitU16(1, expr->line, expr->column);
                emit(OpCode::OP_ADD, expr->line, expr->column);
            }
        }
        emit(OpCode::OP_CALL_SPREAD, expr->line, expr->column);
        return;
    }

    for (auto& arg : expr->args) {
        compileExpr(arg.get());
    }

    // Check if any args are named
    bool hasNamed = false;
    for (auto& n : expr->argNames) { if (!n.empty()) { hasNamed = true; break; } }

    if (hasNamed) {
        auto namesArr = gcNew<PraiaArray>();
        for (auto& n : expr->argNames)
            namesArr->elements.push_back(Value(n));
        uint16_t namesIdx = currentChunk().addConstant(Value(namesArr));
        emit(OpCode::OP_CALL_NAMED, expr->line, expr->column);
        emit(static_cast<uint8_t>(expr->args.size()), expr->line, expr->column);
        emitU16(namesIdx, expr->line, expr->column);
    } else {
        emit(OpCode::OP_CALL, expr->line, expr->column);
        emit(static_cast<uint8_t>(expr->args.size()), expr->line, expr->column);
    }
}

void Compiler::compileTernaryExpr(const TernaryExpr* expr) {
    compileExpr(expr->condition.get());
    int elseJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, expr->line, expr->column);
    compileExpr(expr->thenExpr.get());
    int endJump = emitJump(OpCode::OP_JUMP, expr->line, expr->column);
    patchJump(elseJump);
    compileExpr(expr->elseExpr.get());
    patchJump(endJump);
}

void Compiler::compilePipeExpr(const PipeExpr* expr) {
    // a |> f → f(a)
    // a |> f(x) → f(a, x)
    if (expr->right->type == ExprType::Call) {
        auto* call = static_cast<const CallExpr*>(expr->right.get());
        compileExpr(call->callee.get());
        compileExpr(expr->left.get());  // first arg
        for (auto& arg : call->args) compileExpr(arg.get());

        int totalArgs = static_cast<int>(call->args.size()) + 1;

        // Check if the call has named args
        bool hasNamed = false;
        for (auto& n : call->argNames) { if (!n.empty()) { hasNamed = true; break; } }

        if (hasNamed) {
            // Build names: "" for the piped positional arg, then the call's names
            auto namesArr = gcNew<PraiaArray>();
            namesArr->elements.push_back(Value(std::string("")));
            for (auto& n : call->argNames)
                namesArr->elements.push_back(Value(n));
            uint16_t namesIdx = currentChunk().addConstant(Value(namesArr));
            emit(OpCode::OP_CALL_NAMED, expr->line, expr->column);
            emit(static_cast<uint8_t>(totalArgs), expr->line, expr->column);
            emitU16(namesIdx, expr->line, expr->column);
        } else {
            emit(OpCode::OP_CALL, expr->line, expr->column);
            emit(static_cast<uint8_t>(totalArgs), expr->line, expr->column);
        }
    } else {
        compileExpr(expr->right.get());
        compileExpr(expr->left.get());
        emit(OpCode::OP_CALL, expr->line, expr->column);
        emit(1, expr->line, expr->column);
    }
}

void Compiler::compilePipeTryExpr(const PipeTryExpr* expr) {
    // Compile: try { left } catch (e) { handler(e) }
    // TRY_BEGIN [catch_offset]
    emit(OpCode::OP_TRY_BEGIN, expr->line, expr->column);
    int catchSlot = static_cast<int>(currentChunk().code.size());
    emitU16(0, expr->line, expr->column);
    current->tryDepth++;

    compileExpr(expr->left.get());

    emit(OpCode::OP_TRY_END, expr->line, expr->column);
    current->tryDepth--;

    // Jump over catch block (left succeeded, result is on stack)
    emit(OpCode::OP_JUMP, expr->line, expr->column);
    int jumpOverSlot = static_cast<int>(currentChunk().code.size());
    emitU16(0, expr->line, expr->column);

    // Patch catch offset
    int catchTarget = static_cast<int>(currentChunk().code.size());
    int catchOffset = catchTarget - catchSlot - 2;
    currentChunk().code[catchSlot] = static_cast<uint8_t>((catchOffset >> 8) & 0xFF);
    currentChunk().code[catchSlot + 1] = static_cast<uint8_t>(catchOffset & 0xFF);

    // Catch block: error is on stack. Call handler with it.
    if (expr->right->type == ExprType::Call) {
        auto* call = static_cast<const CallExpr*>(expr->right.get());
        compileExpr(call->callee.get());
        // Swap callee and error so callee is below args
        emit(OpCode::OP_SWAP, expr->line, expr->column);
        for (auto& arg : call->args) compileExpr(arg.get());
        int totalArgs = static_cast<int>(call->args.size()) + 1;
        emit(OpCode::OP_CALL, expr->line, expr->column);
        emit(static_cast<uint8_t>(totalArgs), expr->line, expr->column);
    } else {
        compileExpr(expr->right.get());
        emit(OpCode::OP_SWAP, expr->line, expr->column);
        emit(OpCode::OP_CALL, expr->line, expr->column);
        emit(1, expr->line, expr->column);
    }

    // Patch jump-over
    int afterCatch = static_cast<int>(currentChunk().code.size());
    int jumpDist = afterCatch - jumpOverSlot - 2;
    currentChunk().code[jumpOverSlot] = static_cast<uint8_t>((jumpDist >> 8) & 0xFF);
    currentChunk().code[jumpOverSlot + 1] = static_cast<uint8_t>(jumpDist & 0xFF);
}

// ── Phase 3+ stubs ──────────────────────────────────────────

void Compiler::compileLambdaExpr(const LambdaExpr* expr) {
    auto fn = std::make_shared<CompiledFunction>();
    fn->name = "<lambda>";
    fn->arity = static_cast<int>(expr->params.size());
    fn->paramNames = expr->params;
    fn->restParam = expr->restParam;
    fn->isGenerator = expr->isGenerator;

    CompilerState lamState;
    lamState.enclosing = current;
    lamState.function = fn;
    lamState.isGenerator = expr->isGenerator;
    lamState.scopeDepth = current->scopeDepth + 1;
    current = &lamState;

    addLocal(""); // slot 0

    for (auto& param : expr->params) {
        addLocal(param);
    }

    if (!expr->restParam.empty()) {
        addLocal(expr->restParam);
    }

    // Default parameter evaluation
    for (size_t i = 0; i < expr->defaults.size(); i++) {
        if (expr->defaults[i]) {
            int slot = static_cast<int>(i) + 1;
            emit(OpCode::OP_GET_LOCAL, expr->line, expr->column);
            emitU16(static_cast<uint16_t>(slot), expr->line, expr->column);
            emit(OpCode::OP_NIL, expr->line, expr->column);
            emit(OpCode::OP_EQUAL, expr->line, expr->column);
            int skipJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, expr->line, expr->column);
            compileExpr(expr->defaults[i].get());
            emit(OpCode::OP_SET_LOCAL, expr->line, expr->column);
            emitU16(static_cast<uint16_t>(slot), expr->line, expr->column);
            emit(OpCode::OP_POP, expr->line, expr->column);
            patchJump(skipJump);
        }
    }

    for (auto& s : expr->body) {
        compileStmt(s.get());
    }

    emit(OpCode::OP_NIL, expr->line, expr->column);
    emit(OpCode::OP_RETURN, expr->line, expr->column);

    current = lamState.enclosing;
    fn->upvalueCount = static_cast<int>(lamState.upvalues.size());

    auto proto = std::make_shared<ObjClosure>(fn);
    auto wrapper = std::make_shared<VMClosureCallable>(proto.get());
    wrapper->ownedPrototype = proto;
    uint16_t fnIdx = currentChunk().addConstant(
        Value(std::static_pointer_cast<Callable>(wrapper)));

    emit(OpCode::OP_CLOSURE, expr->line, expr->column);
    emitU16(fnIdx, expr->line, expr->column);

    for (auto& uv : lamState.upvalues) {
        emit(uv.isLocal ? 1 : 0, expr->line, expr->column);
        emitU16(uv.index, expr->line, expr->column);
    }
}
void Compiler::compileArrayLiteralExpr(const ArrayLiteralExpr* expr) {
    bool hasSpreads = false;
    for (auto& elem : expr->elements) {
        if (elem->type == ExprType::Spread) { hasSpreads = true; break; }
    }

    if (!hasSpreads) {
        // Simple case: no spreads, just push elements and build
        for (auto& elem : expr->elements) compileExpr(elem.get());
        emit(OpCode::OP_BUILD_ARRAY, expr->line, expr->column);
        emitU16(static_cast<uint16_t>(expr->elements.size()), expr->line, expr->column);
        return;
    }

    // With spreads: build segments and concat with OP_ADD
    // Strategy: accumulate non-spread elements into arrays, concat spreads
    // Start with an empty array
    emit(OpCode::OP_BUILD_ARRAY, expr->line, expr->column);
    emitU16(0, expr->line, expr->column);

    int pending = 0;
    for (auto& elem : expr->elements) {
        if (elem->type == ExprType::Spread) {
            auto* spread = static_cast<const SpreadExpr*>(elem.get());
            // Flush pending non-spread elements as an array, concat
            if (pending > 0) {
                emit(OpCode::OP_BUILD_ARRAY, expr->line, expr->column);
                emitU16(static_cast<uint16_t>(pending), expr->line, expr->column);
                emit(OpCode::OP_ADD, expr->line, expr->column); // concat with accumulator
                pending = 0;
            }
            // Concat the spread array
            compileExpr(spread->expr.get());
            emit(OpCode::OP_ADD, expr->line, expr->column);
        } else {
            compileExpr(elem.get());
            pending++;
        }
    }
    // Flush remaining
    if (pending > 0) {
        emit(OpCode::OP_BUILD_ARRAY, expr->line, expr->column);
        emitU16(static_cast<uint16_t>(pending), expr->line, expr->column);
        emit(OpCode::OP_ADD, expr->line, expr->column);
    }
}

void Compiler::compileMapLiteralExpr(const MapLiteralExpr* expr) {
    bool hasSpreads = false;
    for (size_t i = 0; i < expr->keys.size(); i++) {
        if (!expr->keys[i] && expr->values[i]->type == ExprType::Spread) {
            hasSpreads = true; break;
        }
    }

    if (!hasSpreads) {
        for (size_t i = 0; i < expr->keys.size(); i++) {
            compileExpr(expr->keys[i].get());
            compileExpr(expr->values[i].get());
        }
        emit(OpCode::OP_BUILD_MAP, expr->line, expr->column);
        emitU16(static_cast<uint16_t>(expr->keys.size()), expr->line, expr->column);
        return;
    }

    // With spreads: build segments and merge with OP_ADD (map + map)
    emit(OpCode::OP_BUILD_MAP, expr->line, expr->column);
    emitU16(0, expr->line, expr->column);

    int pending = 0;
    for (size_t i = 0; i < expr->keys.size(); i++) {
        if (!expr->keys[i] && expr->values[i]->type == ExprType::Spread) {
            if (pending > 0) {
                emit(OpCode::OP_BUILD_MAP, expr->line, expr->column);
                emitU16(static_cast<uint16_t>(pending), expr->line, expr->column);
                emit(OpCode::OP_ADD, expr->line, expr->column);
                pending = 0;
            }
            compileExpr(static_cast<const SpreadExpr*>(expr->values[i].get())->expr.get());
            emit(OpCode::OP_ADD, expr->line, expr->column);
        } else {
            compileExpr(expr->keys[i].get());
            compileExpr(expr->values[i].get());
            pending++;
        }
    }
    if (pending > 0) {
        emit(OpCode::OP_BUILD_MAP, expr->line, expr->column);
        emitU16(static_cast<uint16_t>(pending), expr->line, expr->column);
        emit(OpCode::OP_ADD, expr->line, expr->column);
    }
}

void Compiler::compileIndexExpr(const IndexExpr* expr) {
    compileExpr(expr->object.get());
    compileExpr(expr->index.get());
    emit(expr->isOptional ? OpCode::OP_INDEX_GET_OPT : OpCode::OP_INDEX_GET, expr->line, expr->column);
}

void Compiler::compileIndexAssignExpr(const IndexAssignExpr* expr) {
    compileExpr(expr->object.get());
    compileExpr(expr->index.get());
    compileExpr(expr->value.get());
    emit(OpCode::OP_INDEX_SET, expr->line, expr->column);
}
void Compiler::compileDotExpr(const DotExpr* expr) {
    compileExpr(expr->object.get());
    uint16_t nameIdx = identifierConstant(expr->field);
    emit(expr->isOptional ? OpCode::OP_GET_PROPERTY_OPT : OpCode::OP_GET_PROPERTY, expr->line, expr->column);
    emitU16(nameIdx, expr->line, expr->column);
}

void Compiler::compileDotAssignExpr(const DotAssignExpr* expr) {
    compileExpr(expr->object.get());
    compileExpr(expr->value.get());
    uint16_t nameIdx = identifierConstant(expr->field);
    emit(OpCode::OP_SET_PROPERTY, expr->line, expr->column);
    emitU16(nameIdx, expr->line, expr->column);
}
void Compiler::compileInterpolatedStringExpr(const InterpolatedStringExpr* expr) {
    // Each part is either a StringExpr or an expression — compile them all
    // then emit BUILD_STRING to concatenate
    for (auto& part : expr->parts) {
        compileExpr(part.get());
    }
    emit(OpCode::OP_BUILD_STRING, expr->line, expr->column);
    emitU16(static_cast<uint16_t>(expr->parts.size()), expr->line, expr->column);
}
void Compiler::compileThisExpr(const ThisExpr* expr) {
    // "this" is always local slot 0 in a method
    int slot = resolveLocal(current, "this");
    if (slot != -1) {
        emit(OpCode::OP_GET_LOCAL, expr->line, expr->column);
        emitU16(static_cast<uint16_t>(slot), expr->line, expr->column);
    } else {
        int uv = resolveUpvalue(current, "this");
        if (uv != -1) {
            emit(OpCode::OP_GET_UPVALUE, expr->line, expr->column);
            emitU16(static_cast<uint16_t>(uv), expr->line, expr->column);
        } else {
            error("'this' used outside of a method", expr->line);
        }
    }
}

void Compiler::compileSuperExpr(const SuperExpr* expr) {
    // Push "this" for method binding
    int slot = resolveLocal(current, "this");
    if (slot != -1) {
        emit(OpCode::OP_GET_LOCAL, expr->line, expr->column);
        emitU16(static_cast<uint16_t>(slot), expr->line, expr->column);
    } else {
        int uv = resolveUpvalue(current, "this");
        if (uv != -1) {
            emit(OpCode::OP_GET_UPVALUE, expr->line, expr->column);
            emitU16(static_cast<uint16_t>(uv), expr->line, expr->column);
        } else {
            error("'super' used outside of a method", expr->line);
        }
    }
    uint16_t nameIdx = identifierConstant(expr->method);
    emit(OpCode::OP_GET_SUPER, expr->line, expr->column);
    emitU16(nameIdx, expr->line, expr->column);
}
void Compiler::compileAsyncExpr(const AsyncExpr* expr) {
    // async funcCall(args) — compile the call normally, but use OP_ASYNC instead of OP_CALL
    if (expr->expr->type != ExprType::Call) { error("async requires a function call", expr->line); return; }
    auto* call = static_cast<const CallExpr*>(expr->expr.get());

    compileExpr(call->callee.get());
    for (auto& arg : call->args) compileExpr(arg.get());

    // Check if any args are named
    bool hasNamed = false;
    for (auto& n : call->argNames) { if (!n.empty()) { hasNamed = true; break; } }

    if (hasNamed) {
        auto namesArr = gcNew<PraiaArray>();
        for (auto& n : call->argNames)
            namesArr->elements.push_back(Value(n));
        uint16_t namesIdx = currentChunk().addConstant(Value(namesArr));
        emit(OpCode::OP_ASYNC_NAMED, expr->line, expr->column);
        emit(static_cast<uint8_t>(call->args.size()), expr->line, expr->column);
        emitU16(namesIdx, expr->line, expr->column);
    } else {
        emit(OpCode::OP_ASYNC, expr->line, expr->column);
        emit(static_cast<uint8_t>(call->args.size()), expr->line, expr->column);
    }
}

void Compiler::compileAwaitExpr(const AwaitExpr* expr) {
    compileExpr(expr->expr.get());
    emit(OpCode::OP_AWAIT, expr->line, expr->column);
}

void Compiler::compileYieldExpr(const YieldExpr* expr) {
    if (!current->isGenerator) {
        error("'yield' outside of generator function", expr->line);
        return;
    }
    if (expr->value) {
        compileExpr(expr->value.get());
    } else {
        emit(OpCode::OP_NIL, expr->line, expr->column);
    }
    emit(OpCode::OP_YIELD, expr->line, expr->column);
}
