#include "builtins.h"
#include "builtins/scope_guards.h"
#include "gc_heap.h"
#include "grain_resolve.h"
#include "interpreter.h"
#include "unicode.h"
#include "vm/vm.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <thread>
#ifdef HAVE_SQLITE
#include <sqlite3.h>
#endif
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <poll.h>
#include <unistd.h>
#include <csignal>
#include <atomic>
#include <dlfcn.h>

namespace fs = std::filesystem;

// ── Signal handling infrastructure ──
// Defined here, declared extern in signal_state.h for use by interpreter.cpp and vm.cpp.
#include "signal_state.h"
std::mutex g_signalMutex;
std::unordered_map<int, std::shared_ptr<Callable>> g_signalHandlers;
std::atomic<uint32_t> g_pendingSignals{0};

// ── Plugin loading infrastructure ──
static std::mutex g_pluginMutex;
static std::unordered_map<std::string, std::shared_ptr<PraiaMap>> g_pluginCache;
static std::vector<void*> g_pluginHandles; // never dlclose'd — function pointers must stay valid

static int signalNameToNum(const std::string& name) {
    if (name == "SIGINT"  || name == "INT")  return SIGINT;
    if (name == "SIGTERM" || name == "TERM") return SIGTERM;
    if (name == "SIGKILL" || name == "KILL") return SIGKILL;
    if (name == "SIGHUP"  || name == "HUP")  return SIGHUP;
    if (name == "SIGQUIT" || name == "QUIT") return SIGQUIT;
    if (name == "SIGUSR1" || name == "USR1") return SIGUSR1;
    if (name == "SIGUSR2" || name == "USR2") return SIGUSR2;
    if (name == "SIGSTOP" || name == "STOP") return SIGSTOP;
    if (name == "SIGCONT" || name == "CONT") return SIGCONT;
    if (name == "SIGPIPE" || name == "PIPE") return SIGPIPE;
    if (name == "SIGCHLD" || name == "CHLD") return SIGCHLD;
    return -1;
}

static std::string signalNumToName(int sig) {
    switch (sig) {
        case SIGINT:  return "SIGINT";
        case SIGTERM: return "SIGTERM";
        case SIGKILL: return "SIGKILL";
        case SIGHUP:  return "SIGHUP";
        case SIGQUIT: return "SIGQUIT";
        case SIGUSR1: return "SIGUSR1";
        case SIGUSR2: return "SIGUSR2";
        case SIGSTOP: return "SIGSTOP";
        case SIGCONT: return "SIGCONT";
        case SIGPIPE: return "SIGPIPE";
        case SIGCHLD: return "SIGCHLD";
        default: return "SIG" + std::to_string(sig);
    }
}

void praiaSignalHandler(int sig) {
    // Async-signal-safe: only set atomic flag
    if (sig >= 0 && sig < 32)
        g_pendingSignals.fetch_or(1u << sig);
}

void installDefaultSignalHandlers() {
    struct sigaction sa = {};
    sa.sa_handler = praiaSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
}

