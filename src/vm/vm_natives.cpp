#include "vm.h"
#include "../gc_heap.h"
#include "../interpreter.h"
#include "../environment.h"
#include "../unicode.h"
#include <algorithm>
#include <set>

// Call any Callable within the VM context.
// Handles VM closures (re-entrant execution), bound methods, and native functions.
// On error, throws RuntimeError so the calling native (filter/map/sort) propagates
// the error back to OP_CALL's catch, which feeds it into the VM's exception system.
Value callWithVM(VM& vm, std::shared_ptr<Callable> callable, const std::vector<Value>& args) {
    // VM closure — dispatch based on type, not vm pointer (closures from
    // async tasks may have a stale vm but are still valid VMClosureCallable)
    auto* vmcc = dynamic_cast<VMClosureCallable*>(callable.get());
    if (vmcc) {
        int savedFrameCount = vm.frameCount;
        // Slot 0 = the callable itself (functions reference slot 0 for recursion)
        vm.push(Value(callable));
        for (auto& arg : args) vm.push(arg);
        if (!vm.callClosure(vmcc->closure, static_cast<int>(args.size()), 0))
            throw RuntimeError(vm.lastError().empty() ? "Call failed" : vm.lastError(), 0);
        auto result = vm.execute(savedFrameCount);
        if (result != VM::Result::OK)
            throw RuntimeError(vm.lastError().empty() ? "Callback failed" : vm.lastError(), 0);
        return vm.pop();
    }

    // Bound method
    auto* bound = dynamic_cast<VMBoundMethod*>(callable.get());
    if (bound) {
        int savedFrameCount = vm.frameCount;
        vm.push(bound->receiver); // slot 0 = this
        for (auto& arg : args) vm.push(arg);
        if (!vm.callClosure(bound->method, static_cast<int>(args.size()), 0))
            throw RuntimeError(vm.lastError().empty() ? "Call failed" : vm.lastError(), 0);
        vm.frames[vm.frameCount - 1].definingClass = bound->definingClass;
        auto result = vm.execute(savedFrameCount);
        if (result != VM::Result::OK)
            throw RuntimeError(vm.lastError().empty() ? "Callback failed" : vm.lastError(), 0);
        return vm.pop();
    }

    // Native function
    auto* native = dynamic_cast<NativeFunction*>(callable.get());
    if (native) {
        return native->fn(args);
    }

    // Fallback
    Interpreter dummy;
    return callable->call(dummy, args);
}

