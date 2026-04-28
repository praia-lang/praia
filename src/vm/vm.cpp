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
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

// ── Operator overloading helper ──
// Try to call a dunder method on a VM instance. Returns {true, result} if found.
static std::pair<bool, Value> vmCallDunder(VM& vm, const Value& instance,
                                            const std::string& methodName,
                                            const std::vector<Value>& args) {
    if (!instance.isInstance()) return {false, Value()};
    auto inst = instance.asInstance();
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
    globals[name] = std::move(value);
    builtinNames_.insert(name);
}

void VM::setArgs(const std::vector<std::string>& args) {
    auto arr = gcNew<PraiaArray>();
    for (auto& a : args)
        arr->elements.push_back(Value(a));
    if (globals.count("sys") && globals["sys"].isMap())
        globals["sys"].asMap()->entries[Value("args")] = Value(arr);
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

    // Allow fewer args (defaults to nil) but not more (unless rest param).
    if (!hasRest && argCount > fn->arity) {
        throw RuntimeError(fn->name + "() expected at most " + std::to_string(fn->arity) +
            " " + argStr(fn->arity) + " but got " + std::to_string(argCount), line);
    }

    // Pad missing args with nil
    while (argCount < fn->arity) {
        push(Value());
        argCount++;
    }

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
            Value result = native->fn(args);
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

    // Execute in a fresh VM with only builtins (not user globals)
    VM grainVm;
    for (auto& name : builtinNames_) {
        auto it = globals.find(name);
        if (it != globals.end())
            grainVm.globals[name] = it->second;
    }
    grainVm.builtinNames_ = builtinNames_;
    grainVm.currentFile = resolved;

    grainVm.allClosures.push_back(std::make_unique<ObjClosure>(script));
    auto* grainClosure = grainVm.allClosures.back().get();

    auto wrapper = std::make_shared<VMClosureCallable>(grainClosure);
    grainVm.push(Value(std::static_pointer_cast<Callable>(wrapper)));

    grainVm.frames[0].closure = grainClosure;
    grainVm.frames[0].function = script;
    grainVm.frames[0].ip = script->chunk.code.data();
    grainVm.frames[0].baseSlot = 0;
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

    // Keep the grain's ASTs, closures, and upvalues alive
    grainAsts.push_back(std::move(program));

    // Transfer ownership of closures and upvalues to the parent VM
    for (auto& c : grainVm.allClosures) allClosures.push_back(std::move(c));
    grainVm.allClosures.clear();
    for (auto& u : grainVm.allUpvalues) allUpvalues.push_back(std::move(u));
    grainVm.allUpvalues.clear();

    // Copy grain's globals to parent VM so exported closures can access
    // their module-level variables (which were compiled as globals)
    for (auto& [k, v] : grainVm.globals) {
        // Don't overwrite existing builtins or user globals
        if (globals.find(k) == globals.end()) {
            globals[k] = v;
        }
    }

    // Recursively fix up VM pointers on all grain closures — they were created
    // in grainVm which is about to be destroyed.
    std::function<void(Value&)> fixupValue;
    fixupValue = [this, &grainVm, &fixupValue](Value& v) {
        if (v.isCallable()) {
            auto* vmcc = dynamic_cast<VMClosureCallable*>(v.asCallable().get());
            if (vmcc && (vmcc->vm == &grainVm || vmcc->vm == nullptr))
                vmcc->vm = this;
        }
        if (v.isMap()) {
            for (auto& [k, mv] : v.asMap()->entries) fixupValue(mv);
        }
        if (v.isArray()) {
            for (auto& el : v.asArray()->elements) fixupValue(el);
        }
        if (v.isInstance()) {
            for (auto& [k, fv] : v.asInstance()->fields) fixupValue(fv);
        }
    };
    for (auto& [k, v] : globals) fixupValue(v);
    fixupValue(exports);

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
            if (a.isString() || b.isString()) { push(Value(a.toString() + b.toString())); break; }
            RUNTIME_ERR("Operands of '+' must be numbers, strings, or arrays");
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
                if      (tn == "nil")      result = left.isNil();
                else if (tn == "bool")     result = left.isBool();
                else if (tn == "int")      result = left.isInt();
                else if (tn == "float")    result = left.isDouble();
                else if (tn == "string")   result = left.isString();
                else if (tn == "array")    result = left.isArray();
                else if (tn == "map")      result = left.isMap();
                else if (tn == "function") result = left.isCallable();
                else if (tn == "instance") result = left.isInstance();
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
        case OpCode::OP_LESS:          { Value b=pop(),a=pop(); if(a.isInstance()){auto[ok,r]=vmCallDunder(*this,a,"__lt",{b});if(ok){push(r);break;}} if(a.isString()&&b.isString()){push(Value(a.asString()<b.asString()));break;} if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '<' must be numbers or strings");} push(Value(a.asNumber()<b.asNumber())); break; }
        case OpCode::OP_GREATER:       { Value b=pop(),a=pop(); if(a.isInstance()){auto[ok,r]=vmCallDunder(*this,a,"__gt",{b});if(ok){push(r);break;}} if(a.isString()&&b.isString()){push(Value(a.asString()>b.asString()));break;} if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '>' must be numbers or strings");} push(Value(a.asNumber()>b.asNumber())); break; }
        case OpCode::OP_LESS_EQUAL:    { Value b=pop(),a=pop(); if(a.isInstance()){auto[ok,r]=vmCallDunder(*this,a,"__gt",{b});if(ok){push(Value(!r.isTruthy()));break;}} if(a.isString()&&b.isString()){push(Value(a.asString()<=b.asString()));break;} if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '<=' must be numbers or strings");} push(Value(a.asNumber()<=b.asNumber())); break; }
        case OpCode::OP_GREATER_EQUAL: { Value b=pop(),a=pop(); if(a.isInstance()){auto[ok,r]=vmCallDunder(*this,a,"__lt",{b});if(ok){push(Value(!r.isTruthy()));break;}} if(a.isString()&&b.isString()){push(Value(a.asString()>=b.asString()));break;} if(!a.isNumber()||!b.isNumber()){RUNTIME_ERR("Operands of '>=' must be numbers or strings");} push(Value(a.asNumber()>=b.asNumber())); break; }
        case OpCode::OP_NOT: { push(Value(!pop().isTruthy())); break; }

        // ── Variables ──
        case OpCode::OP_DEFINE_GLOBAL: { std::string n = READ_STRING(); globals[n] = pop(); break; }
        case OpCode::OP_GET_GLOBAL: {
            std::string n = READ_STRING();
            auto it = globals.find(n);
            if (it == globals.end()) { RUNTIME_ERR("Undefined variable '" + n + "'"); }
            push(it->second); break;
        }
        case OpCode::OP_SET_GLOBAL: {
            std::string n = READ_STRING();
            auto it = globals.find(n);
            if (it == globals.end()) { RUNTIME_ERR("Undefined variable '" + n + "'"); }
            it->second = peek(); break;
        }
        case OpCode::OP_GET_LOCAL: { uint16_t slot = READ_U16(); push(stack[FRAME.baseSlot + slot]); break; }
        case OpCode::OP_SET_LOCAL: { uint16_t slot = READ_U16(); stack[FRAME.baseSlot + slot] = peek(); break; }

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
            std::string n = READ_STRING();
            auto it = globals.find(n);
            if (it == globals.end()) { RUNTIME_ERR("Undefined variable '" + n + "'"); }
            if (!it->second.isNumber()) { RUNTIME_ERR("Postfix operator requires a number"); }
            push(it->second);
            bool inc = static_cast<OpCode>(instruction) == OpCode::OP_POST_INC_GLOBAL;
            if (it->second.isInt()) {
                int64_t r; int64_t delta = inc ? 1 : -1;
                bool ov = inc ? __builtin_add_overflow(it->second.asInt(), (int64_t)1, &r)
                              : __builtin_sub_overflow(it->second.asInt(), (int64_t)1, &r);
                if (!ov) it->second = Value(r); else it->second = Value(it->second.asNumber() + delta);
            } else it->second = Value(it->second.asNumber() + (inc ? 1 : -1));
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
            if (!callValue(callee, paramCount, CURRENT_LINE())) return Result::RUNTIME_ERROR;
            break;
        }

        case OpCode::OP_RETURN: {
            Value result = pop();
            int returnBase = FRAME.baseSlot; // save callee's base before popping frame
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

            allClosures.push_back(std::make_unique<ObjClosure>(vmcc->closure->function));
            auto* closure = allClosures.back().get();

            // Read upvalue descriptors
            for (int i = 0; i < closure->upvalueCount; i++) {
                uint8_t isLocal = READ_BYTE();
                uint16_t index = READ_U16();
                if (isLocal) {
                    closure->upvalues[i] = captureUpvalue(&stack[FRAME.baseSlot + index]);
                } else {
                    closure->upvalues[i] = FRAME.closure->upvalues[index];
                }
            }

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

            pop(); // pop superclass
            break;
        }

        case OpCode::OP_GET_PROPERTY_OPT:
        case OpCode::OP_GET_PROPERTY: {
            bool _propOpt = (static_cast<OpCode>(instruction) == OpCode::OP_GET_PROPERTY_OPT);
            std::string name = READ_STRING();
            Value obj = pop();

            if (_propOpt && obj.isNil()) { push(Value()); break; }

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
                if (name == "has" || name == "get") {
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
                if (_propOpt) { push(Value()); break; }
                RUNTIME_ERR("Map has no field '" + name + "'");
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
            // Reserved for future optimization
            RUNTIME_ERR("OP_INVOKE not yet implemented");
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
            if (obj.isArray()) {
                if (!idx.isNumber()) { RUNTIME_ERR("Array index must be a number"); }
                auto& elems = obj.asArray()->elements;
                int i = static_cast<int>(idx.asNumber());
                if (i < 0) i += static_cast<int>(elems.size());
                if (i < 0 || i >= static_cast<int>(elems.size())) { RUNTIME_ERR("Array index out of bounds"); }
                push(elems[i]);
            } else if (obj.isString()) {
                if (!idx.isNumber()) { RUNTIME_ERR("String index must be a number"); }
                auto& str = obj.asString();
                int i = static_cast<int>(idx.asNumber());
#ifdef HAVE_UTF8PROC
                auto gs = utf8_graphemes(str);
                int slen = static_cast<int>(gs.size());
                if (i < 0) i += slen;
                if (i < 0 || i >= slen) { RUNTIME_ERR("String index out of bounds"); }
                push(Value(gs[i]));
#else
                if (i < 0) i += static_cast<int>(str.size());
                if (i < 0 || i >= static_cast<int>(str.size())) { RUNTIME_ERR("String index out of bounds"); }
                push(Value(std::string(1, str[i])));
#endif
            } else if (obj.isMap()) {
                auto& entries = obj.asMap()->entries;
                auto it = entries.find(idx);
                if (it == entries.end()) { RUNTIME_ERR("Map has no key '" + idx.toString() + "'"); }
                push(it->second);
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
            // Not yet fully implemented for VM
            RUNTIME_ERR("Spread in VM not yet supported");
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

            /* Deep-copy heap-allocated values so the async task doesn't share
            the caller's mutable state. Handles PraiaMap, PraiaArray,
            PraiaInstance (clones fields), and VMClosureCallable (clones
            wrapper to prevent dangling vm pointers). NativeFunction and
            PraiaFuture are safe to share. Primitives/strings copy by value.
            Track visited heap objects to handle cycles (e.g. a = []; a.push(a)).
            Key: raw pointer of the original object. Value: its already-created copy. */
            std::unordered_map<void*, Value> visited;

            std::function<Value(const Value&)> deepCopy;
            deepCopy = [&deepCopy, &visited](const Value& v) -> Value {
                if (v.isCallable()) {
                    auto* vmcc = dynamic_cast<VMClosureCallable*>(v.asCallable().get());
                    if (vmcc) {
                        auto clone = std::make_shared<VMClosureCallable>(vmcc->closure);
                        clone->ownedPrototype = vmcc->ownedPrototype;
                        clone->vm = nullptr; // rewire sets this to &taskVm
                        return Value(std::static_pointer_cast<Callable>(clone));
                    }
                    auto* bm = dynamic_cast<VMBoundMethod*>(v.asCallable().get());
                    if (bm) {
                        Value recvCopy = deepCopy(bm->receiver);
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
                        copy->entries[k] = deepCopy(val);
                    return result;
                }
                if (v.isArray()) {
                    void* key = static_cast<void*>(v.asArray().get());
                    auto it = visited.find(key);
                    if (it != visited.end()) return it->second;
                    auto copy = gcNew<PraiaArray>();
                    Value result(copy);
                    visited[key] = result; // register before recursing
                    for (auto& el : v.asArray()->elements)
                        copy->elements.push_back(deepCopy(el));
                    return result;
                }
                if (v.isInstance()) {
                    void* key = static_cast<void*>(v.asInstance().get());
                    auto it = visited.find(key);
                    if (it != visited.end()) return it->second;
                    auto copy = gcNew<PraiaInstance>();
                    copy->klass = v.asInstance()->klass; // share class (immutable)
                    Value result(copy);
                    visited[key] = result; // register before recursing
                    for (auto& [k, fv] : v.asInstance()->fields)
                        copy->fields[k] = deepCopy(fv);
                    return result;
                }
                return v; // primitives, strings, futures
            };

            // Deep-copy args so async task can't mutate caller's objects
            for (auto& arg : args)
                arg = deepCopy(arg);

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

                std::unordered_map<std::string, Value> globalsCopy;
                for (auto& [k, v] : globals)
                    globalsCopy[k] = deepCopy(v);

                // Snapshot upvalues and deep-copy so captured arrays/maps/instances
                // are isolated from the caller
                std::vector<Value> upvalueSnapshot;
                for (int i = 0; i < closureSrc->upvalueCount; i++) {
                    auto* uv = closureSrc->upvalues[i];
                    upvalueSnapshot.push_back(uv ? deepCopy(*uv->location) : Value());
                }

                // For bound methods, deep-copy the receiver so the task
                // doesn't share instance fields with the caller.
                // For constructors, the instance is created fresh in the task.
                Value receiverCopy = bound ? deepCopy(bound->receiver) : Value();
                auto defClass = bound ? bound->definingClass
                              : initVmcc ? initOwner : nullptr;

                sharedFuture = std::async(std::launch::async,
                    [fn, args, globalsCopy = std::move(globalsCopy), arity,
                     upvalueSnapshot = std::move(upvalueSnapshot),
                     builtinNames = builtinNames_,
                     receiverCopy = std::move(receiverCopy),
                     defClass = std::move(defClass),
                     isConstructor, klass]() mutable -> Value {
                        VM taskVm;
                        GcHeap::current().disable(); // task VMs are short-lived
                        taskVm.globals = std::move(globalsCopy);
                        taskVm.builtinNames_ = std::move(builtinNames);
                        taskVm.suppressErrors_ = true; // errors propagate to await, not stderr

                        // Recursively rewire VMClosureCallable vm pointers to taskVm.
                        // Track visited objects to handle cycles.
                        std::unordered_set<void*> rewireVisited;
                        std::function<void(Value&)> rewire;
                        rewire = [&taskVm, &rewire, &rewireVisited](Value& v) {
                            if (v.isCallable()) {
                                auto* vc = dynamic_cast<VMClosureCallable*>(v.asCallable().get());
                                if (vc) vc->vm = &taskVm;
                            }
                            if (v.isMap()) {
                                void* p = static_cast<void*>(v.asMap().get());
                                if (rewireVisited.insert(p).second)
                                    for (auto& [mk, mv] : v.asMap()->entries) rewire(mv);
                            }
                            if (v.isArray()) {
                                void* p = static_cast<void*>(v.asArray().get());
                                if (rewireVisited.insert(p).second)
                                    for (auto& el : v.asArray()->elements) rewire(el);
                            }
                            if (v.isInstance()) {
                                void* p = static_cast<void*>(v.asInstance().get());
                                if (rewireVisited.insert(p).second)
                                    for (auto& [k, fv] : v.asInstance()->fields) rewire(fv);
                            }
                        };
                        for (auto& [k, v] : taskVm.globals) rewire(v);

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
                sharedFuture = std::async(std::launch::async,
                    [native, args]() -> Value {
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
    for (auto& [k, v] : globals)
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
}