Interpreter::Interpreter() {
    globals = gcNew<Environment>();
    env = globals;
    Interpreter* self = this;

    // ── Global functions ──

    globals->define("print", Value(makeNative("print", -1,
        [self](const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                if (args[i].isInstance() && args[i].asInstance()->klass) {
                    auto inst = args[i].asInstance();
                    auto* method = inst->klass->findMethod("__str");
                    if (!method) method = inst->klass->findMethod("toString");
                    if (method) {
                        auto bound = std::make_shared<PraiaMethod>();
                        bound->methodName = method->name;
                        bound->params = method->params;
                        bound->decl = method;
                        bound->instance = inst;
                        auto walk = inst->klass;
                        while (walk && !walk->methods.count(method->name))
                            walk = walk->superclass;
                        bound->definingClass = walk ? walk : inst->klass;
                        bound->closure = bound->definingClass->closure;
                        std::cout << bound->call(*self, {}).toString();
                        continue;
                    }
                }
                std::cout << args[i].toString();
            }
            std::cout << "\n";
            return Value();
        })));

    globals->define("len", Value(makeNative("len", 1,
        [self](const std::vector<Value>& args) -> Value {
            if (args[0].isInstance()) {
                auto inst = args[0].asInstance();
                auto* decl = inst->klass->findMethod("__len");
                if (decl) {
                    auto bound = std::make_shared<PraiaMethod>();
                    bound->methodName = "__len";
                    bound->params = decl->params;
                    bound->decl = decl;
                    bound->instance = inst;
                    auto walk = inst->klass;
                    while (walk && !walk->methods.count("__len"))
                        walk = walk->superclass;
                    bound->definingClass = walk ? walk : inst->klass;
                    bound->closure = bound->definingClass->closure;
                    return bound->call(*self, {});
                }
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
        })));

    globals->define("push", Value(makeNative("push", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("push() requires an array as first argument", 0);
            args[0].asArray()->elements.push_back(args[1]);
            return Value();
        })));

    globals->define("pop", Value(makeNative("pop", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("pop() requires an array", 0);
            auto& elems = args[0].asArray()->elements;
            if (elems.empty())
                throw RuntimeError("pop() on empty array", 0);
            Value last = elems.back();
            elems.pop_back();
            return last;
        })));

    globals->define("type", Value(makeNative("type", 1,
        [](const std::vector<Value>& args) -> Value {
            auto& v = args[0];
            if (v.isNil())      return Value("nil");
            if (v.isBool())     return Value("bool");
            if (v.isInt())      return Value("int");
            if (v.isDouble())   return Value("float");
            if (v.isString())   return Value("string");
            if (v.isArray())    return Value("array");
            if (v.isMap())      return Value("map");
            if (v.isInstance()) return Value("instance");
            if (v.isTagged())   return Value("tagged");
            if (v.isFuture())   return Value("future");
            if (v.isGenerator()) return Value("generator");
            if (v.isCallable()) return Value("function");
            return Value("unknown");
        })));

    globals->define("str", Value(makeNative("str", 1,
        [self](const std::vector<Value>& args) -> Value {
            if (args[0].isInstance()) {
                auto inst = args[0].asInstance();
                if (inst->klass) {
                    // Check __str first, then toString (backwards compat)
                    auto* method = inst->klass->findMethod("__str");
                    if (!method) method = inst->klass->findMethod("toString");
                    if (method) {
                        auto bound = std::make_shared<PraiaMethod>();
                        bound->methodName = method == inst->klass->findMethod("__str") ? "__str" : "toString";
                        bound->params = method->params;
                        bound->decl = method;
                        bound->instance = inst;
                        auto walk = inst->klass;
                        std::string mname = bound->methodName;
                        while (walk && !walk->methods.count(mname))
                            walk = walk->superclass;
                        bound->definingClass = walk ? walk : inst->klass;
                        bound->closure = bound->definingClass->closure;
                        return bound->call(*self, {});
                    }
                }
            }
            return Value(args[0].toString());
        })));

    globals->define("num", Value(makeNative("num", 1,
        [](const std::vector<Value>& args) -> Value {
            if (args[0].isNumber()) return args[0];
            if (args[0].isString()) {
                try { return Value(std::stod(args[0].asString())); }
                catch (...) {
                    throw RuntimeError("num(): cannot parse \"" + args[0].asString() +
                                       "\" as a number", 0);
                }
            }
            throw RuntimeError("num(): cannot convert " + args[0].toString() +
                               " (" + std::string(
                                   args[0].isBool()     ? "bool"
                                 : args[0].isNil()      ? "nil"
                                 : args[0].isArray()    ? "array"
                                 : args[0].isMap()      ? "map"
                                 : args[0].isInstance() ? "instance"
                                 : args[0].isCallable() ? "function"
                                                        : "unknown") + ") to a number", 0);
        })));

    // int(x) — convert to int64. Useful for keeping precision in mixed
    // arithmetic: `bigInt + int(1.0)` stays int instead of routing
    // through double. Companion to `num()`.
    //   int(int)         → unchanged
    //   int(double)      → truncated toward zero; throws on NaN/inf or
    //                      values outside [INT64_MIN, INT64_MAX]
    //   int(bool)        → 1 / 0
    //   int("123")       → 123 (whole-number string parse)
    //   int("3.14")      → 3 (parses as double, truncates)
    //   int(otherwise)   → error
    globals->define("int", Value(makeNative("int", 1,
        [](const std::vector<Value>& args) -> Value {
            const Value& v = args[0];
            if (v.isInt()) return v;
            if (v.isBool()) return Value(static_cast<int64_t>(v.asBool() ? 1 : 0));
            if (v.isDouble()) {
                double d = v.asNumber();
                if (std::isnan(d))
                    throw RuntimeError("int(): cannot convert NaN", 0);
                if (!std::isfinite(d))
                    throw RuntimeError("int(): cannot convert infinity", 0);
                // i64 range check. The upper bound is exclusive in double
                // because 2^63 is exactly representable as double but is
                // one past INT64_MAX. The lower bound is inclusive — -2^63
                // exactly representable and equals INT64_MIN.
                if (d >= 9223372036854775808.0 || d < -9223372036854775808.0)
                    throw RuntimeError("int(): " + v.toString() +
                                       " out of int64 range", 0);
                return Value(static_cast<int64_t>(std::trunc(d)));
            }
            if (v.isString()) {
                const std::string& s = v.asString();
                // Try whole-number parse first (preserves int64 precision
                // for big values that wouldn't round-trip through double).
                try {
                    size_t pos = 0;
                    long long n = std::stoll(s, &pos);
                    if (pos == s.size())
                        return Value(static_cast<int64_t>(n));
                    // Trailing content — fall through to double parse.
                } catch (...) {
                    // Fall through to double parse.
                }
                try {
                    size_t pos = 0;
                    double d = std::stod(s, &pos);
                    if (pos != s.size())
                        throw RuntimeError("int(): cannot parse \"" + s +
                                           "\" as an int", 0);
                    if (std::isnan(d) || !std::isfinite(d))
                        throw RuntimeError("int(): cannot parse \"" + s +
                                           "\" as an int", 0);
                    if (d >= 9223372036854775808.0 || d < -9223372036854775808.0)
                        throw RuntimeError("int(): \"" + s +
                                           "\" out of int64 range", 0);
                    return Value(static_cast<int64_t>(std::trunc(d)));
                } catch (const RuntimeError&) {
                    throw;
                } catch (...) {
                    throw RuntimeError("int(): cannot parse \"" + s +
                                       "\" as an int", 0);
                }
            }
            throw RuntimeError("int(): cannot convert " + v.toString() +
                               " (" + std::string(
                                   v.isNil()      ? "nil"
                                 : v.isArray()    ? "array"
                                 : v.isMap()      ? "map"
                                 : v.isInstance() ? "instance"
                                 : v.isCallable() ? "function"
                                                  : "unknown") + ") to int", 0);
        })));

    globals->define("fromCharCode", Value(makeNative("fromCharCode", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("fromCharCode() requires a number", 0);
            int32_t cp = static_cast<int32_t>(args[0].asNumber());
#ifdef HAVE_UTF8PROC
            if (cp < 0 || cp > 0x10FFFF)
                throw RuntimeError("fromCharCode() codepoint out of range (0-0x10FFFF)", 0);
            return Value(utf8_from_codepoint(cp));
#else
            if (cp < 0 || cp > 255)
                throw RuntimeError("fromCharCode() value out of range (0-255)", 0);
            return Value(std::string(1, static_cast<char>(cp)));
#endif
        })));


    // ── Concurrency (Lock, Channel, futures) — builtins/concurrency.cpp ──
    registerConcurrencyBuiltins(self, globals);

    // ── Functional built-ins (work great with |>) ──

    globals->define("sort", Value(makeNative("sort", -1,
        [this](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isArray())
                throw RuntimeError("sort() requires an array", 0);
            auto sorted = gcNew<PraiaArray>();
            sorted->elements = args[0].asArray()->elements;
            auto& elems = sorted->elements;

            if (args.size() > 1 && args[1].isCallable()) {
                auto cmp = args[1].asCallable();
                Interpreter* interp = this;
                std::sort(elems.begin(), elems.end(),
                    [interp, &cmp](const Value& a, const Value& b) -> bool {
                        Value result = callSafe(*interp, cmp, {a, b});
                        if (result.isNumber()) return result.asNumber() < 0;
                        return result.isTruthy();
                    });
            } else {
                std::sort(elems.begin(), elems.end(), [](const Value& a, const Value& b) {
                    if (a.isNumber() && b.isNumber()) return a.asNumber() < b.asNumber();
                    return a.toString() < b.toString();
                });
            }
            return Value(sorted);
        })));

    globals->define("filter", Value(makeNative("filter", 2,
        [this](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("filter() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("filter() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto result = gcNew<PraiaArray>();
            auto pred = args[1].asCallable();
            for (auto& elem : src) {
                Value test = callSafe(*this, pred, {elem});
                if (test.isTruthy()) result->elements.push_back(elem);
            }
            return Value(result);
        })));

    globals->define("map", Value(makeNative("map", 2,
        [this](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("map() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("map() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto result = gcNew<PraiaArray>();
            auto transform = args[1].asCallable();
            for (auto& elem : src)
                result->elements.push_back(callSafe(*this, transform, {elem}));
            return Value(result);
        })));

    globals->define("each", Value(makeNative("each", 2,
        [this](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("each() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("each() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto fn = args[1].asCallable();
            for (auto& elem : src)
                callSafe(*this, fn, {elem});
            return args[0]; // return the array for chaining
        })));

    globals->define("reduce", Value(makeNative("reduce", -1,
        [this](const std::vector<Value>& args) -> Value {
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
                acc = callSafe(*this, fn, {acc, src[i]});
            return acc;
        })));

    globals->define("any", Value(makeNative("any", 2,
        [this](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("any() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("any() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto pred = args[1].asCallable();
            for (auto& elem : src)
                if (callSafe(*this, pred, {elem}).isTruthy()) return Value(true);
            return Value(false);
        })));

    globals->define("all", Value(makeNative("all", 2,
        [this](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("all() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("all() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto pred = args[1].asCallable();
            for (auto& elem : src)
                if (!callSafe(*this, pred, {elem}).isTruthy()) return Value(false);
            return Value(true);
        })));

    globals->define("flatMap", Value(makeNative("flatMap", 2,
        [this](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("flatMap() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("flatMap() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto fn = args[1].asCallable();
            auto result = gcNew<PraiaArray>();
            for (auto& elem : src) {
                Value mapped = callSafe(*this, fn, {elem});
                if (mapped.isArray()) {
                    for (auto& inner : mapped.asArray()->elements)
                        result->elements.push_back(inner);
                } else {
                    result->elements.push_back(mapped);
                }
            }
            return Value(result);
        })));

    globals->define("unique", Value(makeNative("unique", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("unique() requires an array", 0);
            auto& src = args[0].asArray()->elements;
            auto result = gcNew<PraiaArray>();
            // Use a set of string representations for dedup
            // (Value equality works but unordered_set needs ValueHash)
            std::unordered_set<Value, ValueHash> seen;
            for (auto& elem : src) {
                if (seen.insert(elem).second)
                    result->elements.push_back(elem);
            }
            return Value(result);
        })));

    globals->define("zip", Value(makeNative("zip", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray() || !args[1].isArray())
                throw RuntimeError("zip() requires two arrays", 0);
            auto& a = args[0].asArray()->elements;
            auto& b = args[1].asArray()->elements;
            size_t len = std::min(a.size(), b.size());
            auto result = gcNew<PraiaArray>();
            for (size_t i = 0; i < len; i++) {
                auto pair = gcNew<PraiaArray>();
                pair->elements.push_back(a[i]);
                pair->elements.push_back(b[i]);
                result->elements.push_back(Value(pair));
            }
            return Value(result);
        })));

    globals->define("enumerate", Value(makeNative("enumerate", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("enumerate() requires an array", 0);
            auto& src = args[0].asArray()->elements;
            auto result = gcNew<PraiaArray>();
            for (size_t i = 0; i < src.size(); i++) {
                auto pair = gcNew<PraiaArray>();
                pair->elements.push_back(Value(static_cast<int64_t>(i)));
                pair->elements.push_back(src[i]);
                result->elements.push_back(Value(pair));
            }
            return Value(result);
        })));

    globals->define("groupBy", Value(makeNative("groupBy", 2,
        [this](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("groupBy() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("groupBy() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto fn = args[1].asCallable();
            auto result = gcNew<PraiaMap>();
            for (auto& elem : src) {
                Value key = callSafe(*this, fn, {elem});
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
        })));

    globals->define("findIndex", Value(makeNative("findIndex", 2,
        [this](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("findIndex() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("findIndex() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto pred = args[1].asCallable();
            for (size_t i = 0; i < src.size(); i++)
                if (callSafe(*this, pred, {src[i]}).isTruthy())
                    return Value(static_cast<int64_t>(i));
            return Value(static_cast<int64_t>(-1));
        })));

    globals->define("flatten", Value(makeNative("flatten", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("flatten() requires an array", 0);
            auto& src = args[0].asArray()->elements;
            auto result = gcNew<PraiaArray>();
            for (auto& elem : src) {
                if (elem.isArray()) {
                    for (auto& inner : elem.asArray()->elements)
                        result->elements.push_back(inner);
                } else {
                    result->elements.push_back(elem);
                }
            }
            return Value(result);
        })));

    globals->define("keys", Value(makeNative("keys", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap())
                throw RuntimeError("keys() requires a map", 0);
            auto result = gcNew<PraiaArray>();
            for (auto& [k, v] : args[0].asMap()->entries)
                result->elements.push_back(k);
            return Value(result);
        })));

    globals->define("values", Value(makeNative("values", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap())
                throw RuntimeError("values() requires a map", 0);
            auto result = gcNew<PraiaArray>();
            for (auto& [k, v] : args[0].asMap()->entries)
                result->elements.push_back(v);
            return Value(result);
        })));

    // ── sys namespace ──

    sysMap = gcNew<PraiaMap>();

    sysMap->entries[Value("read")] = Value(makeNative("sys.read", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.read() requires a string path", 0);
            std::ifstream f(args[0].asString());
            if (!f.is_open())
                throw RuntimeError("Cannot read file: " + args[0].asString(), 0);
            std::stringstream ss;
            ss << f.rdbuf();
            return Value(ss.str());
        }));

    sysMap->entries[Value("write")] = Value(makeNative("sys.write", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.write() requires a string path", 0);
            std::ofstream f(args[0].asString());
            if (!f.is_open())
                throw RuntimeError("Cannot write file: " + args[0].asString(), 0);
            f << args[1].toString();
            return Value();
        }));

    sysMap->entries[Value("append")] = Value(makeNative("sys.append", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.append() requires a string path", 0);
            std::ofstream f(args[0].asString(), std::ios::app);
            if (!f.is_open())
                throw RuntimeError("Cannot open file: " + args[0].asString(), 0);
            f << args[1].toString();
            return Value();
        }));

    sysMap->entries[Value("exists")] = Value(makeNative("sys.exists", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.exists() requires a string path", 0);
            return Value(fs::exists(args[0].asString()));
        }));

    sysMap->entries[Value("mkdir")] = Value(makeNative("sys.mkdir", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.mkdir() requires a string path", 0);
            fs::create_directories(args[0].asString());
            return Value();
        }));

    // sys.tempDir(prefix?) — create a unique temp directory under the system
    // temp dir and return its absolute path. Uses mkdtemp(3) for atomic
    // creation with mode 0700. Caller is responsible for removing it.
    sysMap->entries[Value("tempDir")] = Value(makeNative("sys.tempDir", -1,
        [](const std::vector<Value>& args) -> Value {
            std::string prefix = "praia";
            if (!args.empty()) {
                if (!args[0].isString())
                    throw RuntimeError("sys.tempDir() prefix must be a string", 0);
                prefix = args[0].asString();
            }
            std::string base;
            std::error_code ec;
            auto tmpPath = fs::temp_directory_path(ec);
            if (ec)
                throw RuntimeError("sys.tempDir(): " + ec.message(), 0);
            base = tmpPath.string();
            std::string tmpl = base + "/" + prefix + ".XXXXXX";
            std::vector<char> buf(tmpl.begin(), tmpl.end());
            buf.push_back('\0');
            if (!mkdtemp(buf.data()))
                throw RuntimeError("sys.tempDir(): " + std::string(std::strerror(errno)), 0);
            return Value(std::string(buf.data()));
        }));

    sysMap->entries[Value("remove")] = Value(makeNative("sys.remove", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.remove() requires a string path", 0);
            auto& p = args[0].asString();
            if (!fs::exists(p))
                throw RuntimeError("Cannot remove: " + p + " (not found)", 0);
            fs::remove_all(p);
            return Value();
        }));

    sysMap->entries[Value("readDir")] = Value(makeNative("sys.readDir", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.readDir() requires a string path", 0);
            auto& p = args[0].asString();
            if (!fs::is_directory(p))
                throw RuntimeError("sys.readDir(): not a directory: " + p, 0);
            auto arr = gcNew<PraiaArray>();
            for (auto& entry : fs::directory_iterator(p))
                arr->elements.push_back(Value(entry.path().filename().string()));
            return Value(arr);
        }));

    sysMap->entries[Value("copy")] = Value(makeNative("sys.copy", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("sys.copy() requires two string paths", 0);
            auto& src = args[0].asString();
            auto& dst = args[1].asString();
            if (!fs::exists(src))
                throw RuntimeError("Cannot copy: " + src + " (not found)", 0);
            fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            return Value();
        }));

    sysMap->entries[Value("move")] = Value(makeNative("sys.move", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("sys.move() requires two string paths", 0);
            auto& src = args[0].asString();
            auto& dst = args[1].asString();
            if (!fs::exists(src))
                throw RuntimeError("Cannot move: " + src + " (not found)", 0);
            fs::rename(src, dst);
            return Value();
        }));

    auto execImpl = makeNative("sys.exec", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || (!args[0].isString() && !args[0].isArray()))
                throw RuntimeError("sys.exec() requires a string or array of strings", 0);
            int timeoutMs = -1; // no timeout by default
            if (args.size() > 1 && args[1].isNumber()) {
                timeoutMs = static_cast<int>(args[1].asNumber());
            }

            // Build argv for the child process
            bool useShell = args[0].isString();
            std::string cmd;
            std::vector<std::string> argv;
            if (useShell) {
                cmd = args[0].asString();
            } else {
                auto& elems = args[0].asArray()->elements;
                if (elems.empty()) throw RuntimeError("sys.exec() array must not be empty", 0);
                for (auto& e : elems) {
                    if (!e.isString()) throw RuntimeError("sys.exec() array elements must be strings", 0);
                    argv.push_back(e.asString());
                }
            }

            int stdoutPipe[2], stderrPipe[2];
            if (pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0)
                throw RuntimeError("sys.exec(): failed to create pipes", 0);
            // CLOEXEC so any *other* fork+exec we do (or that the child does
            // before it exec()s) doesn't inherit these pipes. The dup2()
            // calls in the child strip CLOEXEC from the duplicates that
            // become the child's stdin/stdout/stderr, so the child still
            // gets its redirected streams across execvp().
            for (int p : {stdoutPipe[0], stdoutPipe[1], stderrPipe[0], stderrPipe[1]})
                praia::setCloexec(p);

            pid_t pid = fork();
            if (pid < 0) {
                close(stdoutPipe[0]); close(stdoutPipe[1]);
                close(stderrPipe[0]); close(stderrPipe[1]);
                throw RuntimeError("sys.exec(): fork failed", 0);
            }

            if (pid == 0) {
                close(stdoutPipe[0]);
                close(stderrPipe[0]);
                dup2(stdoutPipe[1], STDOUT_FILENO);
                dup2(stderrPipe[1], STDERR_FILENO);
                close(stdoutPipe[1]);
                close(stderrPipe[1]);
                // Become a process-group leader so kill(-pid, ...) reaches
                // any grandchildren (e.g. shells that fork their own kids).
                setpgid(0, 0);
                if (useShell) {
                    execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
                } else {
                    // Build C-string array for execvp (no shell, no injection)
                    std::vector<const char*> cargv;
                    for (auto& s : argv) cargv.push_back(s.c_str());
                    cargv.push_back(nullptr);
                    execvp(cargv[0], const_cast<char* const*>(cargv.data()));
                }
                _exit(127);
            }
            // Parent: also setpgid to close the race where we signal before
            // the child's setpgid runs. Either side winning is fine.
            setpgid(pid, pid);

            close(stdoutPipe[1]);
            close(stderrPipe[1]);

            // Read both pipes concurrently with poll() to avoid deadlock
            // when the child fills one pipe while we're blocked reading the other.
            std::string outStr, errStr;
            bool interrupted = false;
            bool timedOut = false;
            {
                struct pollfd fds[2];
                fds[0] = {stdoutPipe[0], POLLIN, 0};
                fds[1] = {stderrPipe[0], POLLIN, 0};
                int openCount = 2;
                char buf[4096];
                struct timeval deadline;
                if (timeoutMs >= 0) gettimeofday(&deadline, nullptr);
                while (openCount > 0) {
                    int pollTimeout = -1;
                    if (timeoutMs >= 0) {
                        struct timeval now;
                        gettimeofday(&now, nullptr);
                        int elapsed = static_cast<int>((now.tv_sec - deadline.tv_sec) * 1000 +
                                                       (now.tv_usec - deadline.tv_usec) / 1000);
                        int remaining = timeoutMs - elapsed;
                        if (remaining <= 0) { timedOut = true; break; }
                        pollTimeout = remaining;
                    }
                    int pr = poll(fds, 2, pollTimeout);
                    if (pr == 0 && timeoutMs >= 0) { timedOut = true; break; }
                    if (pr < 0 && errno == EINTR) {
                        if (g_pendingSignals.load(std::memory_order_relaxed) & (1u << SIGINT)) {
                            kill(-pid, SIGINT);
                            interrupted = true;
                            break;
                        }
                        continue;
                    }
                    for (int i = 0; i < 2; i++) {
                        if (fds[i].revents & (POLLIN | POLLHUP)) {
                            ssize_t n = read(fds[i].fd, buf, sizeof(buf));
                            if (n > 0) {
                                (i == 0 ? outStr : errStr).append(buf, n);
                            } else {
                                close(fds[i].fd);
                                fds[i].fd = -1;
                                openCount--;
                            }
                        }
                    }
                }
                if (fds[0].fd >= 0) close(fds[0].fd);
                if (fds[1].fd >= 0) close(fds[1].fd);
                if (!outStr.empty() && outStr.back() == '\n') outStr.pop_back();
                if (!errStr.empty() && errStr.back() == '\n') errStr.pop_back();
            }

            if (timedOut) {
                // Signal the whole process group so a shell child's
                // grandchildren (e.g. backgrounded sleeps) get reaped too.
                kill(-pid, SIGKILL);
            }

            int status = 0;
            waitpid(pid, &status, 0);
            int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            if (timedOut) exitCode = -1;

            if (interrupted)
                throw RuntimeError("Interrupted", 0);

            auto result = gcNew<PraiaMap>();
            result->entries[Value("stdout")] = Value(std::move(outStr));
            result->entries[Value("stderr")] = Value(std::move(errStr));
            result->entries[Value("exitCode")] = Value(static_cast<int64_t>(exitCode));
            if (timedOut) result->entries[Value("timedOut")] = Value(true);
            return Value(result);
        });
    sysMap->entries[Value("exec")] = Value(std::static_pointer_cast<Callable>(execImpl));
    sysMap->entries[Value("run")] = Value(std::static_pointer_cast<Callable>(execImpl));

    // sys.spawn(cmd) — launch a child process with stdin/stdout/stderr pipes.
    // cmd is either a string (run via /bin/sh -c) or an array of [argv0, ...args]
    // (run via execvp — no shell, no injection risk; mirrors sys.exec).
    // Returns a process handle map with write/read/readErr/readLine/closeStdin/wait methods.
    sysMap->entries[Value("spawn")] = Value(makeNative("sys.spawn", 1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || (!args[0].isString() && !args[0].isArray()))
                throw RuntimeError("sys.spawn() requires a string or array of strings", 0);

            bool useShell = args[0].isString();
            std::string cmd;
            std::vector<std::string> argv;
            if (useShell) {
                cmd = args[0].asString();
            } else {
                auto& elems = args[0].asArray()->elements;
                if (elems.empty()) throw RuntimeError("sys.spawn() array must not be empty", 0);
                for (auto& e : elems) {
                    if (!e.isString()) throw RuntimeError("sys.spawn() array elements must be strings", 0);
                    argv.push_back(e.asString());
                }
            }

            int stdinPipe[2], stdoutPipe[2], stderrPipe[2];
            if (pipe(stdinPipe) < 0 || pipe(stdoutPipe) < 0 || pipe(stderrPipe) < 0)
                throw RuntimeError("sys.spawn(): pipe creation failed", 0);
            for (int p : {stdinPipe[0], stdinPipe[1], stdoutPipe[0], stdoutPipe[1],
                          stderrPipe[0], stderrPipe[1]})
                praia::setCloexec(p);

            pid_t pid = fork();
            if (pid < 0) {
                close(stdinPipe[0]); close(stdinPipe[1]);
                close(stdoutPipe[0]); close(stdoutPipe[1]);
                close(stderrPipe[0]); close(stderrPipe[1]);
                throw RuntimeError("sys.spawn(): fork failed", 0);
            }

            if (pid == 0) {
                // Child
                close(stdinPipe[1]);   // close write end of stdin
                close(stdoutPipe[0]);  // close read end of stdout
                close(stderrPipe[0]);  // close read end of stderr
                dup2(stdinPipe[0], STDIN_FILENO);
                dup2(stdoutPipe[1], STDOUT_FILENO);
                dup2(stderrPipe[1], STDERR_FILENO);
                close(stdinPipe[0]);
                close(stdoutPipe[1]);
                close(stderrPipe[1]);
                // Become a process-group leader so proc.kill(-pid, ...) can
                // signal any grandchildren the child spawns.
                setpgid(0, 0);
                if (useShell) {
                    execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
                } else {
                    std::vector<const char*> cargv;
                    for (auto& s : argv) cargv.push_back(s.c_str());
                    cargv.push_back(nullptr);
                    execvp(cargv[0], const_cast<char* const*>(cargv.data()));
                }
                _exit(127);
            }
            // Parent: race-proof the setpgid by setting it from here too.
            setpgid(pid, pid);

            // Parent
            close(stdinPipe[0]);   // close read end of stdin
            close(stdoutPipe[1]);  // close write end of stdout
            close(stderrPipe[1]);  // close write end of stderr

            // Shared state for the process handle
            struct SpawnState {
                pid_t pid;
                int stdinFd;   // write end
                int stdoutFd;  // read end
                int stderrFd;  // read end
                bool stdinOpen = true;
                bool waited = false;
                int exitCode = -1;
                std::mutex mtx;
            };
            auto state = std::make_shared<SpawnState>();
            state->pid = pid;
            state->stdinFd = stdinPipe[1];
            state->stdoutFd = stdoutPipe[0];
            state->stderrFd = stderrPipe[0];

            auto proc = gcNew<PraiaMap>();

            proc->entries[Value("pid")] = Value(static_cast<int64_t>(pid));

            // proc.write(data) — write to child's stdin
            proc->entries[Value("write")] = Value(makeNative("proc.write", 1,
                [state](const std::vector<Value>& args) -> Value {
                    if (!args[0].isString())
                        throw RuntimeError("proc.write() requires a string", 0);
                    std::lock_guard<std::mutex> lock(state->mtx);
                    if (!state->stdinOpen)
                        throw RuntimeError("proc.write(): stdin is closed", 0);
                    auto& data = args[0].asString();
                    ssize_t written = ::write(state->stdinFd, data.data(), data.size());
                    if (written < 0)
                        throw RuntimeError("proc.write() failed", 0);
                    return Value(static_cast<int64_t>(written));
                }));

            // proc.closeStdin() — close the write end, signaling EOF to child
            proc->entries[Value("closeStdin")] = Value(makeNative("proc.closeStdin", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    if (state->stdinOpen) {
                        close(state->stdinFd);
                        state->stdinOpen = false;
                    }
                    return Value();
                }));

            // Helper: read all from an fd
            auto readAllFd = [](int fd) -> std::string {
                std::string result;
                char buf[4096];
                ssize_t n;
                while ((n = ::read(fd, buf, sizeof(buf))) > 0)
                    result.append(buf, n);
                return result;
            };

            // proc.read() — read all of child's stdout (blocks until EOF)
            proc->entries[Value("read")] = Value(makeNative("proc.read", 0,
                [state, readAllFd](const std::vector<Value>&) -> Value {
                    return Value(readAllFd(state->stdoutFd));
                }));

            // proc.readErr() — read all of child's stderr (blocks until EOF)
            proc->entries[Value("readErr")] = Value(makeNative("proc.readErr", 0,
                [state, readAllFd](const std::vector<Value>&) -> Value {
                    return Value(readAllFd(state->stderrFd));
                }));

            // proc.readLine() — read one line from stdout (blocks until \n or EOF)
            // Returns nil on EOF.
            proc->entries[Value("readLine")] = Value(makeNative("proc.readLine", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::string line;
                    char c;
                    while (true) {
                        ssize_t n = ::read(state->stdoutFd, &c, 1);
                        if (n <= 0) {
                            // EOF — return nil if nothing read, partial line otherwise
                            if (line.empty()) return Value();
                            return Value(std::move(line));
                        }
                        if (c == '\n') return Value(std::move(line));
                        line += c;
                    }
                }));

            // proc.wait() — wait for child to exit, return exitCode
            proc->entries[Value("wait")] = Value(makeNative("proc.wait", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    if (state->waited)
                        return Value(static_cast<int64_t>(state->exitCode));
                    // Close stdin if still open
                    if (state->stdinOpen) {
                        close(state->stdinFd);
                        state->stdinOpen = false;
                    }
                    int status = 0;
                    waitpid(state->pid, &status, 0);
                    state->exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    state->waited = true;
                    return Value(static_cast<int64_t>(state->exitCode));
                }));

            // proc.kill(signal?) — send a signal to the child (default SIGTERM).
            // Sent to the whole process group so any grandchildren the child
            // forked (e.g. shell pipelines) are signalled too.
            proc->entries[Value("kill")] = Value(makeNative("proc.kill", -1,
                [state](const std::vector<Value>& args) -> Value {
                    int sig = SIGTERM;
                    if (!args.empty() && args[0].isString()) {
                        int s = signalNameToNum(args[0].asString());
                        if (s < 0) throw RuntimeError("proc.kill(): unknown signal", 0);
                        sig = s;
                    } else if (!args.empty() && args[0].isNumber()) {
                        sig = static_cast<int>(args[0].asNumber());
                    }
                    ::kill(-state->pid, sig);
                    return Value();
                }));

            return Value(proc);
        }));

    sysMap->entries[Value("exit")] = Value(makeNative("sys.exit", 1,
        [](const std::vector<Value>& args) -> Value {
            int code = 0;
            if (!args.empty() && args[0].isNumber())
                code = static_cast<int>(args[0].asNumber());
            throw ExitSignal{code};
        }));

    // sys.input(prompt?) — read a line from stdin. Returns nil on EOF.
    sysMap->entries[Value("input")] = Value(makeNative("sys.input", -1,
        [](const std::vector<Value>& args) -> Value {
            if (!args.empty() && args[0].isString()) {
                std::cout << args[0].asString() << std::flush;
            }
            std::string line;
            if (!std::getline(std::cin, line)) return Value(); // EOF
            return Value(std::move(line));
        }));

    // sys.args — defaults to empty, set via setArgs()
    auto emptyArgs = gcNew<PraiaArray>();
    sysMap->entries[Value("args")] = Value(emptyArgs);

    globals->define("sys", Value(sysMap));

    // ── http namespace ──

    auto httpMap = gcNew<PraiaMap>();

    httpMap->entries[Value("get")] = Value(makeNative("http.get", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("http.get() requires a URL string", 0);
            return doHttpRequest("GET", args[0].asString(), "", {});
        }));

    httpMap->entries[Value("post")] = Value(makeNative("http.post", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("http.post() requires a URL string", 0);
            std::string body;
            std::unordered_map<std::string, std::string> headers;
            if (args[1].isString()) {
                body = args[1].asString();
            } else if (args[1].isMap()) {
                auto& e = args[1].asMap()->entries;
                if (e.count("body")) body = e.at("body").toString();
                if (e.count("headers") && e.at("headers").isMap()) {
                    for (auto& [k, v] : e.at("headers").asMap()->entries)
                        headers[k.toString()] = v.toString();
                }
            }
            return doHttpRequest("POST", args[0].asString(), body, headers);
        }));

    httpMap->entries[Value("request")] = Value(makeNative("http.request", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap())
                throw RuntimeError("http.request() requires an options map", 0);
            auto& opts = args[0].asMap()->entries;
            std::string method = "GET", url, body;
            std::unordered_map<std::string, std::string> headers;
            if (opts.count("method")) method = opts.at("method").toString();
            if (opts.count("url")) url = opts.at("url").toString();
            else throw RuntimeError("http.request() requires a 'url' field", 0);
            if (opts.count("body")) body = opts.at("body").toString();
            if (opts.count("headers") && opts.at("headers").isMap()) {
                for (auto& [k, v] : opts.at("headers").asMap()->entries)
                    headers[k.toString()] = v.toString();
            }
            return doHttpRequest(method, url, body, headers);
        }));

    httpMap->entries[Value("createServer")] = Value(makeNative("http.createServer", 1,
        [self](const std::vector<Value>& args) -> Value {
            if (!args[0].isCallable())
                throw RuntimeError("http.createServer() requires a handler function", 0);
            auto handler = args[0].asCallable();

            auto server = gcNew<PraiaMap>();
            server->entries[Value("listen")] = Value(makeNative("listen", 1,
                [handler, self](const std::vector<Value>& args) -> Value {
                    if (!args[0].isNumber())
                        throw RuntimeError("listen() requires a port number", 0);
                    httpServerListen(static_cast<int>(args[0].asNumber()), handler, *self);
                    return Value();
                }));
            return Value(server);
        }));

    // http.sse(req, callback) — Server-Sent Events
    // callback receives a send function: send(data, event?)
    // The connection stays open until the callback returns.
    httpMap->entries[Value("sse")] = Value(makeNative("http.sse", 2,
        [self](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap())
                throw RuntimeError("http.sse() requires a request object", 0);
            if (!args[1].isCallable())
                throw RuntimeError("http.sse() requires a callback function", 0);

            auto& reqMap = args[0].asMap()->entries;
            if (!reqMap.count("__clientFd"))
                throw RuntimeError("http.sse() must be called inside an HTTP handler", 0);

            int clientFd = static_cast<int>(reqMap["__clientFd"].asNumber());
            auto callback = args[1].asCallable();

            // Send SSE headers
            std::string headers = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/event-stream\r\n"
                                  "Cache-Control: no-cache\r\n"
                                  "Connection: keep-alive\r\n"
                                  "Access-Control-Allow-Origin: *\r\n"
                                  "\r\n";
            send(clientFd, headers.c_str(), headers.size(), 0);

            // Create a send function for the callback
            auto sendFn = makeNative("send", -1,
                [clientFd](const std::vector<Value>& args) -> Value {
                    if (args.empty())
                        throw RuntimeError("send() requires data", 0);
                    std::string msg;
                    // Optional event name. Per the SSE spec the event field
                    // is a single line — newlines inside it would break the
                    // frame, so strip them rather than re-emit (a multi-line
                    // event name doesn't have a sensible meaning).
                    if (args.size() > 1 && args[1].isString()) {
                        std::string evt = args[1].asString();
                        for (char& c : evt) if (c == '\n' || c == '\r') c = ' ';
                        msg += "event: " + evt + "\n";
                    }
                    // SSE frames every line of the payload with its own
                    // `data:` prefix; the message ends with a blank line.
                    // Normalize \r\n and \r to \n first, then split.
                    const std::string& payload = args[0].toString();
                    std::string line;
                    auto emit = [&]() {
                        msg += "data: ";
                        msg += line;
                        msg += "\n";
                        line.clear();
                    };
                    for (size_t i = 0; i < payload.size(); i++) {
                        char c = payload[i];
                        if (c == '\r') {
                            emit();
                            if (i + 1 < payload.size() && payload[i + 1] == '\n') i++;
                        } else if (c == '\n') {
                            emit();
                        } else {
                            line += c;
                        }
                    }
                    emit(); // trailing/final line (even if empty for "")
                    msg += "\n"; // blank line terminator
                    ssize_t sent = ::send(clientFd, msg.c_str(), msg.size(), 0);
                    if (sent < 0)
                        throw RuntimeError("SSE client disconnected", 0);
                    return Value();
                });

            // Call the callback with the send function
            try {
                std::vector<Value> cbArgs = {Value(std::static_pointer_cast<Callable>(sendFn))};
                callSafe(*self, callback, cbArgs);
            } catch (const RuntimeError&) {
                // Client likely disconnected — not an error
            }

            close(clientFd);

            // Return a marker so the server loop knows not to send a response
            auto marker = gcNew<PraiaMap>();
            marker->entries[Value("__sse")] = Value(true);
            return Value(marker);
        }));

    httpMap->entries[Value("encodeURI")] = Value(makeNative("http.encodeURI", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("http.encodeURI() requires a string", 0);
            auto& input = args[0].asString();
            std::string result;
            result.reserve(input.size() * 3);
            for (unsigned char c : input) {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                    result += static_cast<char>(c);
                } else {
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", c);
                    result += hex;
                }
            }
            return Value(std::move(result));
        }));

    httpMap->entries[Value("decodeURI")] = Value(makeNative("http.decodeURI", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("http.decodeURI() requires a string", 0);
            auto& input = args[0].asString();
            std::string result;
            result.reserve(input.size());
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            for (size_t i = 0; i < input.size(); ++i) {
                if (input[i] == '%' && i + 2 < input.size()) {
                    int hi = hexVal(input[i + 1]), lo = hexVal(input[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        result += static_cast<char>((hi << 4) | lo);
                        i += 2;
                        continue;
                    }
                }
                result += input[i];
            }
            return Value(std::move(result));
        }));

    // ── Response helpers ──

    // http.json(obj, status?) → {status, body, headers}
    httpMap->entries[Value("json")] = Value(makeNative("http.json", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty())
                throw RuntimeError("http.json() requires a value", 0);
            int status = 200;
            if (args.size() > 1 && args[1].isNumber())
                status = static_cast<int>(args[1].asNumber());
            auto res = gcNew<PraiaMap>();
            res->entries[Value("status")] = Value(static_cast<double>(status));
            res->entries[Value("body")] = Value(jsonStringify(args[0], 0, 0));
            auto hdrs = gcNew<PraiaMap>();
            hdrs->entries[Value("Content-Type")] = Value("application/json");
            res->entries[Value("headers")] = Value(hdrs);
            return Value(res);
        }));

    // http.text(str, status?) → {status, body, headers}
    httpMap->entries[Value("text")] = Value(makeNative("http.text", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty())
                throw RuntimeError("http.text() requires a string", 0);
            int status = 200;
            if (args.size() > 1 && args[1].isNumber())
                status = static_cast<int>(args[1].asNumber());
            auto res = gcNew<PraiaMap>();
            res->entries[Value("status")] = Value(static_cast<double>(status));
            res->entries[Value("body")] = Value(args[0].toString());
            auto hdrs = gcNew<PraiaMap>();
            hdrs->entries[Value("Content-Type")] = Value("text/plain");
            res->entries[Value("headers")] = Value(hdrs);
            return Value(res);
        }));

    // http.html(str, status?) → {status, body, headers}
    httpMap->entries[Value("html")] = Value(makeNative("http.html", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty())
                throw RuntimeError("http.html() requires a string", 0);
            int status = 200;
            if (args.size() > 1 && args[1].isNumber())
                status = static_cast<int>(args[1].asNumber());
            auto res = gcNew<PraiaMap>();
            res->entries[Value("status")] = Value(static_cast<double>(status));
            res->entries[Value("body")] = Value(args[0].toString());
            auto hdrs = gcNew<PraiaMap>();
            hdrs->entries[Value("Content-Type")] = Value("text/html; charset=utf-8");
            res->entries[Value("headers")] = Value(hdrs);
            return Value(res);
        }));

    // http.redirect(url, status?) → {status, body, headers}
    // The URL is rejected if it contains CR/LF/NUL — without this check
    // an attacker who controls part of the redirect target (e.g. a
    // `?next=...` query param echoed into http.redirect) could close
    // the Location line and inject further headers (Set-Cookie, etc.)
    // or a fake response body. This is the classic "response splitting"
    // vector — refuse at the call site.
    httpMap->entries[Value("redirect")] = Value(makeNative("http.redirect", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("http.redirect() requires a URL string", 0);
            const std::string& url = args[0].asString();
            for (char c : url) {
                if (c == '\r' || c == '\n' || c == '\0')
                    throw RuntimeError("http.redirect(): URL contains CR/LF/NUL "
                                       "(header injection)", 0);
            }
            int status = 302;
            if (args.size() > 1 && args[1].isNumber())
                status = static_cast<int>(args[1].asNumber());
            auto res = gcNew<PraiaMap>();
            res->entries[Value("status")] = Value(static_cast<double>(status));
            res->entries[Value("body")] = Value(std::string(""));
            auto hdrs = gcNew<PraiaMap>();
            hdrs->entries[Value("Location")] = Value(url);
            res->entries[Value("headers")] = Value(hdrs);
            return Value(res);
        }));

    // Path-traversal guard for http.file / http.fileStream. When the caller
    // supplies a `withinDir` option, both the path and the dir are resolved
    // to canonical absolute paths (handles `..`, relative components, and
    // symlinks via weakly_canonical so non-existent paths don't throw at
    // resolve time — the file-open will surface a clean "not found" later).
    // Then we check the resolved path sits under the resolved dir with a
    // trailing-slash boundary so /var/www-evil can't pass against /var/www.
    //
    // Symlinks are resolved through, so a symlink in the served dir
    // pointing at /etc/passwd is correctly rejected.
    auto validatePathWithin = [](const std::string& path, const std::string& withinDir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto resolvedPath = fs::weakly_canonical(fs::path(path), ec);
        if (ec)
            throw RuntimeError("Invalid path: " + path, 0);
        auto resolvedDir = fs::weakly_canonical(fs::path(withinDir), ec);
        if (ec)
            throw RuntimeError("Invalid withinDir: " + withinDir, 0);
        // Append a separator so /var/www doesn't accidentally match
        // /var/www-evil/secret.
        std::string dirStr = resolvedDir.string();
        if (dirStr.empty() || dirStr.back() != fs::path::preferred_separator)
            dirStr.push_back(fs::path::preferred_separator);
        std::string pathStr = resolvedPath.string();
        // Allow exact match against the dir itself (rare but legal — e.g.
        // serving the directory entry, which open() will then reject).
        if (pathStr + fs::path::preferred_separator != dirStr &&
            pathStr.compare(0, dirStr.size(), dirStr) != 0) {
            throw RuntimeError("Path escapes withinDir: " + path, 0);
        }
    };

    // http.file(path, status?, options?) → {status, body, headers} with MIME detection.
    // options:
    //   {download: bool|string}  — adds Content-Disposition: attachment.
    //     true   → use the path's basename as the suggested filename
    //     "name" → use the given filename (e.g. for renaming on download)
    //   {withinDir: "/path"}     — opt-in jail. Resolves `path` and `withinDir`
    //     and refuses to serve anything outside the jail. Use whenever any
    //     part of `path` came from user input (URL params, query strings,
    //     form fields). Defaults to off — by design, callers without user
    //     input pay no resolution overhead and can serve anywhere.
    auto httpFileImpl = [validatePathWithin](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("http.file() requires a file path", 0);
            auto& path = args[0].asString();
            int status = 200;
            std::shared_ptr<PraiaMap> opts;
            // Accept either (path, status), (path, options), or (path, status, options).
            for (size_t i = 1; i < args.size(); i++) {
                if (args[i].isNumber()) status = static_cast<int>(args[i].asNumber());
                else if (args[i].isMap()) opts = args[i].asMap();
            }

            if (opts) {
                auto wd = opts->entries.find(Value("withinDir"));
                if (wd != opts->entries.end() && wd->second.isString())
                    validatePathWithin(path, wd->second.asString());
            }

            std::ifstream f(path, std::ios::binary);
            if (!f.is_open())
                throw RuntimeError("Cannot read file: " + path, 0);
            std::stringstream ss;
            ss << f.rdbuf();

            // MIME type detection from extension
            std::string mime = "application/octet-stream";
            auto dot = path.rfind('.');
            if (dot != std::string::npos) {
                std::string ext = path.substr(dot);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".html" || ext == ".htm") mime = "text/html; charset=utf-8";
                else if (ext == ".css")  mime = "text/css";
                else if (ext == ".js")   mime = "application/javascript";
                else if (ext == ".json") mime = "application/json";
                else if (ext == ".xml")  mime = "application/xml";
                else if (ext == ".txt")  mime = "text/plain";
                else if (ext == ".csv")  mime = "text/csv";
                else if (ext == ".svg")  mime = "image/svg+xml";
                else if (ext == ".png")  mime = "image/png";
                else if (ext == ".jpg" || ext == ".jpeg") mime = "image/jpeg";
                else if (ext == ".gif")  mime = "image/gif";
                else if (ext == ".ico")  mime = "image/x-icon";
                else if (ext == ".webp") mime = "image/webp";
                else if (ext == ".woff") mime = "font/woff";
                else if (ext == ".woff2") mime = "font/woff2";
                else if (ext == ".pdf")  mime = "application/pdf";
                else if (ext == ".zip")  mime = "application/zip";
                else if (ext == ".mp3")  mime = "audio/mpeg";
                else if (ext == ".mp4")  mime = "video/mp4";
                else if (ext == ".wasm") mime = "application/wasm";
            }

            auto res = gcNew<PraiaMap>();
            res->entries[Value("status")] = Value(static_cast<double>(status));
            res->entries[Value("body")] = Value(ss.str());
            auto hdrs = gcNew<PraiaMap>();
            hdrs->entries[Value("Content-Type")] = Value(mime);

            // Build Content-Disposition if download requested.
            if (opts) {
                auto it = opts->entries.find(Value("download"));
                if (it != opts->entries.end()) {
                    std::string filename;
                    if (it->second.isString()) {
                        filename = it->second.asString();
                    } else if (it->second.isTruthy()) {
                        // Default filename = basename of the served path.
                        auto slash = path.find_last_of("/\\");
                        filename = (slash == std::string::npos) ? path : path.substr(slash + 1);
                    }
                    if (!filename.empty()) {
                        // Quote the filename and escape any embedded quotes/backslashes.
                        std::string quoted;
                        quoted.reserve(filename.size() + 2);
                        for (char c : filename) {
                            if (c == '"' || c == '\\') quoted.push_back('\\');
                            quoted.push_back(c);
                        }
                        hdrs->entries[Value("Content-Disposition")] =
                            Value(std::string("attachment; filename=\"") + quoted + "\"");
                    } else {
                        hdrs->entries[Value("Content-Disposition")] =
                            Value(std::string("attachment"));
                    }
                }
            }

            res->entries[Value("headers")] = Value(hdrs);
            return Value(res);
    };
    httpMap->entries[Value("file")] = Value(makeNative("http.file", -1, httpFileImpl));

    // http.download(path, filename?) — convenience that always sets
    // Content-Disposition: attachment. If filename is omitted, uses basename(path).
    httpMap->entries[Value("download")] = Value(makeNative("http.download", -1,
        [httpFileImpl](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("http.download() requires a file path", 0);
            auto opts = gcNew<PraiaMap>();
            if (args.size() > 1 && args[1].isString()) {
                opts->entries[Value("download")] = args[1];
            } else {
                opts->entries[Value("download")] = Value(true);
            }
            return httpFileImpl({args[0], Value(opts)});
        }));

    // http.fileStream(path, status?, options?) — same shape as http.file but
    // the body is streamed by the server in 64 KB chunks rather than buffered
    // into memory. Use this for large file responses (videos, archives, etc.).
    // Options accepted: same as http.file (currently {download: bool|string}).
    httpMap->entries[Value("fileStream")] = Value(makeNative("http.fileStream", -1,
        [validatePathWithin](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("http.fileStream() requires a file path", 0);
            auto& path = args[0].asString();
            int status = 200;
            std::shared_ptr<PraiaMap> opts;
            for (size_t i = 1; i < args.size(); i++) {
                if (args[i].isNumber()) status = static_cast<int>(args[i].asNumber());
                else if (args[i].isMap()) opts = args[i].asMap();
            }

            if (opts) {
                auto wd = opts->entries.find(Value("withinDir"));
                if (wd != opts->entries.end() && wd->second.isString())
                    validatePathWithin(path, wd->second.asString());
            }

            // MIME type detection from extension (mirrors http.file).
            std::string mime = "application/octet-stream";
            auto dot = path.rfind('.');
            if (dot != std::string::npos) {
                std::string ext = path.substr(dot);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".html" || ext == ".htm") mime = "text/html; charset=utf-8";
                else if (ext == ".css")  mime = "text/css";
                else if (ext == ".js")   mime = "application/javascript";
                else if (ext == ".json") mime = "application/json";
                else if (ext == ".xml")  mime = "application/xml";
                else if (ext == ".txt")  mime = "text/plain";
                else if (ext == ".csv")  mime = "text/csv";
                else if (ext == ".svg")  mime = "image/svg+xml";
                else if (ext == ".png")  mime = "image/png";
                else if (ext == ".jpg" || ext == ".jpeg") mime = "image/jpeg";
                else if (ext == ".gif")  mime = "image/gif";
                else if (ext == ".webp") mime = "image/webp";
                else if (ext == ".pdf")  mime = "application/pdf";
                else if (ext == ".zip")  mime = "application/zip";
                else if (ext == ".mp3")  mime = "audio/mpeg";
                else if (ext == ".mp4")  mime = "video/mp4";
                else if (ext == ".webm") mime = "video/webm";
                else if (ext == ".mov")  mime = "video/quicktime";
                else if (ext == ".wav")  mime = "audio/wav";
            }

            auto res = gcNew<PraiaMap>();
            res->entries[Value("status")] = Value(static_cast<double>(status));
            res->entries[Value("__streamFile")] = Value(path);
            auto hdrs = gcNew<PraiaMap>();
            hdrs->entries[Value("Content-Type")] = Value(mime);
            if (opts) {
                auto it = opts->entries.find(Value("download"));
                if (it != opts->entries.end()) {
                    std::string filename;
                    if (it->second.isString()) filename = it->second.asString();
                    else if (it->second.isTruthy()) {
                        auto slash = path.find_last_of("/\\");
                        filename = (slash == std::string::npos) ? path : path.substr(slash + 1);
                    }
                    if (!filename.empty()) {
                        std::string quoted;
                        quoted.reserve(filename.size() + 2);
                        for (char c : filename) {
                            if (c == '"' || c == '\\') quoted.push_back('\\');
                            quoted.push_back(c);
                        }
                        hdrs->entries[Value("Content-Disposition")] =
                            Value(std::string("attachment; filename=\"") + quoted + "\"");
                    } else {
                        hdrs->entries[Value("Content-Disposition")] = Value(std::string("attachment"));
                    }
                }
            }
            res->entries[Value("headers")] = Value(hdrs);
            return Value(res);
        }));

    globals->define("http", Value(httpMap));

    // ── json namespace ──

    auto jsonMap = gcNew<PraiaMap>();

    // Optional second arg: either a number (max input length in bytes) or
    // an options map (currently only {maxLength: N}). With no second arg
    // the parser accepts any size — same behavior as before. Use the cap
    // for hardening against hostile input on untrusted endpoints (the
    // recursion-depth guard inside the parser already covers stack-blowing
    // bracket bombs; this guards against straight-line allocation bombs
    // like a 100 MB array literal).
    auto extractMaxLength = [](const std::vector<Value>& args) -> std::pair<size_t, bool> {
        if (args.size() < 2) return {0, false};
        if (args[1].isNumber()) {
            double n = args[1].asNumber();
            if (n < 0) return {0, false};
            return {static_cast<size_t>(n), true};
        }
        if (args[1].isMap()) {
            auto it = args[1].asMap()->entries.find(Value("maxLength"));
            if (it != args[1].asMap()->entries.end() && it->second.isNumber()) {
                double n = it->second.asNumber();
                if (n < 0) return {0, false};
                return {static_cast<size_t>(n), true};
            }
        }
        return {0, false};
    };

    jsonMap->entries[Value("parse")] = Value(makeNative("json.parse", -1,
        [extractMaxLength](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("json.parse() requires a string", 0);
            auto& src = args[0].asString();
            auto [limit, hasLimit] = extractMaxLength(args);
            if (hasLimit && src.size() > limit) {
                throw RuntimeError("json.parse(): input exceeds maxLength of " +
                    std::to_string(limit) + " bytes (got " +
                    std::to_string(src.size()) + ")", 0);
            }
            return jsonParse(src);
        }));

    jsonMap->entries[Value("stringify")] = Value(makeNative("json.stringify", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty())
                throw RuntimeError("json.stringify() requires a value", 0);
            int indent = 0;
            if (args.size() > 1 && args[1].isNumber())
                indent = static_cast<int>(args[1].asNumber());
            return Value(jsonStringify(args[0], indent, 0));
        }));

    globals->define("json", Value(jsonMap));

    // ── yaml namespace ──

    auto yamlMap = gcNew<PraiaMap>();

    // Optional second arg mirrors json.parse — number or {maxLength: N}.
    yamlMap->entries[Value("parse")] = Value(makeNative("yaml.parse", -1,
        [extractMaxLength](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("yaml.parse() requires a string", 0);
            auto& src = args[0].asString();
            auto [limit, hasLimit] = extractMaxLength(args);
            if (hasLimit && src.size() > limit) {
                throw RuntimeError("yaml.parse(): input exceeds maxLength of " +
                    std::to_string(limit) + " bytes (got " +
                    std::to_string(src.size()) + ")", 0);
            }
            return yamlParse(src);
        }));

    yamlMap->entries[Value("stringify")] = Value(makeNative("yaml.stringify", 1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty())
                throw RuntimeError("yaml.stringify() requires a value", 0);
            return Value(yamlStringify(args[0], 0));
        }));

    globals->define("yaml", Value(yamlMap));

    // ── base64 namespace ──

    auto base64Map = gcNew<PraiaMap>();

    base64Map->entries[Value("encode")] = Value(makeNative("base64.encode", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("base64.encode() requires a string", 0);
            auto& input = args[0].asString();
            static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string result;
            int val = 0, valb = -6;
            for (unsigned char c : input) {
                val = (val << 8) + c;
                valb += 8;
                while (valb >= 0) {
                    result += table[(val >> valb) & 0x3F];
                    valb -= 6;
                }
            }
            if (valb > -6) result += table[((val << 8) >> (valb + 8)) & 0x3F];
            while (result.size() % 4) result += '=';
            return Value(std::move(result));
        }));

    base64Map->entries[Value("decode")] = Value(makeNative("base64.decode", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("base64.decode() requires a string", 0);
            auto& input = args[0].asString();
            auto decodeChar = [](char c) -> int {
                if (c >= 'A' && c <= 'Z') return c - 'A';
                if (c >= 'a' && c <= 'z') return c - 'a' + 26;
                if (c >= '0' && c <= '9') return c - '0' + 52;
                if (c == '+') return 62;
                if (c == '/') return 63;
                return -1;
            };
            std::string result;
            int val = 0, valb = -8;
            for (unsigned char c : input) {
                if (c == '=') break;
                int d = decodeChar(c);
                if (d < 0) continue;
                val = (val << 6) + d;
                valb += 6;
                if (valb >= 0) {
                    result += static_cast<char>((val >> valb) & 0xFF);
                    valb -= 8;
                }
            }
            return Value(std::move(result));
        }));

    // base64.encodeURL — URL-safe base64 (RFC 4648 §5): +/ replaced with -_, no padding
    base64Map->entries[Value("encodeURL")] = Value(makeNative("base64.encodeURL", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("base64.encodeURL() requires a string", 0);
            auto& input = args[0].asString();
            static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            std::string result;
            int val = 0, valb = -6;
            for (unsigned char c : input) {
                val = (val << 8) + c;
                valb += 8;
                while (valb >= 0) {
                    result += table[(val >> valb) & 0x3F];
                    valb -= 6;
                }
            }
            if (valb > -6) result += table[((val << 8) >> (valb + 8)) & 0x3F];
            // No padding for URL-safe variant
            return Value(std::move(result));
        }));

    // base64.decodeURL — decode URL-safe base64
    base64Map->entries[Value("decodeURL")] = Value(makeNative("base64.decodeURL", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("base64.decodeURL() requires a string", 0);
            auto& input = args[0].asString();
            auto decodeChar = [](char c) -> int {
                if (c >= 'A' && c <= 'Z') return c - 'A';
                if (c >= 'a' && c <= 'z') return c - 'a' + 26;
                if (c >= '0' && c <= '9') return c - '0' + 52;
                if (c == '-') return 62;
                if (c == '_') return 63;
                return -1;
            };
            std::string result;
            int val = 0, valb = -8;
            for (unsigned char c : input) {
                if (c == '=') break;
                int d = decodeChar(c);
                if (d < 0) continue;
                val = (val << 6) + d;
                valb += 6;
                if (valb >= 0) {
                    result += static_cast<char>((val >> valb) & 0xFF);
                    valb -= 8;
                }
            }
            return Value(std::move(result));
        }));

    globals->define("base64", Value(base64Map));

    // ── path namespace ──

    auto pathMap = gcNew<PraiaMap>();

    pathMap->entries[Value("join")] = Value(makeNative("path.join", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(std::string(""));
            fs::path result;
            for (auto& a : args) {
                if (!a.isString())
                    throw RuntimeError("path.join() requires string arguments", 0);
                if (result.empty()) result = a.asString();
                else result /= a.asString();
            }
            return Value(result.string());
        }));

    pathMap->entries[Value("dirname")] = Value(makeNative("path.dirname", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString()) throw RuntimeError("path.dirname() requires a string", 0);
            return Value(fs::path(args[0].asString()).parent_path().string());
        }));

    pathMap->entries[Value("basename")] = Value(makeNative("path.basename", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString()) throw RuntimeError("path.basename() requires a string", 0);
            return Value(fs::path(args[0].asString()).filename().string());
        }));

    pathMap->entries[Value("ext")] = Value(makeNative("path.ext", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString()) throw RuntimeError("path.ext() requires a string", 0);
            return Value(fs::path(args[0].asString()).extension().string());
        }));

    pathMap->entries[Value("resolve")] = Value(makeNative("path.resolve", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString()) throw RuntimeError("path.resolve() requires a string", 0);
            return Value(fs::absolute(args[0].asString()).string());
        }));

    // Walk `dir` recursively, invoking `visit` on each regular file. Matches
    // the default behavior of Go's filepath.Walk, Rust's walkdir, Java's
    // Files.walk, Python's os.walk, and Unix `find`:
    //
    //   - Symlinks to **directories** are not followed (avoids loops; a
    //     subdir pointing at an ancestor would otherwise wedge the
    //     iterator forever).
    //   - Symlinks to **files** are yielded like any other file — they're
    //     useful and pose no DoS risk.
    //   - Broken symlinks are silently skipped.
    //
    // skip_permission_denied also covers unreadable subtrees mid-walk.
    // Used by path.walk and the recursive branch of path.glob.
    auto walkRegularFiles = [](const std::string& dir,
                               std::function<void(const fs::directory_entry&)> visit) {
        fs::recursive_directory_iterator it(
            dir, fs::directory_options::skip_permission_denied);
        fs::recursive_directory_iterator end;
        for (; it != end; ++it) {
            const auto& entry = *it;
            if (entry.is_symlink()) {
                // Decide based on what the link points at. status() follows
                // the symlink; on a broken link it throws/returns "not
                // found" — guard with error_code so we silently skip.
                std::error_code ec;
                auto st = fs::status(entry.path(), ec);
                if (ec || !fs::exists(st)) continue; // broken: skip
                if (fs::is_directory(st)) {
                    // Don't recurse into the target — that's the loop risk.
                    it.disable_recursion_pending();
                    continue;
                }
                if (fs::is_regular_file(st)) visit(entry);
                continue;
            }
            if (entry.is_regular_file()) visit(entry);
        }
    };

    // path.walk(dir) — recursively list all files as relative paths.
    // Skips symlinks (see walkRegularFiles).
    pathMap->entries[Value("walk")] = Value(makeNative("path.walk", 1,
        [walkRegularFiles](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("path.walk() requires a string path", 0);
            auto& dir = args[0].asString();
            if (!fs::is_directory(dir))
                throw RuntimeError("path.walk(): not a directory: " + dir, 0);
            auto arr = gcNew<PraiaArray>();
            walkRegularFiles(dir, [&](const fs::directory_entry& e) {
                arr->elements.push_back(Value(e.path().string()));
            });
            return Value(arr);
        }));

    // path.glob(dir, pattern) — match files by extension pattern (e.g. "*.praia", "*.cpp").
    // Recursive (`**`) variant skips symlinks; non-recursive variant only
    // visits direct children so symlink loops can't arise there.
    pathMap->entries[Value("glob")] = Value(makeNative("path.glob", 2,
        [walkRegularFiles](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("path.glob() requires (directory, pattern)", 0);
            auto& dir = args[0].asString();
            auto& pattern = args[1].asString();
            if (!fs::is_directory(dir))
                throw RuntimeError("path.glob(): not a directory: " + dir, 0);

            // Extract extension from pattern like "*.praia" or ".praia"
            std::string ext;
            bool recursive = pattern.find("**") != std::string::npos;
            auto starDot = pattern.find("*.");
            if (starDot != std::string::npos) {
                ext = pattern.substr(starDot + 1); // ".praia"
            } else if (pattern[0] == '.') {
                ext = pattern;
            }

            auto arr = gcNew<PraiaArray>();
            auto matches = [&](const fs::path& p) {
                return ext.empty() || p.extension().string() == ext;
            };
            if (recursive) {
                walkRegularFiles(dir, [&](const fs::directory_entry& e) {
                    if (matches(e.path()))
                        arr->elements.push_back(Value(e.path().string()));
                });
            } else {
                // Non-recursive: no loop risk, so symlink-to-file is fine.
                // Just resolve through symlinks for the regular-file check.
                fs::directory_iterator it(
                    dir, fs::directory_options::skip_permission_denied);
                fs::directory_iterator end;
                for (; it != end; ++it) {
                    const auto& entry = *it;
                    std::error_code ec;
                    auto st = entry.is_symlink() ? fs::status(entry.path(), ec)
                                                 : entry.status(ec);
                    if (ec || !fs::is_regular_file(st)) continue;
                    if (matches(entry.path()))
                        arr->elements.push_back(Value(entry.path().string()));
                }
            }
            return Value(arr);
        }));

    // path.isDir(path) — check if path is a directory
    pathMap->entries[Value("isDir")] = Value(makeNative("path.isDir", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString()) throw RuntimeError("path.isDir() requires a string", 0);
            return Value(fs::is_directory(args[0].asString()));
        }));

    // path.isFile(path) — check if path is a regular file
    pathMap->entries[Value("isFile")] = Value(makeNative("path.isFile", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString()) throw RuntimeError("path.isFile() requires a string", 0);
            return Value(fs::is_regular_file(args[0].asString()));
        }));

    // path.size(path) — file size in bytes
    pathMap->entries[Value("size")] = Value(makeNative("path.size", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString()) throw RuntimeError("path.size() requires a string", 0);
            auto& p = args[0].asString();
            if (!fs::exists(p))
                throw RuntimeError("path.size(): file not found: " + p, 0);
            return Value(static_cast<int64_t>(fs::file_size(p)));
        }));

    // path.mtime(path) — last modification time in seconds since the Unix epoch.
    pathMap->entries[Value("mtime")] = Value(makeNative("path.mtime", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString()) throw RuntimeError("path.mtime() requires a string", 0);
            auto& p = args[0].asString();
            if (!fs::exists(p))
                throw RuntimeError("path.mtime(): file not found: " + p, 0);
            // fs::file_time_type uses an unspecified clock pre-C++20, so go through stat(2).
            struct stat st;
            if (::stat(p.c_str(), &st) != 0)
                throw RuntimeError("path.mtime(): stat failed: " + p, 0);
            return Value(static_cast<int64_t>(st.st_mtime));
        }));

    globals->define("path", Value(pathMap));

    // ── url namespace ──

    auto urlMap = gcNew<PraiaMap>();

    urlMap->entries[Value("parse")] = Value(makeNative("url.parse", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString()) throw RuntimeError("url.parse() requires a string", 0);
            auto& input = args[0].asString();
            auto result = gcNew<PraiaMap>();
            std::string rest = input;

            auto schemeEnd = rest.find("://");
            if (schemeEnd != std::string::npos) {
                result->entries[Value("scheme")] = Value(rest.substr(0, schemeEnd));
                rest = rest.substr(schemeEnd + 3);
            } else {
                result->entries[Value("scheme")] = Value(std::string(""));
            }

            auto slashPos = rest.find('/');
            std::string hostPort = (slashPos != std::string::npos) ? rest.substr(0, slashPos) : rest;
            std::string pathAndQuery = (slashPos != std::string::npos) ? rest.substr(slashPos) : "/";

            auto colonPos = hostPort.find(':');
            if (colonPos != std::string::npos) {
                result->entries[Value("host")] = Value(hostPort.substr(0, colonPos));
                try { result->entries[Value("port")] = Value(static_cast<double>(std::stoi(hostPort.substr(colonPos + 1)))); }
                catch (...) { result->entries[Value("port")] = Value(0.0); }
            } else {
                result->entries[Value("host")] = Value(hostPort);
                result->entries[Value("port")] = Value(0.0);
            }

            auto queryPos = pathAndQuery.find('?');
            if (queryPos != std::string::npos) {
                result->entries[Value("path")] = Value(pathAndQuery.substr(0, queryPos));
                result->entries[Value("query")] = Value(pathAndQuery.substr(queryPos + 1));
            } else {
                result->entries[Value("path")] = Value(pathAndQuery);
                result->entries[Value("query")] = Value(std::string(""));
            }

            return Value(result);
        }));

    globals->define("url", Value(urlMap));

    // ── net namespace (TCP sockets) ──


    auto netMap = gcNew<PraiaMap>();
    registerNetBuiltins(netMap);
    globals->define("net", Value(netMap));

    auto bytesMap = gcNew<PraiaMap>();
    registerBytesBuiltins(bytesMap);
    globals->define("bytes", Value(bytesMap));

    auto cryptoMap = gcNew<PraiaMap>();
    registerCryptoBuiltins(cryptoMap);
    globals->define("crypto", Value(cryptoMap));

    // ── random namespace ──
    //
    // Mersenne Twister, seeded from std::random_device. Fast and
    // uniform, suitable for simulations, games, shuffles, jitter, and
    // any other case where statistical quality is what matters.
    //
    // NOT suitable for security: tokens, session IDs, password salts,
    // UUIDs you treat as unguessable, anything attacker-facing. After
    // a few hundred outputs MT's internal state can be recovered, and
    // all future outputs predicted. Use `crypto.randomBytes(n)` for
    // those — it pulls from the OS CSPRNG (/dev/urandom + friends).

    auto randomMap = gcNew<PraiaMap>();
    auto rng = std::make_shared<std::mt19937>(std::random_device{}());

    randomMap->entries[Value("int")] = Value(makeNative("random.int", 2,
        [rng](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("random.int() requires two numbers", 0);
            int lo = static_cast<int>(args[0].asNumber());
            int hi = static_cast<int>(args[1].asNumber());
            std::uniform_int_distribution<int> dist(lo, hi);
            return Value(static_cast<int64_t>(dist(*rng)));
        }));

    randomMap->entries[Value("float")] = Value(makeNative("random.float", 0,
        [rng](const std::vector<Value>&) -> Value {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            return Value(dist(*rng));
        }));

    randomMap->entries[Value("choice")] = Value(makeNative("random.choice", 1,
        [rng](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("random.choice() requires an array", 0);
            auto& elems = args[0].asArray()->elements;
            if (elems.empty())
                throw RuntimeError("random.choice() on empty array", 0);
            std::uniform_int_distribution<size_t> dist(0, elems.size() - 1);
            return elems[dist(*rng)];
        }));

    randomMap->entries[Value("shuffle")] = Value(makeNative("random.shuffle", 1,
        [rng](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("random.shuffle() requires an array", 0);
            auto& elems = args[0].asArray()->elements;
            std::shuffle(elems.begin(), elems.end(), *rng);
            return args[0];
        }));

    randomMap->entries[Value("seed")] = Value(makeNative("random.seed", 1,
        [rng](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("random.seed() requires a number", 0);
            rng->seed(static_cast<unsigned>(args[0].asNumber()));
            return Value();
        }));

    globals->define("random", Value(randomMap));

    // ── time namespace ──

    auto timeMap = gcNew<PraiaMap>();

    timeMap->entries[Value("now")] = Value(makeNative("time.now", 0,
        [](const std::vector<Value>&) -> Value {
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            return Value(static_cast<int64_t>(ms));
        }));

    timeMap->entries[Value("sleep")] = Value(makeNative("time.sleep", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("time.sleep() requires milliseconds", 0);
            // Sleep in short bursts to remain responsive to SIGINT
            int remaining = static_cast<int>(args[0].asNumber());
            while (remaining > 0) {
                int chunk = remaining > 100 ? 100 : remaining;
                std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
                remaining -= chunk;
                if (g_pendingSignals.load(std::memory_order_relaxed) & (1u << SIGINT))
                    throw RuntimeError("Interrupted", 0);
            }
            return Value();
        }));

    timeMap->entries[Value("format")] = Value(makeNative("time.format", -1,
        [](const std::vector<Value>& args) -> Value {
            std::string fmt = "%Y-%m-%d %H:%M:%S";
            double timestamp = 0;

            if (!args.empty() && args[0].isString())
                fmt = args[0].asString();
            if (args.size() > 1 && args[1].isNumber())
                timestamp = args[1].asNumber();

            bool utc = args.size() > 2 && args[2].isTruthy();

            std::time_t t;
            if (timestamp > 0) {
                t = static_cast<std::time_t>(timestamp / 1000.0);
            } else {
                t = std::time(nullptr);
            }
            std::tm tm = utc ? *std::gmtime(&t) : *std::localtime(&t);
            std::ostringstream oss;
            oss << std::put_time(&tm, fmt.c_str());
            return Value(oss.str());
        }));

    timeMap->entries[Value("epoch")] = Value(makeNative("time.epoch", 0,
        [](const std::vector<Value>&) -> Value {
            return Value(static_cast<int64_t>(std::time(nullptr)));
        }));

    timeMap->entries[Value("parse")] = Value(makeNative("time.parse", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("time.parse() requires a date string", 0);
            const std::string& dateStr = args[0].asString();
            // args: (str, format?, utc?)
            bool utc = false;
            if (args.size() > 2 && args[2].isTruthy()) utc = true;
            // If second arg is bool (not string), treat as utc flag
            if (args.size() == 2 && args[1].isBool() && args[1].isTruthy()) utc = true;

            auto parseWith = [&](const std::string& format) -> std::pair<bool, int64_t> {
                std::tm tm = {};
                tm.tm_isdst = -1;
                std::istringstream iss(dateStr);
                iss >> std::get_time(&tm, format.c_str());
                if (iss.fail()) return {false, 0};
                std::time_t t;
                if (utc) {
                    t = timegm(&tm);
                } else {
                    t = std::mktime(&tm);
                }
                if (t == -1) return {false, 0};
                return {true, static_cast<int64_t>(t) * 1000};
            };

            if (args.size() > 1 && args[1].isString()) {
                auto [ok, ms] = parseWith(args[1].asString());
                if (!ok) throw RuntimeError("time.parse() failed to parse '" + dateStr + "'", 0);
                return Value(ms);
            }
            auto [ok1, ms1] = parseWith("%Y-%m-%d %H:%M:%S");
            if (ok1) return Value(ms1);
            auto [ok2, ms2] = parseWith("%Y-%m-%d");
            if (ok2) return Value(ms2);
            throw RuntimeError("time.parse() could not parse '" + dateStr + "'", 0);
        }));

    // Helper: convert ms timestamp to std::tm
    auto msToTm = [](const std::vector<Value>& args, const char* name) -> std::tm {
        if (args.empty() || !args[0].isNumber())
            throw RuntimeError(std::string(name) + " requires a millisecond timestamp", 0);
        std::time_t t = static_cast<std::time_t>(args[0].asNumber() / 1000.0);
        return *std::localtime(&t);
    };

    timeMap->entries[Value("year")] = Value(makeNative("time.year", 1,
        [msToTm](const std::vector<Value>& args) -> Value {
            return Value(static_cast<int64_t>(msToTm(args, "time.year()").tm_year + 1900));
        }));
    timeMap->entries[Value("month")] = Value(makeNative("time.month", 1,
        [msToTm](const std::vector<Value>& args) -> Value {
            return Value(static_cast<int64_t>(msToTm(args, "time.month()").tm_mon + 1));
        }));
    timeMap->entries[Value("day")] = Value(makeNative("time.day", 1,
        [msToTm](const std::vector<Value>& args) -> Value {
            return Value(static_cast<int64_t>(msToTm(args, "time.day()").tm_mday));
        }));
    timeMap->entries[Value("hour")] = Value(makeNative("time.hour", 1,
        [msToTm](const std::vector<Value>& args) -> Value {
            return Value(static_cast<int64_t>(msToTm(args, "time.hour()").tm_hour));
        }));
    timeMap->entries[Value("minute")] = Value(makeNative("time.minute", 1,
        [msToTm](const std::vector<Value>& args) -> Value {
            return Value(static_cast<int64_t>(msToTm(args, "time.minute()").tm_min));
        }));
    timeMap->entries[Value("second")] = Value(makeNative("time.second", 1,
        [msToTm](const std::vector<Value>& args) -> Value {
            return Value(static_cast<int64_t>(msToTm(args, "time.second()").tm_sec));
        }));
    timeMap->entries[Value("weekday")] = Value(makeNative("time.weekday", 1,
        [msToTm](const std::vector<Value>& args) -> Value {
            return Value(static_cast<int64_t>(msToTm(args, "time.weekday()").tm_wday));
        }));

    // Date arithmetic — returns new ms timestamp
    timeMap->entries[Value("addDays")] = Value(makeNative("time.addDays", 2,
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("time.addDays(timestamp, days) requires two numbers", 0);
            return Value(static_cast<int64_t>(args[0].asNumber() + args[1].asNumber() * 86400000.0));
        }));
    timeMap->entries[Value("addHours")] = Value(makeNative("time.addHours", 2,
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("time.addHours(timestamp, hours) requires two numbers", 0);
            return Value(static_cast<int64_t>(args[0].asNumber() + args[1].asNumber() * 3600000.0));
        }));
    timeMap->entries[Value("addMinutes")] = Value(makeNative("time.addMinutes", 2,
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("time.addMinutes(timestamp, minutes) requires two numbers", 0);
            return Value(static_cast<int64_t>(args[0].asNumber() + args[1].asNumber() * 60000.0));
        }));
    timeMap->entries[Value("addSeconds")] = Value(makeNative("time.addSeconds", 2,
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("time.addSeconds(timestamp, seconds) requires two numbers", 0);
            return Value(static_cast<int64_t>(args[0].asNumber() + args[1].asNumber() * 1000.0));
        }));

    // time.components(ts, utc?) — all components at once, optional UTC
    timeMap->entries[Value("components")] = Value(makeNative("time.components", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("time.components() requires a millisecond timestamp", 0);
            std::time_t t = static_cast<std::time_t>(args[0].asNumber() / 1000.0);
            bool utc = args.size() > 1 && args[1].isTruthy();
            std::tm tm = utc ? *std::gmtime(&t) : *std::localtime(&t);
            auto result = gcNew<PraiaMap>();
            result->entries[Value("year")]    = Value(static_cast<int64_t>(tm.tm_year + 1900));
            result->entries[Value("month")]   = Value(static_cast<int64_t>(tm.tm_mon + 1));
            result->entries[Value("day")]     = Value(static_cast<int64_t>(tm.tm_mday));
            result->entries[Value("hour")]    = Value(static_cast<int64_t>(tm.tm_hour));
            result->entries[Value("minute")]  = Value(static_cast<int64_t>(tm.tm_min));
            result->entries[Value("second")]  = Value(static_cast<int64_t>(tm.tm_sec));
            result->entries[Value("weekday")] = Value(static_cast<int64_t>(tm.tm_wday));
            return Value(result);
        }));

    globals->define("time", Value(timeMap));

    // ── math namespace (built-in, replaces grains/math.praia for C++ math) ──

    auto mathMap = gcNew<PraiaMap>();

    mathMap->entries[Value("PI")] = Value(3.14159265358979323846);
    mathMap->entries[Value("E")] = Value(2.71828182845904523536);
    mathMap->entries[Value("INF")] = Value(std::numeric_limits<double>::infinity());

    auto mathFn1 = [&](const std::string& name, double(*fn)(double)) {
        mathMap->entries[Value(name)] = Value(makeNative("math." + name, 1,
            [fn](const std::vector<Value>& args) -> Value {
                if (!args[0].isNumber())
                    throw RuntimeError("math function requires a number", 0);
                return Value(fn(args[0].asNumber()));
            }));
    };

    mathFn1("sqrt", std::sqrt);
    mathFn1("sin", std::sin);
    mathFn1("cos", std::cos);
    mathFn1("tan", std::tan);
    mathFn1("asin", std::asin);
    mathFn1("acos", std::acos);
    mathFn1("atan", std::atan);
    mathFn1("floor", std::floor);
    mathFn1("ceil", std::ceil);
    mathFn1("round", std::round);

    mathMap->entries[Value("trunc")] = Value(makeNative("math.trunc", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("math.trunc() requires a number", 0);
            return Value(static_cast<int64_t>(args[0].asNumber()));
        }));

    mathMap->entries[Value("idiv")] = Value(makeNative("math.idiv", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("math.idiv() requires two numbers", 0);
            if (args[1].asNumber() == 0)
                throw RuntimeError("Division by zero", 0);
            double result = args[0].asNumber() / args[1].asNumber();
            return Value(static_cast<int64_t>(result > 0 ? std::floor(result) : std::ceil(result)));
        }));
    mathFn1("abs", std::fabs);
    mathFn1("log", std::log);
    mathFn1("log2", std::log2);
    mathFn1("log10", std::log10);
    mathFn1("exp", std::exp);

    mathMap->entries[Value("pow")] = Value(makeNative("math.pow", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("math.pow() requires two numbers", 0);
            return Value(std::pow(args[0].asNumber(), args[1].asNumber()));
        }));

    mathMap->entries[Value("min")] = Value(makeNative("math.min", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("math.min() requires two numbers", 0);
            return Value(std::fmin(args[0].asNumber(), args[1].asNumber()));
        }));

    mathMap->entries[Value("max")] = Value(makeNative("math.max", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("math.max() requires two numbers", 0);
            return Value(std::fmax(args[0].asNumber(), args[1].asNumber()));
        }));

    mathMap->entries[Value("clamp")] = Value(makeNative("math.clamp", 3,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber() || !args[2].isNumber())
                throw RuntimeError("math.clamp() requires three numbers", 0);
            double x = args[0].asNumber(), lo = args[1].asNumber(), hi = args[2].asNumber();
            return Value(std::fmax(lo, std::fmin(x, hi)));
        }));

    mathMap->entries[Value("atan2")] = Value(makeNative("math.atan2", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("math.atan2() requires two numbers", 0);
            return Value(std::atan2(args[0].asNumber(), args[1].asNumber()));
        }));

    mathMap->entries[Value("isNan")] = Value(makeNative("math.isNan", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber()) return Value(false);
            return Value(std::isnan(args[0].asNumber()));
        }));

    mathMap->entries[Value("isInf")] = Value(makeNative("math.isInf", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber()) return Value(false);
            return Value(std::isinf(args[0].asNumber()));
        }));

    mathMap->entries[Value("approx")] = Value(makeNative("math.approx", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("math.approx() requires two numbers and an optional epsilon", 0);
            double a = args[0].asNumber(), b = args[1].asNumber();
            double epsilon = (args.size() >= 3 && args[2].isNumber()) ? args[2].asNumber() : 1e-9;
            return Value(std::fabs(a - b) < epsilon);
        }));

    globals->define("math", Value(mathMap));

    // ── OS extras on sys ──

    sysMap->entries[Value("currentFile")] = Value(makeNative("sys.currentFile", 0,
        [this](const std::vector<Value>&) -> Value {
            // Check VM first (bytecode engine), then tree-walker
            VM* vm = VM::current();
            if (vm && !vm->getCurrentFile().empty())
                return Value(vm->getCurrentFile());
            if (!currentFile.empty()) return Value(currentFile);
            return Value();
        }));

    sysMap->entries[Value("env")] = Value(makeNative("sys.env", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.env() requires a string", 0);
            const char* val = std::getenv(args[0].asString().c_str());
            if (!val) return Value();
            return Value(std::string(val));
        }));

    sysMap->entries[Value("setenv")] = Value(makeNative("sys.setenv", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("sys.setenv() requires two strings (key, value)", 0);
            if (setenv(args[0].asString().c_str(), args[1].asString().c_str(), 1) != 0)
                throw RuntimeError("sys.setenv() failed for key '" + args[0].asString() + "'", 0);
            return Value();
        }));

    sysMap->entries[Value("envAll")] = Value(makeNative("sys.envAll", 0,
        [](const std::vector<Value>&) -> Value {
            auto result = gcNew<PraiaMap>();
            extern char** environ;
            for (char** env = environ; *env; env++) {
                std::string entry(*env);
                auto eq = entry.find('=');
                if (eq != std::string::npos) {
                    result->entries[Value(entry.substr(0, eq))] = Value(entry.substr(eq + 1));
                }
            }
            return Value(result);
        }));

    sysMap->entries[Value("chdir")] = Value(makeNative("sys.chdir", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.chdir() requires a string path", 0);
            std::error_code ec;
            std::filesystem::current_path(args[0].asString(), ec);
            if (ec)
                throw RuntimeError("sys.chdir() failed: " + ec.message(), 0);
            return Value();
        }));

    sysMap->entries[Value("readLines")] = Value(makeNative("sys.readLines", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.readLines() requires a string path", 0);
            std::ifstream f(args[0].asString());
            if (!f.is_open())
                throw RuntimeError("Cannot read file: " + args[0].asString(), 0);
            auto result = gcNew<PraiaArray>();
            std::string line;
            while (std::getline(f, line))
                result->elements.push_back(Value(std::move(line)));
            return Value(result);
        }));

    sysMap->entries[Value("cwd")] = Value(makeNative("sys.cwd", 0,
        [](const std::vector<Value>&) -> Value {
            return Value(fs::current_path().string());
        }));

