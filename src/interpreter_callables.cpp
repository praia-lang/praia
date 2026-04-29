#include "fiber.h"
#include "gc_heap.h"
#include "interpreter.h"

// ── PraiaClass / PraiaMethod ──────────────────────────────────

const ClassMethod* PraiaClass::findMethod(const std::string& name) const {
    auto it = methods.find(name);
    if (it != methods.end()) return it->second;
    if (superclass) return superclass->findMethod(name);
    return nullptr;
}

int PraiaClass::arity() const {
    auto* init = findMethod("init");
    if (!init) return 0;
    return init->restParam.empty() ? static_cast<int>(init->params.size()) : -1;
}

Value PraiaClass::call(Interpreter& interp, const std::vector<Value>& args) {
    auto instance = gcNew<PraiaInstance>();
    instance->klass = shared_from_this();

    auto* init = findMethod("init");
    if (init) {
        // Bind and call the init method
        auto method = std::make_shared<PraiaMethod>();
        method->methodName = "init";
        method->params = init->params;
        method->decl = init;
        method->closure = closure;
        method->instance = instance;
        // Find which class in the hierarchy defines init
        auto self = shared_from_this();
        while (self && !self->methods.count("init"))
            self = self->superclass;
        method->definingClass = self ? self : shared_from_this();
        method->astOwner = astOwner;
        method->call(interp, args);
    }

    return Value(instance);
}

Value PraiaMethod::call(Interpreter& interp, const std::vector<Value>& args) {
    auto methodEnv = gcNew<Environment>(closure);
    methodEnv->define("this", instance ? Value(instance) : Value());

    // Store the defining class so super resolves correctly in multi-level inheritance
    if (definingClass && definingClass->superclass) {
        methodEnv->define("__super__", Value(
            std::static_pointer_cast<Callable>(definingClass->superclass)));
    }

    auto prevEnv = interp.env;
    interp.env = methodEnv;
    for (size_t i = 0; i < params.size(); i++) {
        if (i < args.size() && !args[i].isNil()) {
            methodEnv->define(params[i], args[i]);
        } else if (decl && i < decl->defaults.size() && decl->defaults[i]) {
            methodEnv->define(params[i], interp.evaluate(decl->defaults[i].get()));
        } else if (i < args.size()) {
            methodEnv->define(params[i], args[i]);
        } else {
            methodEnv->define(params[i], Value());
        }
    }
    if (decl && !decl->restParam.empty()) {
        auto rest = gcNew<PraiaArray>();
        for (size_t i = params.size(); i < args.size(); i++)
            rest->elements.push_back(args[i]);
        methodEnv->define(decl->restParam, Value(rest));
    }
    try {
        for (const auto& stmt : decl->body)
            interp.execute(stmt.get());
    } catch (const ReturnSignal& ret) {
        interp.env = prevEnv;
        return ret.value;
    } catch (...) {
        interp.env = prevEnv;
        throw;
    }
    interp.env = prevEnv;
    return Value();
}

// ── PraiaFunction / PraiaLambda ───────────────────────────────

Value Interpreter::callBody(std::shared_ptr<Environment> callEnv,
                            const std::vector<std::string>& params,
                            const std::string& restParam,
                            const std::vector<Value>& args,
                            std::function<const Expr*(size_t)> getDefault,
                            std::function<void()> runBody) {
    auto prevEnv = env;
    env = callEnv;

    for (size_t i = 0; i < params.size(); i++) {
        if (i < args.size() && !args[i].isNil()) {
            callEnv->define(params[i], args[i]);
        } else if (auto* def = getDefault(i)) {
            callEnv->define(params[i], evaluate(def));
        } else if (i < args.size()) {
            callEnv->define(params[i], args[i]);
        } else {
            callEnv->define(params[i], Value());
        }
    }
    if (!restParam.empty()) {
        auto rest = gcNew<PraiaArray>();
        for (size_t i = params.size(); i < args.size(); i++)
            rest->elements.push_back(args[i]);
        callEnv->define(restParam, Value(rest));
    }

    deferStacks_.push_back({});
    auto runDefers = [this]() {
        if (deferStacks_.empty()) return;
        auto defers = std::move(deferStacks_.back());
        deferStacks_.pop_back();
        for (int i = static_cast<int>(defers.size()) - 1; i >= 0; i--) {
            try { evaluate(defers[i]); } catch (...) {}
        }
    };

    try {
        runBody();
    } catch (const ReturnSignal& ret) {
        runDefers();
        env = prevEnv;
        return ret.value;
    } catch (...) {
        runDefers();
        env = prevEnv;
        throw;
    }
    runDefers();
    env = prevEnv;
    return Value();
}

Value PraiaLambda::call(Interpreter& interp, const std::vector<Value>& args) {
    auto* e = expr;
    return interp.callBody(gcNew<Environment>(closure), params, restParam, args,
        [e](size_t i) -> const Expr* {
            return (i < e->defaults.size() && e->defaults[i]) ? e->defaults[i].get() : nullptr;
        },
        [&interp, e]() {
            for (const auto& stmt : e->body) interp.execute(stmt.get());
        });
}

Value PraiaFunction::call(Interpreter& interp, const std::vector<Value>& args) {
    auto* d = defaults;
    auto* b = body;
    return interp.callBody(gcNew<Environment>(closure), params, restParam, args,
        [d](size_t i) -> const Expr* {
            return (d && i < d->size() && (*d)[i]) ? (*d)[i].get() : nullptr;
        },
        [&interp, b]() {
            for (const auto& stmt : b->statements) interp.execute(stmt.get());
        });
}

