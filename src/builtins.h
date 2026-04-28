#pragma once

#include "interpreter.h"
#include "value.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

inline std::string argStr(int n) { return n == 1 ? "argument" : "arguments"; }

// Factory for native (C++) functions exposed as Praia callables.
inline std::shared_ptr<NativeFunction> makeNative(
    const std::string& name, int arity,
    std::function<Value(const std::vector<Value>&)> fn) {
    auto f = std::make_shared<NativeFunction>();
    f->funcName = name;
    f->numArgs = arity;
    f->fn = std::move(fn);
    return f;
}

// Safe callback invocation for tree-walker native code.
// Validates arity and pads missing args with nil before calling, preventing
// crashes from NativeFunction callbacks receiving wrong arg count.
inline Value callSafe(Interpreter& interp, std::shared_ptr<Callable> callable,
                      const std::vector<Value>& args) {
    int arity = callable->arity();
    if (arity != -1) {
        int argc = static_cast<int>(args.size());
        if (argc > arity)
            throw RuntimeError(callable->name() + "() expected at most " +
                std::to_string(arity) + " " + argStr(arity) + " but got " + std::to_string(argc), 0);
        if (argc < arity) {
            std::vector<Value> padded = args;
            padded.resize(arity); // fills with nil (Value default)
            return callable->call(interp, padded);
        }
    }
    return callable->call(interp, args);
}

// ── Builtin registration functions (each in src/builtins/*.cpp) ──
void registerNetBuiltins(std::shared_ptr<PraiaMap> netMap);
void registerBytesBuiltins(std::shared_ptr<PraiaMap> bytesMap);
void registerCryptoBuiltins(std::shared_ptr<PraiaMap> cryptoMap);
void registerConcurrencyBuiltins(Interpreter* self, std::shared_ptr<Environment> globals);

// ── HTTP (builtins/http.cpp) ─────────────────────────────────
Value doHttpRequest(const std::string& method, const std::string& url,
                    const std::string& body,
                    const std::unordered_map<std::string, std::string>& extraHeaders);
void httpServerListen(int port, std::shared_ptr<Callable> handler, Interpreter& interp);

// ── JSON (builtins/json.cpp) ─────────────────────────────────
Value jsonParse(const std::string& src);
std::string jsonStringify(const Value& val, int indent = 0, int depth = 0);

// ── YAML (builtins/yaml.cpp) ─────────────────────────────────
Value yamlParse(const std::string& src);
std::string yamlStringify(const Value& val, int depth = 0);

// ── String / Array dot-method dispatch (builtins/methods.cpp) ─
class VM;
Value getStringMethod(const std::string& str, const std::string& name, int line);
Value getArrayMethod(std::shared_ptr<PraiaArray> arr, const std::string& name, int line,
                     Interpreter* interp = nullptr, VM* vm = nullptr);