#if defined(__APPLE__)
    sysMap->entries[Value("platform")] = Value("darwin");
#elif defined(__linux__)
    sysMap->entries[Value("platform")] = Value("linux");
#elif defined(_WIN32)
    sysMap->entries[Value("platform")] = Value("windows");
#else
    sysMap->entries[Value("platform")] = Value("unknown");
#endif

    // sys.libdir — the library directory (where grains/stdlib live), or nil in dev mode
    if (g_praiaLibDir) {
        sysMap->entries[Value("libdir")] = Value(std::string(g_praiaLibDir));
    } else {
        sysMap->entries[Value("libdir")] = Value();
    }

    sysMap->entries[Value("stdout")] = Value(makeNative("sys.stdout", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.stdout() requires a string", 0);
            std::cout << args[0].asString() << std::flush;
            return Value();
        }));

    // ── Process identity ──

    sysMap->entries[Value("uid")] = Value(makeNative("sys.uid", 0,
        [](const std::vector<Value>&) -> Value {
            return Value(static_cast<int64_t>(geteuid()));
        }));

    sysMap->entries[Value("isRoot")] = Value(makeNative("sys.isRoot", 0,
        [](const std::vector<Value>&) -> Value {
            return Value(geteuid() == 0);
        }));

    sysMap->entries[Value("getpid")] = Value(makeNative("sys.getpid", 0,
        [](const std::vector<Value>&) -> Value {
            return Value(static_cast<int64_t>(getpid()));
        }));

    // ── Signal handling ──

    // sys.onSignal(name, handler) — register a callback for a signal
    // handler receives the signal name as an argument
    sysMap->entries[Value("onSignal")] = Value(makeNative("sys.onSignal", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.onSignal() first argument must be a signal name string", 0);
            if (!args[1].isCallable() && !args[1].isNil())
                throw RuntimeError("sys.onSignal() second argument must be a function or nil", 0);

            int sig = signalNameToNum(args[0].asString());
            if (sig < 0)
                throw RuntimeError("sys.onSignal(): unknown signal '" + args[0].asString() +
                    "'. Valid: SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2", 0);

            std::lock_guard<std::mutex> lock(g_signalMutex);
            if (args[1].isNil()) {
                // Remove handler, restore default
                g_signalHandlers.erase(sig);
                signal(sig, SIG_DFL);
            } else {
                g_signalHandlers[sig] = args[1].asCallable();
                struct sigaction sa = {};
                sa.sa_handler = praiaSignalHandler;
                sigemptyset(&sa.sa_mask);
                sa.sa_flags = 0;
                sigaction(sig, &sa, nullptr);
            }
            return Value();
        }));

    // sys.kill(pid, signal?) — send a signal to another process. The signal
    // is "TERM" / "INT" / "KILL" / etc. (or any name `sys.onSignal` accepts);
    // it defaults to "TERM" if omitted. Returns true if the signal was sent.
    sysMap->entries[Value("kill")] = Value(makeNative("sys.kill", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("sys.kill(pid, signal?) requires a pid", 0);
            int pid = static_cast<int>(args[0].asNumber());
            int sig = SIGTERM;
            if (args.size() > 1) {
                if (args[1].isNumber()) {
                    sig = static_cast<int>(args[1].asNumber());
                } else if (args[1].isString()) {
                    sig = signalNameToNum(args[1].asString());
                    if (sig < 0)
                        throw RuntimeError("sys.kill(): unknown signal '" +
                                           args[1].asString() + "'", 0);
                } else {
                    throw RuntimeError("sys.kill(): signal must be a name or number", 0);
                }
            }
            int rc = ::kill(static_cast<pid_t>(pid), sig);
            return Value(rc == 0);
        }));

    // sys.signal(name) — send a signal to the current process (for testing)
    sysMap->entries[Value("signal")] = Value(makeNative("sys.signal", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.signal() requires a signal name string", 0);
            int sig = signalNameToNum(args[0].asString());
            if (sig < 0)
                throw RuntimeError("sys.signal(): unknown signal '" + args[0].asString() + "'", 0);
            raise(sig);
            return Value();
        }));

    // sys.checkSignals() — process any pending signals by calling registered handlers.
    // Call this in long-running loops to allow signal callbacks to run.
    sysMap->entries[Value("checkSignals")] = Value(makeNative("sys.checkSignals", 0,
        [self](const std::vector<Value>&) -> Value {
            uint32_t pending = g_pendingSignals.exchange(0);
            if (pending == 0) return Value(false);

            // Copy handlers under the lock, then release before invoking
            // so that callbacks can safely call sys.onSignal() without
            // deadlocking on g_signalMutex.
            std::vector<std::pair<int, std::shared_ptr<Callable>>> toCall;
            {
                std::lock_guard<std::mutex> lock(g_signalMutex);
                for (int sig = 0; sig < 32 && pending; sig++) {
                    if (pending & (1u << sig)) {
                        pending &= ~(1u << sig);
                        auto it = g_signalHandlers.find(sig);
                        if (it != g_signalHandlers.end())
                            toCall.emplace_back(sig, it->second);
                    }
                }
            }
            for (auto& [sig, handler] : toCall) {
                std::string name = signalNumToName(sig);
                callSafe(*self, handler, {Value(name)});
            }
            return Value(true);
        }));

    // ── Terminal I/O ──

    // Store the original terminal settings so we can restore them
    auto origTermios = std::make_shared<struct termios>();
    auto rawModeActive = std::make_shared<bool>(false);
    tcgetattr(STDIN_FILENO, origTermios.get());

    sysMap->entries[Value("rawMode")] = Value(makeNative("sys.rawMode", 1,
        [origTermios, rawModeActive](const std::vector<Value>& args) -> Value {
            if (!args[0].isBool())
                throw RuntimeError("sys.rawMode() requires a boolean", 0);
            if (args[0].asBool()) {
                struct termios raw = *origTermios;
                raw.c_lflag &= ~(ECHO | ICANON | ISIG);
                raw.c_iflag &= ~(IXON | ICRNL);
                raw.c_cc[VMIN] = 0;
                raw.c_cc[VTIME] = 0;
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
                *rawModeActive = true;
            } else {
                tcsetattr(STDIN_FILENO, TCSAFLUSH, origTermios.get());
                *rawModeActive = false;
            }
            return Value();
        }));

    sysMap->entries[Value("readKey")] = Value(makeNative("sys.readKey", 0,
        [](const std::vector<Value>&) -> Value {
            // Block until at least one byte
            struct termios prev;
            tcgetattr(STDIN_FILENO, &prev);
            struct termios t = prev;
            t.c_cc[VMIN] = 1;
            t.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);

            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n <= 0) { tcsetattr(STDIN_FILENO, TCSANOW, &prev); return Value(); }

            // If it's an ESC, try to read the rest of the escape sequence
            if (c == '\x1b') {
                // Switch to non-blocking with short timeout to grab remaining bytes
                t.c_cc[VMIN] = 0;
                t.c_cc[VTIME] = 1; // 100ms timeout
                tcsetattr(STDIN_FILENO, TCSANOW, &t);

                char seq[7] = {};
                ssize_t seqLen = read(STDIN_FILENO, seq, sizeof(seq) - 1);
                tcsetattr(STDIN_FILENO, TCSANOW, &prev);

                if (seqLen > 0) {
                    std::string result(1, c);
                    result.append(seq, seqLen);
                    return Value(std::move(result));
                }
                // Bare ESC
                return Value(std::string(1, c));
            }

            tcsetattr(STDIN_FILENO, TCSANOW, &prev);
            return Value(std::string(1, c));
        }));

    sysMap->entries[Value("termSize")] = Value(makeNative("sys.termSize", 0,
        [](const std::vector<Value>&) -> Value {
            struct winsize ws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) {
                auto result = gcNew<PraiaMap>();
                result->entries[Value("rows")] = Value(static_cast<int64_t>(24));
                result->entries[Value("cols")] = Value(static_cast<int64_t>(80));
                return Value(result);
            }
            auto result = gcNew<PraiaMap>();
            result->entries[Value("rows")] = Value(static_cast<int64_t>(ws.ws_row));
            result->entries[Value("cols")] = Value(static_cast<int64_t>(ws.ws_col));
            return Value(result);
        }));

    // ── sqlite namespace ──