// ── PraiaGenerator destructor ────────────────────────────────
// Defined here because it needs the complete Fiber type.

PraiaGenerator::~PraiaGenerator() {
    if (fiber && !fiber->isCompleted() && state == State::SUSPENDED) {
        // Resume the fiber so it can unwind (yield checks gen->done and throws ReturnSignal)
        done = true;
        try {
            fiber->resume();
        } catch (...) {
            // Suppress exceptions from destructor — the fiber body's error handlers
            // should catch everything, but guard against unexpected exceptions.
        }
    }
}

// ── Generator callables ──────────────────────────────────────
// Calling a generator function returns a PraiaGenerator object.
// The body runs on a fiber (lightweight coroutine), suspended/resumed
// cooperatively via Fiber::suspend() / fiber->resume().

Value makeGeneratorFromEnv(
    std::shared_ptr<Environment> funcEnv,
    std::shared_ptr<Environment> sharedGlobals,
    const std::vector<StmtPtr>& bodyStmts,
    std::shared_ptr<PraiaGenerator> gen) {

    gen->isVM = false;
    gen->state = PraiaGenerator::State::CREATED;
    gen->fiberEnv = funcEnv; // keep environment alive for GC marking

    // Capture raw pointer to avoid reference cycle (gen owns fiber, fiber captures gen)
    PraiaGenerator* genRaw = gen.get();
    const auto* stmtsPtr = &bodyStmts;

    gen->fiber = std::make_unique<Fiber>([genRaw, funcEnv, stmtsPtr, sharedGlobals]() {
        GcHeap::current().disable();
        Interpreter genInterp(sharedGlobals);
        genInterp.env = funcEnv;

        genRaw->state = PraiaGenerator::State::RUNNING;

        try {
            for (const auto& stmt : *stmtsPtr)
                genInterp.execute(stmt.get());
        } catch (const ReturnSignal& ret) {
            genRaw->lastYielded = ret.value;
            genRaw->done = true;
            genRaw->state = PraiaGenerator::State::COMPLETED;
            return;
        } catch (const ThrowSignal& ts) {
            genRaw->errorMessage = ts.value.toString();
            genRaw->done = true;
            genRaw->state = PraiaGenerator::State::COMPLETED;
            return;
        } catch (const RuntimeError& err) {
            genRaw->errorMessage = err.what();
            genRaw->done = true;
            genRaw->state = PraiaGenerator::State::COMPLETED;
            return;
        } catch (...) {
            genRaw->errorMessage = "Generator failed";
            genRaw->done = true;
            genRaw->state = PraiaGenerator::State::COMPLETED;
            return;
        }

        // Body finished without explicit return
        genRaw->lastYielded = Value();
        genRaw->done = true;
        genRaw->state = PraiaGenerator::State::COMPLETED;
    });

    return Value(gen);
}

Value PraiaGeneratorFunction::call(Interpreter& interp, const std::vector<Value>& args) {
    auto funcEnv = gcNew<Environment>(closure);
    auto gen = gcNew<PraiaGenerator>();

    auto prevEnv = interp.env;
    interp.env = funcEnv;
    for (size_t i = 0; i < params.size(); i++) {
        if (i < args.size() && !args[i].isNil()) {
            funcEnv->define(params[i], args[i]);
        } else if (defaults && i < defaults->size() && (*defaults)[i]) {
            funcEnv->define(params[i], interp.evaluate((*defaults)[i].get()));
        } else if (i < args.size()) {
            funcEnv->define(params[i], args[i]);
        } else {
            funcEnv->define(params[i], Value());
        }
    }
    if (!restParam.empty()) {
        auto rest = gcNew<PraiaArray>();
        for (size_t i = params.size(); i < args.size(); i++)
            rest->elements.push_back(args[i]);
        funcEnv->define(restParam, Value(rest));
    }
    interp.env = prevEnv;

    funcEnv->define("__gen__", Value(gen));
    return makeGeneratorFromEnv(funcEnv, interp.globals, body->statements, gen);
}

Value PraiaGeneratorLambda::call(Interpreter& interp, const std::vector<Value>& args) {
    auto funcEnv = gcNew<Environment>(closure);
    auto gen = gcNew<PraiaGenerator>();

    auto prevEnv = interp.env;
    interp.env = funcEnv;
    for (size_t i = 0; i < params.size(); i++) {
        if (i < args.size() && !args[i].isNil()) {
            funcEnv->define(params[i], args[i]);
        } else if (i < expr->defaults.size() && expr->defaults[i]) {
            funcEnv->define(params[i], interp.evaluate(expr->defaults[i].get()));
        } else if (i < args.size()) {
            funcEnv->define(params[i], args[i]);
        } else {
            funcEnv->define(params[i], Value());
        }
    }
    if (!restParam.empty()) {
        auto rest = gcNew<PraiaArray>();
        for (size_t i = params.size(); i < args.size(); i++)
            rest->elements.push_back(args[i]);
        funcEnv->define(restParam, Value(rest));
    }
    interp.env = prevEnv;

    funcEnv->define("__gen__", Value(gen));
    return makeGeneratorFromEnv(funcEnv, interp.globals, expr->body, gen);
}