// Reuse ALL builtins from the tree-walker by creating an Interpreter
// and copying its global environment into the VM.
void vmRegisterNatives(VM& vm) {
    Interpreter interp;

    auto globals = interp.getGlobals();

    std::vector<std::string> globalNames = {
        "print", "len", "push", "pop", "type", "str", "num", "int", "fromCharCode",
        "Lock", "Queue", "Channel", "SharedMap", "CancellationToken",
        "futures", "sort", "filter", "map", "each", "reduce", "any", "all",
        "flatMap", "unique", "zip", "enumerate", "groupBy", "findIndex", "flatten", "keys", "values",
        "sys", "fs", "http", "json", "yaml", "base64", "path", "url", "net",
        "bytes", "crypto", "random", "time", "math", "unicode",
        "loadNative",
#ifdef HAVE_SQLITE
        "sqlite",
#endif
    };

    for (auto& name : globalNames) {
        try {
            Value val = globals->get(name, 0);
            vm.defineNative(name, val);
        } catch (...) {}
    }

    auto makeNat = [](const std::string& name, int arity,
                      std::function<Value(const std::vector<Value>&)> fn) -> Value {
        auto f = std::make_shared<NativeFunction>();
        f->funcName = name;
        f->numArgs = arity;
        f->fn = std::move(fn);
        return Value(std::static_pointer_cast<Callable>(f));
    };

    // Override str() to call toString() on VM instances.
    // Uses VM::current() so it always targets the active VM (safe for async).
    // Helper: find and call a dunder/named method on a VM instance
    auto vmCallInstanceMethod = [](VM* vm, std::shared_ptr<PraiaInstance> inst,
                                    const std::string& methodName,
                                    const std::vector<Value>& args) -> std::pair<bool, Value> {
        auto walk = inst->klass;
        while (walk) {
            auto it = walk->vmMethods.find(methodName);
            if (it != walk->vmMethods.end() && it->second.isCallable()) {
                auto* vmcc = dynamic_cast<VMClosureCallable*>(it->second.asCallable().get());
                if (vmcc) {
                    auto bm = std::make_shared<VMBoundMethod>(Value(inst), vmcc->closure, walk);
                    Value result = callWithVM(*vm, std::static_pointer_cast<Callable>(bm), args);
                    return {true, result};
                }
                break;
            }
            walk = walk->superclass;
        }
        return {false, Value()};
    };

    // Override str() — checks __str, then toString
    vm.defineNative("str", makeNat("str", 1,
        [vmCallInstanceMethod](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            if (args[0].isInstance() && vm) {
                auto inst = args[0].asInstance();
                auto [ok, r] = vmCallInstanceMethod(vm, inst, "__str", {});
                if (ok) return r;
                auto [ok2, r2] = vmCallInstanceMethod(vm, inst, "toString", {});
                if (ok2) return r2;
            }
            return Value(args[0].toString());
        }));

    // Override len() — checks __len
    vm.defineNative("len", makeNat("len", 1,
        [vmCallInstanceMethod](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            if (args[0].isInstance() && vm) {
                auto inst = args[0].asInstance();
                auto [ok, r] = vmCallInstanceMethod(vm, inst, "__len", {});
                if (ok) return r;
            }
            if (args[0].isArray())
                return Value(static_cast<int64_t>(args[0].asArray()->elements.size()));
            if (args[0].isString())
#ifdef HAVE_UTF8PROC
                return Value(static_cast<int64_t>(utf8_grapheme_count(args[0].asString())));
#else
                return Value(static_cast<int64_t>(args[0].asString().size()));
#endif
            if (args[0].isMap())
                return Value(static_cast<int64_t>(args[0].asMap()->entries.size()));
            throw RuntimeError("len() requires an array, string, or map", 0);
        }));

    // Override print() — checks __str/toString on instances
    vm.defineNative("print", makeNat("print", -1,
        [vmCallInstanceMethod](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                if (args[i].isInstance() && vm) {
                    auto inst = args[i].asInstance();
                    auto [ok, r] = vmCallInstanceMethod(vm, inst, "__str", {});
                    if (!ok) std::tie(ok, r) = vmCallInstanceMethod(vm, inst, "toString", {});
                    if (ok) { std::cout << r.toString(); continue; }
                }
                std::cout << args[i].toString();
            }
            std::cout << "\n";
            return Value();
        }));

    // Override higher-order functions with VM-aware versions.
    // Uses VM::current() instead of capturing a VM reference, so these work
    // correctly in async tasks (each async VM sets itself as current).
    vm.defineNative("filter", makeNat("filter", 2,
        [](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            if (!vm) throw RuntimeError("filter() requires VM context", 0);
            if (!args[0].isArray())
                throw RuntimeError("filter() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("filter() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto result = gcNew<PraiaArray>();
            auto pred = args[1].asCallable();
            for (auto& elem : src) {
                Value test = callWithVM(*vm, pred, {elem});
                if (test.isTruthy()) result->elements.push_back(elem);
            }
            return Value(result);
        }));

    vm.defineNative("map", makeNat("map", 2,
        [](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            if (!vm) throw RuntimeError("map() requires VM context", 0);
            if (!args[0].isArray())
                throw RuntimeError("map() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("map() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto result = gcNew<PraiaArray>();
            auto transform = args[1].asCallable();
            for (auto& elem : src)
                result->elements.push_back(callWithVM(*vm, transform, {elem}));
            return Value(result);
        }));

    vm.defineNative("each", makeNat("each", 2,
        [](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            if (!vm) throw RuntimeError("each() requires VM context", 0);
            if (!args[0].isArray())
                throw RuntimeError("each() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("each() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto fn = args[1].asCallable();
            for (auto& elem : src)
                callWithVM(*vm, fn, {elem});
            return args[0];
        }));

    vm.defineNative("sort", makeNat("sort", -1,
        [](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            if (!vm) throw RuntimeError("sort() requires VM context", 0);
            if (args.empty() || !args[0].isArray())
                throw RuntimeError("sort() requires an array", 0);
            auto sorted = gcNew<PraiaArray>();
            sorted->elements = args[0].asArray()->elements;
            auto& elems = sorted->elements;
            if (args.size() > 1 && args[1].isCallable()) {
                auto cmp = args[1].asCallable();
                std::sort(elems.begin(), elems.end(),
                    [vm, &cmp](const Value& a, const Value& b) -> bool {
                        Value result = callWithVM(*vm, cmp, {a, b});
                        if (result.isNumber()) return result.asNumber() < 0;
                        return result.isTruthy();
                    });
            } else {
                std::sort(elems.begin(), elems.end(),
                    [](const Value& a, const Value& b) {
                        if (a.isNumber() && b.isNumber()) return a.asNumber() < b.asNumber();
                        return a.toString() < b.toString();
                    });
            }
            return Value(sorted);
        }));

    vm.defineNative("reduce", makeNat("reduce", -1,
        [](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            if (!vm) throw RuntimeError("reduce() requires VM context", 0);
            if (args.empty() || !args[0].isArray())
                throw RuntimeError("reduce() requires an array as first argument", 0);
            if (args.size() < 2 || !args[1].isCallable())
                throw RuntimeError("reduce() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto fn = args[1].asCallable();
            size_t start = 0;
            Value acc;
            if (args.size() > 2) {
                acc = args[2];
            } else {
                if (src.empty())
                    throw RuntimeError("reduce() of empty array with no initial value", 0);
                acc = src[0];
                start = 1;
            }
            for (size_t i = start; i < src.size(); i++)
                acc = callWithVM(*vm, fn, {acc, src[i]});
            return acc;
        }));

    vm.defineNative("any", makeNat("any", 2,
        [](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            if (!vm) throw RuntimeError("any() requires VM context", 0);
            if (!args[0].isArray())
                throw RuntimeError("any() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("any() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto pred = args[1].asCallable();
            for (auto& elem : src)
                if (callWithVM(*vm, pred, {elem}).isTruthy()) return Value(true);
            return Value(false);
        }));

    vm.defineNative("all", makeNat("all", 2,
        [](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            if (!vm) throw RuntimeError("all() requires VM context", 0);
            if (!args[0].isArray())
                throw RuntimeError("all() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("all() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto pred = args[1].asCallable();
            for (auto& elem : src)
                if (!callWithVM(*vm, pred, {elem}).isTruthy()) return Value(false);
            return Value(true);
        }));

    vm.defineNative("flatMap", makeNat("flatMap", 2,
        [](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            if (!vm) throw RuntimeError("flatMap() requires VM context", 0);
            if (!args[0].isArray())
                throw RuntimeError("flatMap() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("flatMap() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto fn = args[1].asCallable();
            auto result = gcNew<PraiaArray>();
            for (auto& elem : src) {
                Value mapped = callWithVM(*vm, fn, {elem});
                if (mapped.isArray()) {
                    for (auto& inner : mapped.asArray()->elements)
                        result->elements.push_back(inner);
                } else {
                    result->elements.push_back(mapped);
                }
            }
            return Value(result);
        }));

    vm.defineNative("groupBy", makeNat("groupBy", 2,
        [](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            if (!vm) throw RuntimeError("groupBy() requires VM context", 0);
            if (!args[0].isArray())
                throw RuntimeError("groupBy() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("groupBy() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto fn = args[1].asCallable();
            auto result = gcNew<PraiaMap>();
            for (auto& elem : src) {
                Value key = callWithVM(*vm, fn, {elem});
                auto it = result->entries.find(key);
                if (it == result->entries.end()) {
                    auto group = gcNew<PraiaArray>();
                    group->elements.push_back(elem);
                    result->entries[key] = Value(group);
                } else {
                    it->second.asArray()->elements.push_back(elem);
                }
            }
            return Value(result);
        }));

    vm.defineNative("findIndex", makeNat("findIndex", 2,
        [](const std::vector<Value>& args) -> Value {
            VM* vm = VM::current();
            if (!vm) throw RuntimeError("findIndex() requires VM context", 0);
            if (!args[0].isArray())
                throw RuntimeError("findIndex() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("findIndex() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto pred = args[1].asCallable();
            for (size_t i = 0; i < src.size(); i++)
                if (callWithVM(*vm, pred, {src[i]}).isTruthy())
                    return Value(static_cast<int64_t>(i));
            return Value(static_cast<int64_t>(-1));
        }));

    // Convert any iterable to an array for for-in loops.
    // Arrays pass through, maps become [{key, value}, ...], strings become char arrays.
    vm.defineNative("__iterEntries", makeNat("__iterEntries", 1,
        [](const std::vector<Value>& args) -> Value {
            const Value& v = args[0];
            if (v.isGenerator()) return v; // generators are their own iterator
            if (v.isArray()) {
                // Snapshot to protect against mutation during iteration
                auto snapshot = gcNew<PraiaArray>();
                snapshot->elements = v.asArray()->elements;
                return Value(snapshot);
            }
            if (v.isMap()) {
                auto arr = gcNew<PraiaArray>();
                for (auto& [k, val] : v.asMap()->entries) {
                    auto entry = gcNew<PraiaMap>();
                    entry->entries[Value("key")] = Value(k);
                    entry->entries[Value("value")] = val;
                    arr->elements.push_back(Value(entry));
                }
                return Value(arr);
            }
            if (v.isString()) {
                auto arr = gcNew<PraiaArray>();
#ifdef HAVE_UTF8PROC
                for (auto& g : utf8_graphemes(v.asString()))
                    arr->elements.push_back(Value(std::move(g)));
#else
                for (char c : v.asString())
                    arr->elements.push_back(Value(std::string(1, c)));
#endif
                return Value(arr);
            }
            return Value(); // nil for non-iterables
        }));

    // __iterNext(iter, idx) — unified iteration step for for-in loops.
    // For arrays: returns {value: iter[idx], done: false} or {done: true}.
    // For generators: calls .next(), returns {value, done}.
    vm.defineNative("__iterNext", makeNat("__iterNext", 2,
        [](const std::vector<Value>& args) -> Value {
            auto result = gcNew<PraiaMap>();
            if (args[0].isGenerator()) {
                auto gen = args[0].asGenerator();
                auto* vm = VM::current();
                if (!vm) throw RuntimeError("No active VM for generator iteration", 0);
                return vm->resumeGenerator(gen, Value());
            }
            // Array path
            if (args[0].isArray()) {
                auto& elems = args[0].asArray()->elements;
                int idx = static_cast<int>(args[1].asNumber());
                if (idx >= static_cast<int>(elems.size())) {
                    result->entries[Value("value")] = Value();
                    result->entries[Value("done")] = Value(true);
                } else {
                    result->entries[Value("value")] = elems[idx];
                    result->entries[Value("done")] = Value(false);
                }
                return Value(result);
            }
            // Nil or unknown — done
            result->entries[Value("value")] = Value();
            result->entries[Value("done")] = Value(true);
            return Value(result);
        }));

    vm.defineNative("__arraySlice", makeNat("__arraySlice", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray() || !args[1].isNumber()) return Value();
            auto& elems = args[0].asArray()->elements;
            int start = static_cast<int>(args[1].asNumber());
            auto rest = gcNew<PraiaArray>();
            for (int i = start; i < static_cast<int>(elems.size()); i++)
                rest->elements.push_back(elems[i]);
            return Value(rest);
        }));

    vm.defineNative("__mapRest", makeNat("__mapRest", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap() || !args[1].isArray()) return Value();
            auto& entries = args[0].asMap()->entries;
            auto& excludeArr = args[1].asArray()->elements;
            std::set<std::string> exclude;
            for (auto& e : excludeArr) {
                if (e.isString()) exclude.insert(e.asString());
            }
            auto rest = gcNew<PraiaMap>();
            for (auto& [k, v] : entries) {
                if (!k.isString() || !exclude.count(k.asString())) rest->entries[k] = v;
            }
            return Value(rest);
        }));
}