#ifdef HAVE_SQLITE
    auto sqliteMap = gcNew<PraiaMap>();

    sqliteMap->entries[Value("open")] = Value(makeNative("sqlite.open", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sqlite.open() requires a path string", 0);

            sqlite3* raw = nullptr;
            int rc = sqlite3_open(args[0].asString().c_str(), &raw);
            if (rc != SQLITE_OK) {
                std::string err = sqlite3_errmsg(raw);
                sqlite3_close(raw);
                throw RuntimeError("Cannot open database: " + err, 0);
            }

            // Wrap in shared_ptr for automatic cleanup
            auto db = std::make_shared<sqlite3*>(raw);

            auto dbMap = gcNew<PraiaMap>();

            // db.query(sql, params?) → array of maps
            dbMap->entries[Value("query")] = Value(makeNative("query", -1,
                [db](const std::vector<Value>& args) -> Value {
                    if (args.empty() || !args[0].isString())
                        throw RuntimeError("query() requires a SQL string", 0);
                    if (!*db)
                        throw RuntimeError("Database is closed", 0);

                    sqlite3_stmt* stmt = nullptr;
                    int rc = sqlite3_prepare_v2(*db, args[0].asString().c_str(), -1, &stmt, nullptr);
                    if (rc != SQLITE_OK) {
                        std::string err = sqlite3_errmsg(*db);
                        throw RuntimeError("SQL error: " + err, 0);
                    }

                    // Bind parameters
                    if (args.size() > 1 && args[1].isArray()) {
                        auto& params = args[1].asArray()->elements;
                        for (size_t i = 0; i < params.size(); i++) {
                            int idx = static_cast<int>(i + 1);
                            auto& p = params[i];
                            if (p.isNil()) sqlite3_bind_null(stmt, idx);
                            else if (p.isBool()) sqlite3_bind_int(stmt, idx, p.asBool() ? 1 : 0);
                            else if (p.isNumber()) sqlite3_bind_double(stmt, idx, p.asNumber());
                            else if (p.isString()) sqlite3_bind_text(stmt, idx, p.asString().c_str(), -1, SQLITE_TRANSIENT);
                            else sqlite3_bind_text(stmt, idx, p.toString().c_str(), -1, SQLITE_TRANSIENT);
                        }
                    }

                    // Execute and collect rows
                    auto rows = gcNew<PraiaArray>();
                    int cols = sqlite3_column_count(stmt);

                    while (sqlite3_step(stmt) == SQLITE_ROW) {
                        auto row = gcNew<PraiaMap>();
                        for (int c = 0; c < cols; c++) {
                            std::string name = sqlite3_column_name(stmt, c);
                            int type = sqlite3_column_type(stmt, c);
                            switch (type) {
                                case SQLITE_NULL:
                                    row->entries[Value(name)] = Value();
                                    break;
                                case SQLITE_INTEGER:
                                    row->entries[Value(name)] = Value(static_cast<double>(sqlite3_column_int64(stmt, c)));
                                    break;
                                case SQLITE_FLOAT:
                                    row->entries[Value(name)] = Value(sqlite3_column_double(stmt, c));
                                    break;
                                default:
                                    row->entries[Value(name)] = Value(std::string(
                                        reinterpret_cast<const char*>(sqlite3_column_text(stmt, c))));
                                    break;
                            }
                        }
                        rows->elements.push_back(Value(row));
                    }

                    sqlite3_finalize(stmt);
                    return Value(rows);
                }));

            // db.run(sql, params?) → {changes, lastId}
            dbMap->entries[Value("run")] = Value(makeNative("run", -1,
                [db](const std::vector<Value>& args) -> Value {
                    if (args.empty() || !args[0].isString())
                        throw RuntimeError("run() requires a SQL string", 0);
                    if (!*db)
                        throw RuntimeError("Database is closed", 0);

                    sqlite3_stmt* stmt = nullptr;
                    int rc = sqlite3_prepare_v2(*db, args[0].asString().c_str(), -1, &stmt, nullptr);
                    if (rc != SQLITE_OK) {
                        std::string err = sqlite3_errmsg(*db);
                        throw RuntimeError("SQL error: " + err, 0);
                    }

                    // Bind parameters
                    if (args.size() > 1 && args[1].isArray()) {
                        auto& params = args[1].asArray()->elements;
                        for (size_t i = 0; i < params.size(); i++) {
                            int idx = static_cast<int>(i + 1);
                            auto& p = params[i];
                            if (p.isNil()) sqlite3_bind_null(stmt, idx);
                            else if (p.isBool()) sqlite3_bind_int(stmt, idx, p.asBool() ? 1 : 0);
                            else if (p.isNumber()) sqlite3_bind_double(stmt, idx, p.asNumber());
                            else if (p.isString()) sqlite3_bind_text(stmt, idx, p.asString().c_str(), -1, SQLITE_TRANSIENT);
                            else sqlite3_bind_text(stmt, idx, p.toString().c_str(), -1, SQLITE_TRANSIENT);
                        }
                    }

                    rc = sqlite3_step(stmt);
                    sqlite3_finalize(stmt);

                    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                        std::string err = sqlite3_errmsg(*db);
                        throw RuntimeError("SQL error: " + err, 0);
                    }

                    auto result = gcNew<PraiaMap>();
                    result->entries[Value("changes")] = Value(static_cast<int64_t>(sqlite3_changes(*db)));
                    result->entries[Value("lastId")] = Value(static_cast<int64_t>(sqlite3_last_insert_rowid(*db)));
                    return Value(result);
                }));

            // db.close()
            dbMap->entries[Value("close")] = Value(makeNative("close", 0,
                [db](const std::vector<Value>&) -> Value {
                    if (*db) {
                        sqlite3_close(*db);
                        *db = nullptr;
                    }
                    return Value();
                }));

            return Value(dbMap);
        }));

    globals->define("sqlite", Value(sqliteMap));
#endif

    // ── loadNative(path) — load a native C++ plugin ──
    globals->define("loadNative", Value(makeNative("loadNative", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("loadNative() requires a string path", 0);

            std::string path = args[0].asString();

            // Auto-resolve platform extension if omitted
            if (!fs::exists(path)) {
#ifdef __APPLE__
                if (fs::exists(path + ".dylib")) path += ".dylib";
                else if (fs::exists(path + ".so")) path += ".so";
#else
                if (fs::exists(path + ".so")) path += ".so";
                else if (fs::exists(path + ".dylib")) path += ".dylib";
#endif
            }

            // Resolve to absolute path for consistent caching
            std::string absPath;
            try {
                absPath = fs::canonical(path).string();
            } catch (const std::filesystem::filesystem_error&) {
                throw RuntimeError("loadNative(): file not found: " + path, 0);
            }

            // Lock for the entire load to prevent double-loading
            std::lock_guard<std::mutex> lock(g_pluginMutex);

            // Check cache
            auto it = g_pluginCache.find(absPath);
            if (it != g_pluginCache.end())
                return Value(it->second);

            // dlopen
            void* handle = dlopen(absPath.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!handle) {
                std::string err = dlerror();
                throw RuntimeError("loadNative(): failed to load '" + path + "': " + err, 0);
            }

            // dlsym for the entry point. If this fails or returns null, no
            // plugin code has run yet — safe to dlclose so the handle doesn't
            // leak. Once we call registerFn we MUST keep the handle alive
            // forever: the plugin's callables (and even the plugin-built
            // std::function deleters in their destructors) hold pointers
            // into plugin code that would dangle if unloaded.
            using RegisterFn = void (*)(PraiaMap*);
            dlerror(); // clear any old error
            auto registerFn = reinterpret_cast<RegisterFn>(dlsym(handle, "praia_register"));
            const char* dlErr = dlerror();
            if (dlErr || !registerFn) {
                std::string sym_err = dlErr ? std::string(dlErr) : "praia_register is null";
                dlclose(handle);
                throw RuntimeError(
                    "loadNative(): plugin '" + path +
                    "' missing 'praia_register' symbol: " + sym_err, 0);
            }

            // Past the point of no return: track the handle BEFORE calling
            // registerFn so a partial/throwing register still records the
            // handle (never dlclose'd, but at least visible for diagnostics).
            g_pluginHandles.push_back(handle);

            auto moduleMap = gcNew<PraiaMap>();
            try {
                registerFn(moduleMap.get());
            } catch (const std::exception& e) {
                // Don't dlclose here — plugin code may still be referenced
                // by std::function deleters in the partially-populated
                // moduleMap. Just wrap and re-throw with context.
                throw RuntimeError(
                    "loadNative(): plugin '" + path +
                    "' threw during registration: " + std::string(e.what()), 0);
            }

            // Success — cache the module
            g_pluginCache[absPath] = moduleMap;

            return Value(moduleMap);
        })));
}

void Interpreter::setArgs(const std::vector<std::string>& args) {
    auto arr = gcNew<PraiaArray>();
    for (auto& a : args)
        arr->elements.push_back(Value(a));
    sysMap->entries[Value("args")] = Value(arr);
}

void Interpreter::setCurrentFile(const std::string& path) {
    currentFile = fs::absolute(path).string();
}
