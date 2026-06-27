#include "builtins.h"
#include "builtins/scope_guards.h"
#include "deprecation.h"
#include "gc_heap.h"
#include "grain_resolve.h"
#include "interpreter.h"
#include "praia_plugin.h"  // PRAIA_PLUGIN_ABI_VERSION for loadNative
#include "unicode.h"
#include "url.h"
#include "vm/vm.h"
#ifdef HAVE_UTF8PROC
#include <utf8proc.h>
#endif
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <climits>
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
#include <fcntl.h>
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

// praia_at_exit hooks, paired with the plugin path that registered
// them. Path is kept for diagnostic wording when a hook throws.
// Vector order matches load order; runPluginExitHooks walks it in
// reverse so plugins loaded later tear down first (LIFO, mirrors C's
// atexit and what plugin authors expect from layered dependencies).
struct PluginAtExitEntry {
    void (*fn)();
    std::string pluginPath;
};
static std::vector<PluginAtExitEntry> g_pluginAtExitHooks;

// Set once by runPluginExitHooks under g_pluginMutex when teardown
// begins. loadNative checks it (also under g_pluginMutex) and
// refuses to register new hooks past that point — so a late call
// (theoretical under current ordering, possible under future
// threading-model changes) can't enqueue a hook the drain loop has
// already passed.
static bool g_pluginShutdown = false;

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
            if (args[0].isSet())
                return Value(static_cast<int64_t>(args[0].asSet()->elements.size()));
            throw RuntimeError("len() requires an array, string, map, or set", 0);
        })));

    // `assert(cond, msg?)` — always-on production assertion.
    // Falsy `cond` throws a RuntimeError; the optional `msg` is
    // appended to the default text. Distinct from
    // `testing.expect`, which counts pass/fail for the test
    // runner instead of throwing.
    globals->define("assert", Value(makeNative("assert", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || args.size() > 2)
                throw RuntimeError("assert() expects 1 or 2 arguments (condition, message?)", 0);
            if (args[0].isTruthy()) return Value();
            // toString on the message handles non-string values
            // (numbers, maps, errors caught from elsewhere)
            // without forcing the user to stringify by hand.
            std::string msg = args.size() == 2
                ? "assertion failed: " + args[1].toString()
                : "assertion failed";
            throw RuntimeError(msg, 0);
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
            if (v.isSet())      return Value("set");
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
                                 : args[0].isSet()      ? "set"
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
                                 : v.isSet()      ? "set"
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

    // The functional natives below dispatch callbacks through g_currentInterp
    // rather than capturing `this` at registration time. Capturing `this` would
    // route async-task invocations back through the parent Interpreter on the
    // task thread, racing on its env/savedEnvStack_. See the comment on
    // g_currentInterp in interpreter.h.

    globals->define("sort", Value(makeNative("sort", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isArray())
                throw RuntimeError("sort() requires an array", 0);
            auto sorted = gcNew<PraiaArray>();
            sorted->elements = args[0].asArray()->elements;
            auto& elems = sorted->elements;

            if (args.size() > 1 && args[1].isCallable()) {
                auto cmp = args[1].asCallable();
                std::sort(elems.begin(), elems.end(),
                    [&cmp](const Value& a, const Value& b) -> bool {
                        Value result = callSafe(*g_currentInterp, cmp, {a, b});
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
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("filter() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("filter() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto result = gcNew<PraiaArray>();
            auto pred = args[1].asCallable();
            for (auto& elem : src) {
                Value test = callSafe(*g_currentInterp, pred, {elem});
                if (test.isTruthy()) result->elements.push_back(elem);
            }
            return Value(result);
        })));

    globals->define("map", Value(makeNative("map", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("map() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("map() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto result = gcNew<PraiaArray>();
            auto transform = args[1].asCallable();
            for (auto& elem : src)
                result->elements.push_back(callSafe(*g_currentInterp, transform, {elem}));
            return Value(result);
        })));

    globals->define("each", Value(makeNative("each", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("each() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("each() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto fn = args[1].asCallable();
            for (auto& elem : src)
                callSafe(*g_currentInterp, fn, {elem});
            return args[0]; // return the array for chaining
        })));

    globals->define("reduce", Value(makeNative("reduce", -1,
        [](const std::vector<Value>& args) -> Value {
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
                acc = callSafe(*g_currentInterp, fn, {acc, src[i]});
            return acc;
        })));

    globals->define("any", Value(makeNative("any", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("any() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("any() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto pred = args[1].asCallable();
            for (auto& elem : src)
                if (callSafe(*g_currentInterp, pred, {elem}).isTruthy()) return Value(true);
            return Value(false);
        })));

    globals->define("all", Value(makeNative("all", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("all() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("all() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto pred = args[1].asCallable();
            for (auto& elem : src)
                if (!callSafe(*g_currentInterp, pred, {elem}).isTruthy()) return Value(false);
            return Value(true);
        })));

    globals->define("flatMap", Value(makeNative("flatMap", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("flatMap() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("flatMap() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto fn = args[1].asCallable();
            auto result = gcNew<PraiaArray>();
            for (auto& elem : src) {
                Value mapped = callSafe(*g_currentInterp, fn, {elem});
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
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("groupBy() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("groupBy() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto fn = args[1].asCallable();
            auto result = gcNew<PraiaMap>();
            for (auto& elem : src) {
                Value key = callSafe(*g_currentInterp, fn, {elem});
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
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("findIndex() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("findIndex() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto pred = args[1].asCallable();
            for (size_t i = 0; i < src.size(); i++)
                if (callSafe(*g_currentInterp, pred, {src[i]}).isTruthy())
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

    // ── fs namespace ──
    //
    // The canonical home for filesystem I/O. sys.* used to absorb
    // these (read/write/append/exists/mkdir/tempDir/remove/readDir/
    // copy/move/readLines) but sys had become the kitchen sink of
    // unrelated concerns — process control, env vars, signals,
    // execvp, AND filesystem ops. fs.* is the new canonical name;
    // the sys.* aliases below stay as one-shot-deprecation forwarders
    // so existing callers keep working. Remove the aliases at 1.0.
    auto fsMap = gcNew<PraiaMap>();

    // Each impl is captured as a std::function so the same body
    // serves both fs.<name> (canonical) and the sys.<name> deprecated
    // forwarder. Error messages name the canonical fs.<name> form —
    // when a sys.<name> call fails the user sees the deprecation
    // banner first and the rename guidance in the error second.
    using FsImpl = std::function<Value(const std::vector<Value>&)>;

    FsImpl fsRead = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.read() requires a string path", 0);
        std::ifstream f(args[0].asString());
        if (!f.is_open())
            throw RuntimeError("Cannot read file: " + args[0].asString(), 0);
        std::stringstream ss;
        ss << f.rdbuf();
        return Value(ss.str());
    };

    FsImpl fsWrite = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.write() requires a string path", 0);
        std::ofstream f(args[0].asString());
        if (!f.is_open())
            throw RuntimeError("Cannot write file: " + args[0].asString(), 0);
        f << args[1].toString();
        return Value();
    };

    FsImpl fsAppend = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.append() requires a string path", 0);
        std::ofstream f(args[0].asString(), std::ios::app);
        if (!f.is_open())
            throw RuntimeError("Cannot open file: " + args[0].asString(), 0);
        f << args[1].toString();
        return Value();
    };

    FsImpl fsExists = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.exists() requires a string path", 0);
        return Value(fs::exists(args[0].asString()));
    };

    FsImpl fsMkdir = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.mkdir() requires a string path", 0);
        fs::create_directories(args[0].asString());
        return Value();
    };

    // fs.tempDir(prefix?) — atomic mkdtemp(3) under the system temp
    // dir, mode 0700. Caller is responsible for removal.
    FsImpl fsTempDir = [](const std::vector<Value>& args) -> Value {
        std::string prefix = "praia";
        if (!args.empty()) {
            if (!args[0].isString())
                throw RuntimeError("fs.tempDir() prefix must be a string", 0);
            prefix = args[0].asString();
        }
        std::error_code ec;
        auto tmpPath = fs::temp_directory_path(ec);
        if (ec)
            throw RuntimeError("fs.tempDir(): " + ec.message(), 0);
        std::string tmpl = tmpPath.string() + "/" + prefix + ".XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (!mkdtemp(buf.data()))
            throw RuntimeError("fs.tempDir(): " + std::string(std::strerror(errno)), 0);
        return Value(std::string(buf.data()));
    };

    FsImpl fsRemove = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.remove() requires a string path", 0);
        auto& p = args[0].asString();
        if (!fs::exists(p))
            throw RuntimeError("Cannot remove: " + p + " (not found)", 0);
        fs::remove_all(p);
        return Value();
    };

    FsImpl fsReadDir = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.readDir() requires a string path", 0);
        auto& p = args[0].asString();
        if (!fs::is_directory(p))
            throw RuntimeError("fs.readDir(): not a directory: " + p, 0);
        auto arr = gcNew<PraiaArray>();
        for (auto& entry : fs::directory_iterator(p))
            arr->elements.push_back(Value(entry.path().filename().string()));
        return Value(arr);
    };

    FsImpl fsCopy = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString() || !args[1].isString())
            throw RuntimeError("fs.copy() requires two string paths", 0);
        auto& src = args[0].asString();
        auto& dst = args[1].asString();
        if (!fs::exists(src))
            throw RuntimeError("Cannot copy: " + src + " (not found)", 0);
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        return Value();
    };

    FsImpl fsMove = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString() || !args[1].isString())
            throw RuntimeError("fs.move() requires two string paths", 0);
        auto& src = args[0].asString();
        auto& dst = args[1].asString();
        if (!fs::exists(src))
            throw RuntimeError("Cannot move: " + src + " (not found)", 0);
        fs::rename(src, dst);
        return Value();
    };

    // ── Phase 2: stat, lstat, chmod, symlink, readlink, atomicWrite, mktemp ──
    //
    // These hit raw POSIX rather than std::filesystem because we want the
    // full stat(2) shape (uid/gid/atime/ctime/nlink/ino/dev) and the
    // atomicity guarantees that std::filesystem's higher-level wrappers
    // either hide or implement on a best-effort basis.

    // Convert a struct stat into the Praia map shape exposed to user
    // code. The type string covers all POSIX file types so callers can
    // dispatch without having to know the bit layout of st_mode. mode
    // is masked to 0o7777 (low 12 bits = permission + setuid/setgid/
    // sticky) — the file-type bits live in `.type` instead so callers
    // don't have to do their own bit-twiddling.
    auto statToMap = [](const struct stat& st) -> Value {
        auto m = gcNew<PraiaMap>();
        std::string typeStr;
        if      (S_ISREG(st.st_mode))  typeStr = "file";
        else if (S_ISDIR(st.st_mode))  typeStr = "dir";
        else if (S_ISLNK(st.st_mode))  typeStr = "symlink";
        else if (S_ISSOCK(st.st_mode)) typeStr = "socket";
        else if (S_ISFIFO(st.st_mode)) typeStr = "fifo";
        else if (S_ISBLK(st.st_mode))  typeStr = "block";
        else if (S_ISCHR(st.st_mode))  typeStr = "char";
        else                           typeStr = "unknown";
        m->entries[Value("type")]  = Value(typeStr);
        m->entries[Value("size")]  = Value(static_cast<int64_t>(st.st_size));
        m->entries[Value("mode")]  = Value(static_cast<int64_t>(st.st_mode & 07777));
        m->entries[Value("uid")]   = Value(static_cast<int64_t>(st.st_uid));
        m->entries[Value("gid")]   = Value(static_cast<int64_t>(st.st_gid));
        m->entries[Value("mtime")] = Value(static_cast<int64_t>(st.st_mtime));
        m->entries[Value("atime")] = Value(static_cast<int64_t>(st.st_atime));
        m->entries[Value("ctime")] = Value(static_cast<int64_t>(st.st_ctime));
        m->entries[Value("nlink")] = Value(static_cast<int64_t>(st.st_nlink));
        m->entries[Value("ino")]   = Value(static_cast<int64_t>(st.st_ino));
        m->entries[Value("dev")]   = Value(static_cast<int64_t>(st.st_dev));
        return Value(m);
    };

    FsImpl fsStat = [statToMap](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.stat() requires a string path", 0);
        auto& p = args[0].asString();
        struct stat st;
        if (::stat(p.c_str(), &st) != 0)
            throw RuntimeError("fs.stat(): " + p + ": " + std::strerror(errno), 0);
        return statToMap(st);
    };

    // lstat differs from stat only in symlink handling: it returns
    // metadata for the link itself rather than its target. Use this
    // when "type" should distinguish symlinks from regular files.
    FsImpl fsLstat = [statToMap](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.lstat() requires a string path", 0);
        auto& p = args[0].asString();
        struct stat st;
        if (::lstat(p.c_str(), &st) != 0)
            throw RuntimeError("fs.lstat(): " + p + ": " + std::strerror(errno), 0);
        return statToMap(st);
    };

    // chmod(2). Mode is an integer; Praia doesn't have octal literals
    // yet so users will write either decimal (420 == 0o644) or hex
    // (0x1A4). Mask the user-supplied bits so we can't accidentally
    // pass through stat-style file-type bits (chmod ignores them but
    // it's a sharper API).
    FsImpl fsChmod = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.chmod() requires a string path", 0);
        if (!args[1].isNumber())
            throw RuntimeError("fs.chmod() requires a numeric mode", 0);
        auto& p = args[0].asString();
        mode_t mode = static_cast<mode_t>(args[1].toInt64ForBitwise()) & 07777;
        if (::chmod(p.c_str(), mode) != 0)
            throw RuntimeError("fs.chmod(): " + p + ": " + std::strerror(errno), 0);
        return Value();
    };

    // symlink(2). Note POSIX argument order: (target, linkpath) — the
    // target string is stored verbatim and isn't checked for validity
    // (dangling symlinks are legal). linkpath is the filesystem
    // location to create.
    FsImpl fsSymlink = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString() || !args[1].isString())
            throw RuntimeError("fs.symlink(target, linkpath) requires two string paths", 0);
        auto& target   = args[0].asString();
        auto& linkpath = args[1].asString();
        if (::symlink(target.c_str(), linkpath.c_str()) != 0)
            throw RuntimeError("fs.symlink(): " + linkpath + ": " + std::strerror(errno), 0);
        return Value();
    };

    // readlink(2). The returned string is whatever the symlink stores
    // — relative or absolute, possibly dangling. Throws on a non-link.
    FsImpl fsReadlink = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.readlink() requires a string path", 0);
        auto& p = args[0].asString();
        // PATH_MAX-sized scratch then grow if the link is longer (rare
        // but possible — Linux allows symlink contents up to ~4096
        // bytes; macOS allows somewhat less but still over PATH_MAX
        // for our purposes).
        std::vector<char> buf(1024);
        while (true) {
            ssize_t n = ::readlink(p.c_str(), buf.data(), buf.size());
            if (n < 0)
                throw RuntimeError("fs.readlink(): " + p + ": " + std::strerror(errno), 0);
            if (static_cast<size_t>(n) < buf.size())
                return Value(std::string(buf.data(), n));
            // Buffer was exactly filled — symlink may have been
            // truncated. Double the buffer and retry.
            buf.resize(buf.size() * 2);
        }
    };

    // fs.atomicWrite(path, content) — write content to a sibling temp
    // file, fsync, then rename(2) onto path. POSIX rename is atomic on
    // the same filesystem: readers see either the old contents or the
    // new ones, never a half-written file. Closes the "config
    // truncated by Ctrl-C / power loss" hole.
    //
    // On failure anywhere, unlink the temp file so we don't leave
    // .tmp.XXXXXX droppings behind.
    FsImpl fsAtomicWrite = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.atomicWrite() requires a string path", 0);
        auto& targetPath = args[0].asString();
        // Allow strings or bytes for content. toString() copes with both;
        // for bytes we want raw payload not a stringified representation,
        // but Praia's bytes Value stringifies to its raw bytes already.
        std::string content = args[1].toString();

        // Sibling temp file — must be on the same filesystem as the
        // target so rename(2) stays atomic. Derive a template from
        // the target's parent directory + basename.
        fs::path tgt(targetPath);
        fs::path dir = tgt.parent_path();
        if (dir.empty()) dir = ".";
        std::string tmpl = (dir / ("." + tgt.filename().string() + ".XXXXXX")).string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');

        int fd = ::mkstemp(buf.data());
        if (fd < 0)
            throw RuntimeError("fs.atomicWrite(): mkstemp failed for " + targetPath +
                               ": " + std::strerror(errno), 0);
        std::string tmpPath(buf.data());

        // Lambda to unlink the temp on any error path before
        // re-throwing.
        auto bail = [&](const std::string& msg) {
            ::close(fd);
            ::unlink(tmpPath.c_str());
            throw RuntimeError("fs.atomicWrite(): " + msg, 0);
        };

        // mkstemp creates with mode 0600; loosen to 0644 so the
        // renamed file matches what `fs.write` produces. Users who
        // want stricter perms can chmod afterwards.
        if (::fchmod(fd, 0644) != 0)
            bail("fchmod failed: " + std::string(std::strerror(errno)));

        // Write the full content. write(2) can return short; loop
        // until done or error.
        const char* p = content.data();
        size_t remaining = content.size();
        while (remaining > 0) {
            ssize_t n = ::write(fd, p, remaining);
            if (n < 0) {
                if (errno == EINTR) continue;
                bail("write failed: " + std::string(std::strerror(errno)));
            }
            p += n;
            remaining -= static_cast<size_t>(n);
        }

        // fsync the data before the rename. Without this the rename
        // happens in cache but the new contents may not be on disk
        // when the rename's metadata commit lands — a crash window
        // where readers see an empty file. fsync closes that window.
        if (::fsync(fd) != 0)
            bail("fsync failed: " + std::string(std::strerror(errno)));
        if (::close(fd) != 0) {
            ::unlink(tmpPath.c_str());
            throw RuntimeError("fs.atomicWrite(): close failed: " +
                               std::string(std::strerror(errno)), 0);
        }

        // The atomic step. On the same filesystem this is one
        // metadata commit; readers see old-or-new, nothing in
        // between.
        if (::rename(tmpPath.c_str(), targetPath.c_str()) != 0) {
            std::string err = std::strerror(errno);
            ::unlink(tmpPath.c_str());
            throw RuntimeError("fs.atomicWrite(): rename to " + targetPath +
                               " failed: " + err, 0);
        }

        // fsync the parent directory so the rename's directory-entry
        // update is durable. Without this, a power-loss after a
        // successful rename(2) can revert to the pre-rename listing
        // — the data is on disk (we fsynced fd above) but the
        // directory's pointer to it isn't. "Atomic for readers"
        // already held; this extends the guarantee to "durable across
        // crashes." On platforms where directory fsync is unsupported
        // we surface the error rather than silently downgrading the
        // contract.
        int dirFd = ::open(dir.string().c_str(), O_RDONLY | O_DIRECTORY);
        if (dirFd < 0)
            throw RuntimeError("fs.atomicWrite(): open parent dir for fsync failed: " +
                               std::string(std::strerror(errno)), 0);
        if (::fsync(dirFd) != 0) {
            int e = errno;
            ::close(dirFd);
            throw RuntimeError("fs.atomicWrite(): fsync parent dir failed: " +
                               std::string(std::strerror(e)), 0);
        }
        ::close(dirFd);
        return Value();
    };

    // ── Phase 3: fs.open + FileHandle (streaming I/O) ──
    //
    // FileHandle wraps a POSIX fd plus a small read buffer so methods
    // like readLine() don't pay a syscall per byte. Shared mutable
    // state lives in std::shared_ptr<FileHandle>; each method is a
    // native callable that captures the shared_ptr, so mutations
    // (readBufPos advancing, closed flipping) are visible across
    // calls on the same handle. The destructor closes any still-open
    // fd as a GC safety net — `defer h.close()` is the polite form
    // but a forgotten close won't leak.
    struct FileHandle {
        int fd = -1;
        bool closed = false;
        std::string path;
        std::string mode;
        // 4 KiB read-ahead buffer. Empty until first read/readLine.
        std::vector<char> readBuf;
        size_t readBufPos = 0;
        size_t readBufEnd = 0;
        bool atEOF = false;

        ~FileHandle() {
            if (!closed && fd >= 0) ::close(fd);
        }
    };

    // Pull one chunk into readBuf. Returns true if at least one byte
    // landed. Sets atEOF when read(2) returns 0.
    auto refillReadBuf = [](FileHandle& h) -> bool {
        if (h.atEOF) return false;
        if (h.readBuf.empty()) h.readBuf.resize(4096);
        ssize_t n;
        do { n = ::read(h.fd, h.readBuf.data(), h.readBuf.size()); }
        while (n < 0 && errno == EINTR);
        if (n < 0)
            throw RuntimeError("FileHandle.read: " + std::string(std::strerror(errno)), 0);
        if (n == 0) { h.atEOF = true; return false; }
        h.readBufPos = 0;
        h.readBufEnd = static_cast<size_t>(n);
        return true;
    };

    // Before a write on a handle that has read-buffered bytes, seek
    // the kernel position back to our logical position so the write
    // lands where the user expects. (POSIX requires a seek between
    // read→write transitions on stdio streams; with raw fds we have
    // to mimic that ourselves on r+/w+ handles.)
    auto syncBeforeWrite = [](FileHandle& h) {
        if (h.readBufPos < h.readBufEnd) {
            int64_t buffered = static_cast<int64_t>(h.readBufEnd - h.readBufPos);
            if (::lseek(h.fd, -buffered, SEEK_CUR) < 0)
                throw RuntimeError("FileHandle.write: seek correction failed: " +
                                   std::string(std::strerror(errno)), 0);
        }
        h.readBufPos = h.readBufEnd = 0;
        h.atEOF = false;
    };

    FsImpl fsOpen = [refillReadBuf, syncBeforeWrite](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.open() requires a string path", 0);
        if (!args[1].isString())
            throw RuntimeError("fs.open() requires a mode string ('r','w','a','r+','w+','a+')", 0);
        auto& p = args[0].asString();
        auto& m = args[1].asString();

        int flags;
        if      (m == "r")  flags = O_RDONLY;
        else if (m == "w")  flags = O_WRONLY | O_CREAT | O_TRUNC;
        else if (m == "a")  flags = O_WRONLY | O_CREAT | O_APPEND;
        else if (m == "r+") flags = O_RDWR;
        else if (m == "w+") flags = O_RDWR   | O_CREAT | O_TRUNC;
        else if (m == "a+") flags = O_RDWR   | O_CREAT | O_APPEND;
        else
            throw RuntimeError("fs.open(): invalid mode \"" + m +
                               "\" (use 'r','w','a','r+','w+','a+')", 0);

        int fd = ::open(p.c_str(), flags, 0644);
        if (fd < 0)
            throw RuntimeError("fs.open(): " + p + ": " + std::strerror(errno), 0);

        auto h = std::make_shared<FileHandle>();
        h->fd = fd;
        h->path = p;
        h->mode = m;

        auto handleMap = gcNew<PraiaMap>();

        handleMap->entries[Value("read")] = Value(makeNative("FileHandle.read", 1,
            [h](const std::vector<Value>& args) -> Value {
                if (h->closed) throw RuntimeError("FileHandle is closed", 0);
                if (!args[0].isNumber())
                    throw RuntimeError("FileHandle.read(n) requires a numeric byte count", 0);
                int64_t wantSigned = args[0].toInt64ForBitwise();
                if (wantSigned < 0)
                    throw RuntimeError("FileHandle.read(n): n must be non-negative", 0);
                size_t want = static_cast<size_t>(wantSigned);
                if (want == 0) return Value(std::string(""));
                std::string out;
                out.reserve(want);
                // Drain any leftover from readBuf first so .read() and
                // .readLine() can be mixed on the same handle without
                // dropping bytes (or duplicating them).
                if (h->readBufPos < h->readBufEnd) {
                    size_t avail = h->readBufEnd - h->readBufPos;
                    size_t take = std::min(want, avail);
                    out.append(h->readBuf.data() + h->readBufPos, take);
                    h->readBufPos += take;
                    want -= take;
                }
                while (want > 0 && !h->atEOF) {
                    char tmp[4096];
                    size_t chunk = std::min(want, sizeof(tmp));
                    ssize_t n;
                    do { n = ::read(h->fd, tmp, chunk); }
                    while (n < 0 && errno == EINTR);
                    if (n < 0)
                        throw RuntimeError("FileHandle.read: " +
                                           std::string(std::strerror(errno)), 0);
                    if (n == 0) { h->atEOF = true; break; }
                    out.append(tmp, n);
                    want -= static_cast<size_t>(n);
                }
                return Value(std::move(out));
            }));

        handleMap->entries[Value("readLine")] = Value(makeNative("FileHandle.readLine", 0,
            [h, refillReadBuf](const std::vector<Value>&) -> Value {
                if (h->closed) throw RuntimeError("FileHandle is closed", 0);
                std::string line;
                while (true) {
                    // Scan current buffer for '\n'.
                    while (h->readBufPos < h->readBufEnd) {
                        char c = h->readBuf[h->readBufPos++];
                        if (c == '\n') return Value(std::move(line));
                        line.push_back(c);
                    }
                    // Buffer exhausted; pull the next chunk.
                    if (!refillReadBuf(*h)) {
                        // EOF. Return trailing line if any, else nil to
                        // signal end-of-stream to the caller's loop.
                        if (line.empty()) return Value();
                        return Value(std::move(line));
                    }
                }
            }));

        handleMap->entries[Value("write")] = Value(makeNative("FileHandle.write", 1,
            [h, syncBeforeWrite](const std::vector<Value>& args) -> Value {
                if (h->closed) throw RuntimeError("FileHandle is closed", 0);
                std::string data = args[0].toString();
                syncBeforeWrite(*h);
                const char* p = data.data();
                size_t remaining = data.size();
                while (remaining > 0) {
                    ssize_t n;
                    do { n = ::write(h->fd, p, remaining); }
                    while (n < 0 && errno == EINTR);
                    if (n < 0)
                        throw RuntimeError("FileHandle.write: " +
                                           std::string(std::strerror(errno)), 0);
                    p += n;
                    remaining -= static_cast<size_t>(n);
                }
                return Value(static_cast<int64_t>(data.size()));
            }));

        handleMap->entries[Value("seek")] = Value(makeNative("FileHandle.seek", -1,
            [h](const std::vector<Value>& args) -> Value {
                if (h->closed) throw RuntimeError("FileHandle is closed", 0);
                if (args.empty() || !args[0].isNumber())
                    throw RuntimeError("FileHandle.seek(offset, whence='start') requires a numeric offset", 0);
                int64_t offset = args[0].toInt64ForBitwise();
                int whence = SEEK_SET;
                if (args.size() >= 2 && !args[1].isNil()) {
                    if (args[1].isString()) {
                        auto& w = args[1].asString();
                        if      (w == "start")   whence = SEEK_SET;
                        else if (w == "current") whence = SEEK_CUR;
                        else if (w == "end")     whence = SEEK_END;
                        else throw RuntimeError(
                            "FileHandle.seek: whence must be 'start'/'current'/'end' (got '" + w + "')", 0);
                    } else if (args[1].isNumber()) {
                        int w = static_cast<int>(args[1].toInt64ForBitwise());
                        if (w != SEEK_SET && w != SEEK_CUR && w != SEEK_END)
                            throw RuntimeError(
                                "FileHandle.seek: numeric whence must be 0 (start), 1 (current), or 2 (end)", 0);
                        whence = w;
                    } else {
                        throw RuntimeError(
                            "FileHandle.seek: whence must be a string or number", 0);
                    }
                }
                // Drop the read buffer — its contents no longer reflect
                // the new file offset.
                h->readBufPos = h->readBufEnd = 0;
                h->atEOF = false;
                off_t r = ::lseek(h->fd, offset, whence);
                if (r < 0)
                    throw RuntimeError("FileHandle.seek: " +
                                       std::string(std::strerror(errno)), 0);
                return Value(static_cast<int64_t>(r));
            }));

        handleMap->entries[Value("tell")] = Value(makeNative("FileHandle.tell", 0,
            [h](const std::vector<Value>&) -> Value {
                if (h->closed) throw RuntimeError("FileHandle is closed", 0);
                off_t r = ::lseek(h->fd, 0, SEEK_CUR);
                if (r < 0)
                    throw RuntimeError("FileHandle.tell: " +
                                       std::string(std::strerror(errno)), 0);
                // Kernel position is past readBufEnd; the user's
                // logical position is readBufPos. Subtract the
                // unconsumed buffered bytes.
                int64_t buffered = static_cast<int64_t>(h->readBufEnd - h->readBufPos);
                return Value(static_cast<int64_t>(r) - buffered);
            }));

        // flush() is fsync. We never buffer writes (each write() syscall
        // hits the kernel immediately), so the name is really about
        // pushing kernel page cache to durable storage. Cheap on success.
        handleMap->entries[Value("flush")] = Value(makeNative("FileHandle.flush", 0,
            [h](const std::vector<Value>&) -> Value {
                if (h->closed) throw RuntimeError("FileHandle is closed", 0);
                if (::fsync(h->fd) != 0)
                    throw RuntimeError("FileHandle.flush: " +
                                       std::string(std::strerror(errno)), 0);
                return Value();
            }));

        // close() is idempotent — calling twice (or via `defer` after a
        // manual close) is a no-op. The handle is unusable after the
        // first close; subsequent reads/writes throw.
        handleMap->entries[Value("close")] = Value(makeNative("FileHandle.close", 0,
            [h](const std::vector<Value>&) -> Value {
                if (h->closed) return Value();
                h->closed = true;
                ::close(h->fd);
                h->fd = -1;
                return Value();
            }));

        // Read-only metadata. `closed` would need to be a callable to
        // stay in sync; users can rely on read/write throwing instead.
        handleMap->entries[Value("path")] = Value(p);
        handleMap->entries[Value("mode")] = Value(m);

        return Value(handleMap);
    };

    // fs.mktemp(prefix?) — race-free temp FILE creation. mkstemp(3)
    // creates the file with mode 0600 in one syscall; there's no
    // window between "pick a name" and "create it" where a different
    // process could race us. Returns the path; the file already
    // exists empty. (Phase 3 will add a handle-returning variant so
    // callers don't have to open() it again.)
    FsImpl fsMktemp = [](const std::vector<Value>& args) -> Value {
        std::string prefix = "praia";
        if (!args.empty()) {
            if (!args[0].isString())
                throw RuntimeError("fs.mktemp() prefix must be a string", 0);
            prefix = args[0].asString();
        }
        std::error_code ec;
        auto tmpPath = fs::temp_directory_path(ec);
        if (ec)
            throw RuntimeError("fs.mktemp(): " + ec.message(), 0);
        std::string tmpl = tmpPath.string() + "/" + prefix + ".XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = ::mkstemp(buf.data());
        if (fd < 0)
            throw RuntimeError("fs.mktemp(): " + std::string(std::strerror(errno)), 0);
        ::close(fd);  // Phase 3: return a handle instead.
        return Value(std::string(buf.data()));
    };

    // Register canonical fs.* entries.
    fsMap->entries[Value("read")]        = Value(makeNative("fs.read",        1,  fsRead,        {"path"}));
    fsMap->entries[Value("write")]       = Value(makeNative("fs.write",       2,  fsWrite,       {"path", "data"}));
    fsMap->entries[Value("append")]      = Value(makeNative("fs.append",      2,  fsAppend,      {"path", "data"}));
    fsMap->entries[Value("exists")]      = Value(makeNative("fs.exists",      1,  fsExists,      {"path"}));
    fsMap->entries[Value("mkdir")]       = Value(makeNative("fs.mkdir",       1,  fsMkdir,       {"path"}));
    fsMap->entries[Value("tempDir")]     = Value(makeNative("fs.tempDir",     -1, fsTempDir,     {"prefix"}));
    fsMap->entries[Value("remove")]      = Value(makeNative("fs.remove",      1,  fsRemove,      {"path"}));
    fsMap->entries[Value("readDir")]     = Value(makeNative("fs.readDir",     1,  fsReadDir,     {"path"}));
    fsMap->entries[Value("copy")]        = Value(makeNative("fs.copy",        2,  fsCopy,        {"src", "dst"}));
    fsMap->entries[Value("move")]        = Value(makeNative("fs.move",        2,  fsMove,        {"src", "dst"}));
    fsMap->entries[Value("stat")]        = Value(makeNative("fs.stat",        1,  fsStat,        {"path"}));
    fsMap->entries[Value("lstat")]       = Value(makeNative("fs.lstat",       1,  fsLstat,       {"path"}));
    fsMap->entries[Value("chmod")]       = Value(makeNative("fs.chmod",       2,  fsChmod,       {"path", "mode"}));
    fsMap->entries[Value("symlink")]     = Value(makeNative("fs.symlink",     2,  fsSymlink,     {"target", "linkPath"}));
    fsMap->entries[Value("readlink")]    = Value(makeNative("fs.readlink",    1,  fsReadlink,    {"path"}));
    fsMap->entries[Value("atomicWrite")] = Value(makeNative("fs.atomicWrite", 2,  fsAtomicWrite, {"path", "data"}));
    fsMap->entries[Value("mktemp")]      = Value(makeNative("fs.mktemp",      -1, fsMktemp,      {"prefix"}));
    fsMap->entries[Value("open")]        = Value(makeNative("fs.open",        2,  fsOpen,        {"path", "mode"}));

    // Helper: register a deprecated sys.<name> forwarder that calls
    // through to the canonical fs.<name> implementation, emitting a
    // one-shot stderr warning on the first call per process. Each
    // forwarder gets its own warned-flag (shared_ptr-wrapped so it
    // can sit in a captured lambda without being moved).
    auto registerDeprecatedSys = [&](const std::string& fnName, int arity, FsImpl impl) {
        auto warned = std::make_shared<std::atomic<bool>>(false);
        sysMap->entries[Value(fnName)] = Value(makeNative("sys." + fnName, arity,
            [warned, fnName, impl](const std::vector<Value>& args) -> Value {
                if (!warned->exchange(true))
                    std::cerr << "[deprecated] sys." << fnName << "() is now fs."
                              << fnName << "(); update callers\n";
                return impl(args);
            }));
    };

    registerDeprecatedSys("read",    1,  fsRead);
    registerDeprecatedSys("write",   2,  fsWrite);
    registerDeprecatedSys("append",  2,  fsAppend);
    registerDeprecatedSys("exists",  1,  fsExists);
    registerDeprecatedSys("mkdir",   1,  fsMkdir);
    registerDeprecatedSys("tempDir", -1, fsTempDir);
    registerDeprecatedSys("remove",  1,  fsRemove);
    registerDeprecatedSys("readDir", 1,  fsReadDir);
    registerDeprecatedSys("copy",    2,  fsCopy);
    registerDeprecatedSys("move",    2,  fsMove);

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

    // sys.notifyDeprecation(name, hint) — surface a deprecation
    // warning from grain/user code, sharing the same dedup-per-name
    // and --strict-deprecations enforcement the C++ method-rename
    // path uses. Resolves the calling engine at runtime via the
    // thread-local current-pointers (VM takes precedence — if it's
    // set we're running under bytecode); falls back to g_currentInterp
    // for tree-walker calls.
    sysMap->entries[Value("notifyDeprecation")] = Value(makeNative("sys.notifyDeprecation", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("sys.notifyDeprecation() requires (name, hint) strings", 0);
            const std::string& name = args[0].asString();
            const std::string& hint = args[1].asString();
            VM* vm = VM::current();
            if (vm) {
                praia::emitNamedDeprecation(vm->strictDeprecations(),
                                            vm->warnedDeprecationsSet(),
                                            name, hint, /*line=*/0);
            } else if (g_currentInterp) {
                praia::emitNamedDeprecation(g_currentInterp->strictDeprecations(),
                                            g_currentInterp->warnedDeprecationsSet(),
                                            name, hint, /*line=*/0);
            }
            return Value();
        }, {"name", "hint"}));

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

                // RAII cleanup for handles the user drops without
                // calling .wait(). Without this dtor the three pipe
                // fds stay open until the process exits (fd leak in
                // long-running servers that spawn helpers in a loop)
                // and the child stays as a zombie until reaped. Mirrors
                // the HttpStreamState::~HttpStreamState fix.
                //
                // Refcount-zero on the shared_ptr means no other
                // thread holds this state, so no locking is needed.
                ~SpawnState() {
                    if (stdinOpen) ::close(stdinFd);
                    ::close(stdoutFd);
                    ::close(stderrFd);
                    if (!waited) {
                        int status = 0;
                        // Try a non-blocking reap first — the child may
                        // already have exited cleanly. If still running,
                        // SIGKILL the whole process group (matches
                        // proc.kill's pgroup semantics) and block on
                        // the final reap. Bounded wait: we're killing,
                        // not asking for a clean shutdown.
                        pid_t r = ::waitpid(pid, &status, WNOHANG);
                        if (r == 0) {
                            ::kill(-pid, SIGKILL);
                            ::waitpid(pid, &status, 0);
                        }
                    }
                }
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
    // fsMap is the same shared_ptr after additional entries (readLines)
    // get tacked on below, so registering it here is fine.
    globals->define("fs", Value(fsMap));

    // ── http namespace ──

    auto httpMap = gcNew<PraiaMap>();

    // Shared options-map parser: layers per-call options on top of a
    // base HttpOptions in place (only overrides fields the user
    // actually set). Used by http.request, http.session creation, and
    // the session-bound request paths so all three share one schema.
    // Generic lambda — `auto& m` accepts the full PraiaMap::entries
    // type including its ValueHash / ValueKeyEqual template params
    // without having to spell them.
    auto applyHttpOpts = [](HttpOptions& opts, const auto& m) {
        auto secsToMs = [](const Value& v) -> int {
            if (!v.isNumber()) return -1;
            double secs = v.asNumber();
            if (secs <= 0) return -1;
            return static_cast<int>(secs * 1000.0);
        };
        if (m.count("timeout")) {
            // Shorthand: applies to all three axes simultaneously.
            // Matches Python requests' `timeout=10` form.
            int ms = secsToMs(m.at("timeout"));
            if (ms > 0) {
                opts.connectTimeoutMs = ms;
                opts.readTimeoutMs    = ms;
                opts.totalTimeoutMs   = ms;
            }
        }
        if (m.count("connectTimeout"))
            opts.connectTimeoutMs = secsToMs(m.at("connectTimeout"));
        if (m.count("readTimeout"))
            opts.readTimeoutMs    = secsToMs(m.at("readTimeout"));
        if (m.count("totalTimeout"))
            opts.totalTimeoutMs   = secsToMs(m.at("totalTimeout"));
        if (m.count("followRedirects") && m.at("followRedirects").isBool())
            opts.followRedirects = m.at("followRedirects").asBool();
        if (m.count("maxRedirects") && m.at("maxRedirects").isNumber())
            opts.maxRedirects = static_cast<int>(m.at("maxRedirects").asNumber());
        if (m.count("insecure") && m.at("insecure").isBool())
            opts.insecure = m.at("insecure").asBool();
        if (m.count("caBundle") && m.at("caBundle").isString())
            opts.caBundle = m.at("caBundle").asString();
        if (m.count("blockPrivateHosts") && m.at("blockPrivateHosts").isBool())
            opts.blockPrivateHosts = m.at("blockPrivateHosts").asBool();
    };

    // Layer per-call headers on top of session defaults (or {} for
    // stateless). Per-call values win on key conflict. Generic
    // lambda for the same reason applyHttpOpts is.
    auto layerHeaders = [](std::unordered_map<std::string, std::string> base,
                           const auto& m) {
        if (m.count("headers") && m.at("headers").isMap()) {
            for (auto& [k, v] : m.at("headers").asMap()->entries)
                base[k.toString()] = v.toString();
        }
        return base;
    };

    httpMap->entries[Value("get")] = Value(makeNative("http.get", -1,
        [applyHttpOpts, layerHeaders](const std::vector<Value>& args) -> Value {
            // http.get(url)                  → stateless
            // http.get(url, opts)            → stateless with per-call opts
            // http.get(session, url)         → session-bound
            // http.get(session, url, opts)   → session-bound with per-call opts
            auto applyOptsArg = [&applyHttpOpts, &layerHeaders](
                                    const Value& v, HttpOptions& opts,
                                    std::unordered_map<std::string, std::string>& headers) {
                if (!v.isMap()) return;
                auto& m = v.asMap()->entries;
                applyHttpOpts(opts, m);
                headers = layerHeaders(headers, m);
            };

            if (args.size() == 1 && args[0].isString()) {
                return doHttpRequest("GET", args[0].asString(), "", {});
            }
            if (args.size() == 2 && args[0].isString() && args[1].isMap()) {
                HttpOptions opts;
                std::unordered_map<std::string, std::string> headers;
                applyOptsArg(args[1], opts, headers);
                return doHttpRequest("GET", args[0].asString(), "", headers, opts);
            }
            if (args.size() == 2 && httpIsSession(args[0]) && args[1].isString()) {
                auto opts = httpSessionGetDefaultOpts(args[0]);
                auto hdrs = httpSessionGetDefaultHeaders(args[0]);
                return httpSessionRequest(args[0], "GET", args[1].asString(),
                                          "", hdrs, opts);
            }
            if (args.size() == 3 && httpIsSession(args[0]) && args[1].isString() && args[2].isMap()) {
                auto opts = httpSessionGetDefaultOpts(args[0]);
                auto hdrs = httpSessionGetDefaultHeaders(args[0]);
                applyOptsArg(args[2], opts, hdrs);
                return httpSessionRequest(args[0], "GET", args[1].asString(),
                                          "", hdrs, opts);
            }
            throw RuntimeError(
                "http.get() requires (url), (url, opts), (session, url), or (session, url, opts)", 0);
        }));

    httpMap->entries[Value("post")] = Value(makeNative("http.post", -1,
        [applyHttpOpts, layerHeaders](const std::vector<Value>& args) -> Value {
            // http.post(url, body|opts)             → stateless
            // http.post(session, url, body|opts)    → session-bound
            auto parseBodyAndHeaders = [](const Value& v, std::string& body,
                                          std::unordered_map<std::string, std::string>& headers) {
                if (v.isString()) {
                    body = v.asString();
                } else if (v.isMap()) {
                    auto& e = v.asMap()->entries;
                    if (e.count("body")) body = e.at("body").toString();
                    if (e.count("headers") && e.at("headers").isMap()) {
                        for (auto& [k, vv] : e.at("headers").asMap()->entries)
                            headers[k.toString()] = vv.toString();
                    }
                }
            };
            if (args.size() == 2 && args[0].isString()) {
                std::string body;
                std::unordered_map<std::string, std::string> headers;
                parseBodyAndHeaders(args[1], body, headers);
                return doHttpRequest("POST", args[0].asString(), body, headers);
            }
            if (args.size() == 3 && httpIsSession(args[0]) && args[1].isString()) {
                std::string body;
                std::unordered_map<std::string, std::string> headers;
                parseBodyAndHeaders(args[2], body, headers);
                // Layer session defaults under per-call values.
                auto opts = httpSessionGetDefaultOpts(args[0]);
                auto hdrs = httpSessionGetDefaultHeaders(args[0]);
                for (auto& [k, v] : headers) hdrs[k] = v;
                // body is fully user-specified; opts inherited from session
                // (per-call timeout overrides aren't passed via the body-or-opts
                // shorthand — use http.request for that).
                (void)applyHttpOpts; (void)layerHeaders;   // not used here
                return httpSessionRequest(args[0], "POST", args[1].asString(),
                                          body, hdrs, opts);
            }
            throw RuntimeError(
                "http.post() requires either (url, body|opts) or (session, url, body|opts)", 0);
        }));

    httpMap->entries[Value("request")] = Value(makeNative("http.request", -1,
        [applyHttpOpts, layerHeaders](const std::vector<Value>& args) -> Value {
            // http.request(opts)              → stateless
            // http.request(session, opts)     → session-bound
            const Value* sessionArg = nullptr;
            const Value* optsArg    = nullptr;
            if (args.size() == 1 && args[0].isMap()) {
                optsArg = &args[0];
            } else if (args.size() == 2 && httpIsSession(args[0]) && args[1].isMap()) {
                sessionArg = &args[0];
                optsArg    = &args[1];
            } else {
                throw RuntimeError(
                    "http.request() requires (opts) or (session, opts)", 0);
            }

            auto& opts = optsArg->asMap()->entries;
            std::string method = "GET", url, body;
            if (opts.count("method")) method = opts.at("method").toString();
            if (opts.count("url")) url = opts.at("url").toString();
            else throw RuntimeError("http.request() requires a 'url' field", 0);
            if (opts.count("body")) body = opts.at("body").toString();

            // Base values: from session if present, else fresh defaults.
            HttpOptions httpOpts = sessionArg
                ? httpSessionGetDefaultOpts(*sessionArg) : HttpOptions{};
            std::unordered_map<std::string, std::string> baseHeaders;
            if (sessionArg) baseHeaders = httpSessionGetDefaultHeaders(*sessionArg);

            applyHttpOpts(httpOpts, opts);
            auto headers = layerHeaders(std::move(baseHeaders), opts);

            return sessionArg
                ? httpSessionRequest(*sessionArg, method, url, body, headers, httpOpts)
                : doHttpRequest(method, url, body, headers, httpOpts);
        }));

    // http.session(opts?) — create a keepalive session. Accepts the
    // same options shape as http.request, plus a top-level `headers`
    // map for session-default request headers. Returns an opaque
    // PraiaExternal (prints as <external:http.session>); use it as
    // the first arg to http.get/post/request, and close with
    // http.close (or let the GC do it).
    httpMap->entries[Value("session")] = Value(makeNative("http.session", -1,
        [applyHttpOpts, layerHeaders](const std::vector<Value>& args) -> Value {
            if (args.size() > 1)
                throw RuntimeError("http.session() takes 0 or 1 argument", 0);
            HttpOptions defaultOpts;
            std::unordered_map<std::string, std::string> defaultHeaders;
            // -1 here means "let httpCreateSession use the struct default"
            // (100 conns, 90s TTL). Explicit user values override.
            int poolMaxSize = -1;
            int poolIdleMs  = -1;
            if (args.size() == 1) {
                if (!args[0].isMap())
                    throw RuntimeError("http.session() options must be a map", 0);
                auto& m = args[0].asMap()->entries;
                applyHttpOpts(defaultOpts, m);
                defaultHeaders = layerHeaders({}, m);
                if (m.count("poolSize") && m.at("poolSize").isNumber()) {
                    double v = m.at("poolSize").asNumber();
                    // Reject NaN / Inf (cast to int is UB), out-of-int-range
                    // values, and non-integers — "1.5 connections" is
                    // meaningless, fail loudly instead of silently truncating.
                    if (!std::isfinite(v) ||
                        std::fabs(v) > static_cast<double>(INT_MAX) ||
                        v != std::floor(v)) {
                        throw RuntimeError(
                            "http.session(): poolSize must be a finite integer", 0);
                    }
                    poolMaxSize = static_cast<int>(v);
                }
                if (m.count("poolIdleSecs") && m.at("poolIdleSecs").isNumber()) {
                    double secs = m.at("poolIdleSecs").asNumber();
                    // NaN / Inf reject always (cast to int is UB). The
                    // INT_MAX/1000 overflow bound only applies to positive
                    // values — anything <= 0 collapses to 0 below ("disable
                    // TTL"), so a large negative like -1e18 is a valid
                    // way to say "no timeout." Don't reject it.
                    if (!std::isfinite(secs) ||
                        (secs > 0 && secs * 1000.0 > static_cast<double>(INT_MAX))) {
                        throw RuntimeError(
                            "http.session(): poolIdleSecs must be a finite "
                            "value below INT_MAX/1000 seconds", 0);
                    }
                    // 0 / negative is a deliberate "disable the TTL" toggle —
                    // pass it through as 0. Positive values convert to ms;
                    // fractional seconds are allowed (e.g. 0.1 = 100 ms).
                    // Round UP rather than truncate so sub-millisecond
                    // positives (0.0005 → 0.5 ms) don't collapse to 0 and
                    // accidentally disable the TTL — every positive value
                    // gets at least a 1 ms timeout.
                    poolIdleMs = secs <= 0
                        ? 0
                        : static_cast<int>(std::ceil(secs * 1000.0));
                }
            }
            // The struct's own defaults kick in when we pass -1 (or
            // anything < 1 for poolMaxSize). poolIdleMs == 0 means
            // "disable TTL"; the factory passes it through verbatim.
            return httpCreateSession(defaultOpts, defaultHeaders,
                                     poolMaxSize, poolIdleMs);
        }));

    httpMap->entries[Value("close")] = Value(makeNative("http.close", 1,
        [](const std::vector<Value>& args) -> Value {
            httpSessionClose(args[0]);
            return Value();
        }));

    // http.openStream — same option surface as http.request, but
    // returns a stream handle instead of slurping the body. Use it
    // for downloads / NDJSON feeds / anything that doesn't fit
    // comfortably in memory. The handle is compatible with
    // json.parser (both expose .read(n)).
    //
    //   http.openStream(opts)              → stateless
    //   http.openStream(session, opts)     → inherits session defaults
    //
    // The session path layers session-default headers + opts under
    // per-call values; the stream itself still opens a fresh socket
    // (pool-of-streams is documented as a deliberate non-feature).
    httpMap->entries[Value("openStream")] = Value(makeNative("http.openStream", -1,
        [applyHttpOpts, layerHeaders](const std::vector<Value>& args) -> Value {
            const Value* sessionArg = nullptr;
            const Value* optsArg    = nullptr;
            if (args.size() == 1 && args[0].isMap()) {
                optsArg = &args[0];
            } else if (args.size() == 2 && httpIsSession(args[0]) && args[1].isMap()) {
                sessionArg = &args[0];
                optsArg    = &args[1];
            } else {
                throw RuntimeError(
                    "http.openStream() requires (opts) or (session, opts)", 0);
            }

            auto& opts = optsArg->asMap()->entries;
            std::string method = "GET", url, body;
            if (opts.count("method")) method = opts.at("method").toString();
            if (opts.count("url")) url = opts.at("url").toString();
            else throw RuntimeError("http.openStream() requires a 'url' field", 0);
            if (opts.count("body")) body = opts.at("body").toString();

            HttpOptions httpOpts = sessionArg
                ? httpSessionGetDefaultOpts(*sessionArg) : HttpOptions{};
            std::unordered_map<std::string, std::string> baseHeaders;
            if (sessionArg) baseHeaders = httpSessionGetDefaultHeaders(*sessionArg);

            applyHttpOpts(httpOpts, opts);
            auto headers = layerHeaders(std::move(baseHeaders), opts);

            return httpOpenStream(method, url, body, headers, httpOpts);
        }));

    // listen() runs the request handler against the Interpreter that invoked
    // listen() (resolved via g_currentInterp), not the one that constructed
    // the server. That way listen() inside an async task drives the handler
    // through the task's Interpreter instead of racing on the parent's.
    httpMap->entries[Value("createServer")] = Value(makeNative("http.createServer", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isCallable())
                throw RuntimeError("http.createServer() requires a handler function", 0);
            auto handler = args[0].asCallable();

            auto server = gcNew<PraiaMap>();
            server->entries[Value("listen")] = Value(makeNative("listen", 1,
                [handler](const std::vector<Value>& args) -> Value {
                    if (!args[0].isNumber())
                        throw RuntimeError("listen() requires a port number", 0);
                    httpServerListen(static_cast<int>(args[0].asNumber()), handler, *g_currentInterp);
                    return Value();
                }));
            return Value(server);
        }));

    // http.sse(req, callback) — Server-Sent Events
    // callback receives a send function: send(data, event?)
    // The connection stays open until the callback returns.
    httpMap->entries[Value("sse")] = Value(makeNative("http.sse", 2,
        [](const std::vector<Value>& args) -> Value {
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

            // Call the callback with the send function via the current
            // (calling) Interpreter — see http.createServer comment.
            try {
                std::vector<Value> cbArgs = {Value(std::static_pointer_cast<Callable>(sendFn))};
                callSafe(*g_currentInterp, callback, cbArgs);
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
        }, {"input", "maxLength"}));

    jsonMap->entries[Value("stringify")] = Value(makeNative("json.stringify", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty())
                throw RuntimeError("json.stringify() requires a value", 0);
            int indent = 0;
            if (args.size() > 1 && args[1].isNumber())
                indent = static_cast<int>(args[1].asNumber());
            return Value(jsonStringify(args[0], indent, 0));
        }, {"value", "indent"}));

    // json.parser(input) — pull-parser for streaming reads. `input`
    // is either a string (parsed in-memory) or a file handle with a
    // .read(n) method (read incrementally). Returns a handle with
    // .next() / .nextValue() / .eof() / .close().
    jsonMap->entries[Value("parser")] = Value(makeNative("json.parser", 1,
        [](const std::vector<Value>& args) -> Value {
            return jsonParserCreate(args[0]);
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
        }, {"dir", "pattern"}));

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

    // url.parse delegates to the shared RFC 3986-shaped parser in
    // src/url.cpp so http.* and this builtin can't drift from each
    // other on IPv6 brackets, userinfo, fragments, or port handling.
    // Returns a map with the seven canonical components:
    //   scheme, userinfo, host, port (nil when absent), path, query,
    //   fragment.
    // `port` is intentionally nil rather than 0 when omitted — a real
    // 0 is a valid (if pathological) port value and we don't want
    // "absent" and "explicit 0" to collide.
    urlMap->entries[Value("parse")] = Value(makeNative("url.parse", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString()) throw RuntimeError("url.parse() requires a string", 0);
            auto& input = args[0].asString();
            praia::url::ParsedUrl u;
            try {
                u = praia::url::parse(input);
            } catch (const praia::url::UrlParseError& e) {
                throw RuntimeError(std::string("url.parse: ") + e.what(), 0);
            }
            auto result = gcNew<PraiaMap>();
            result->entries[Value("scheme")]   = Value(u.scheme);
            result->entries[Value("userinfo")] = Value(u.userinfo);
            result->entries[Value("host")]     = Value(u.host);
            result->entries[Value("port")] = u.hasPort
                ? Value(static_cast<int64_t>(u.port))
                : Value();  // nil
            result->entries[Value("path")]     = Value(u.path);
            result->entries[Value("query")]    = Value(u.query);
            result->entries[Value("fragment")] = Value(u.fragment);
            return Value(result);
        }));

    // Internal helpers for the builders. Duplicated from http.encodeURI
    // logic so url.* doesn't reach into the http namespace. Cheap (<10
    // lines) and lets the two implementations diverge cleanly later if
    // we ever need component-specific encoding rules (e.g. query allows
    // some bytes path doesn't).
    auto percentEncode = [](const std::string& s) -> std::string {
        std::string out;
        out.reserve(s.size() * 3);
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += static_cast<char>(c);
            } else {
                char hex[4];
                std::snprintf(hex, sizeof(hex), "%%%02X", c);
                out += hex;
            }
        }
        return out;
    };
    auto percentDecode = [](const std::string& s) -> std::string {
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            // '+' decodes to space — form-urlencoded compatibility.
            // RFC 3986 doesn't require this but every real-world
            // query string mixes the two encodings, and decoding
            // '+' as '+' instead of ' ' loses data on form submits.
            if (s[i] == '+') { out += ' '; continue; }
            if (s[i] == '%' && i + 2 < s.size()) {
                int hi = hexVal(s[i + 1]);
                int lo = hexVal(s[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out += static_cast<char>((hi << 4) | lo);
                    i += 2;
                    continue;
                }
            }
            out += s[i];
        }
        return out;
    };

    // url.encode / url.decode — RFC 3986 percent-encoding for the
    // "unreserved" character class. Same byte-level behavior as
    // http.encodeURI/decodeURI; offered under the url namespace so
    // callers building URLs don't have to mix namespaces.
    urlMap->entries[Value("encode")] = Value(makeNative("url.encode", 1,
        [percentEncode](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("url.encode() requires a string", 0);
            return Value(percentEncode(args[0].asString()));
        }));
    urlMap->entries[Value("decode")] = Value(makeNative("url.decode", 1,
        [percentDecode](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("url.decode() requires a string", 0);
            return Value(percentDecode(args[0].asString()));
        }));

    // url.buildQuery(map) — serialize a map to "k=v&k=v" form-style.
    //
    // Per-value handling:
    //   string         → percent-encode
    //   number/bool    → toString → percent-encode
    //   nil            → SKIP the entry (treat as "no value supplied")
    //   array          → emit one k=v pair per element
    //                    ({tags: ["a","b"]} → "tags=a&tags=b")
    //   nested map     → throw (no canonical wire form for this)
    //
    // Keys are always percent-encoded. Map iteration order is
    // unspecified (Praia's primitive map is unordered), so callers
    // who need stable output should use an OrderedMap-style flow
    // and build an array of pairs instead.
    auto buildQueryPair = [percentEncode](std::string& out,
                                          const std::string& key,
                                          const Value& v) {
        auto append = [&](const std::string& strVal) {
            if (!out.empty()) out += '&';
            out += percentEncode(key);
            out += '=';
            out += percentEncode(strVal);
        };
        if (v.isNil()) return;  // skip nil values
        if (v.isString())        append(v.asString());
        else if (v.isBool())     append(v.asBool() ? "true" : "false");
        else if (v.isInt())      append(std::to_string(v.asInt()));
        else if (v.isDouble())   append(v.toString());
        else if (v.isArray()) {
            for (const auto& e : v.asArray()->elements) {
                if (e.isNil()) continue;
                if (e.isMap() || e.isArray())
                    throw RuntimeError("url.buildQuery: nested array/map values aren't supported", 0);
                std::string s = e.isString() ? e.asString() : e.toString();
                if (!out.empty()) out += '&';
                out += percentEncode(key);
                out += '=';
                out += percentEncode(s);
            }
        }
        else if (v.isMap())
            throw RuntimeError("url.buildQuery: nested map values aren't supported", 0);
        else
            throw RuntimeError("url.buildQuery: unsupported value type for key \"" + key + "\"", 0);
    };

    urlMap->entries[Value("buildQuery")] = Value(makeNative("url.buildQuery", 1,
        [buildQueryPair](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap())
                throw RuntimeError("url.buildQuery() requires a map", 0);
            std::string out;
            for (const auto& [k, v] : args[0].asMap()->entries) {
                if (!k.isString())
                    throw RuntimeError("url.buildQuery: keys must be strings", 0);
                buildQueryPair(out, k.asString(), v);
            }
            return Value(std::move(out));
        }));

    // url.parseQuery(s) — inverse of buildQuery.
    //
    // Returns a map. Keys appearing multiple times collapse to an
    // array of values; single-occurrence keys are plain strings.
    // (Python's parse_qs always returns arrays; we auto-flatten for
    // the common single-value case. Use parseQueryAll for the
    // always-array form when you need predictable shape.)
    auto parseQueryToPairs = [percentDecode](const std::string& s) {
        std::vector<std::pair<std::string, std::string>> pairs;
        if (s.empty()) return pairs;
        size_t i = 0;
        while (i <= s.size()) {
            size_t amp = s.find('&', i);
            size_t end = (amp == std::string::npos) ? s.size() : amp;
            if (end > i) {
                size_t eq = s.find('=', i);
                std::string k, v;
                if (eq == std::string::npos || eq > end) {
                    k = s.substr(i, end - i);
                } else {
                    k = s.substr(i, eq - i);
                    v = s.substr(eq + 1, end - eq - 1);
                }
                pairs.emplace_back(percentDecode(k), percentDecode(v));
            }
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
        return pairs;
    };

    urlMap->entries[Value("parseQuery")] = Value(makeNative("url.parseQuery", 1,
        [parseQueryToPairs](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("url.parseQuery() requires a string", 0);
            auto pairs = parseQueryToPairs(args[0].asString());
            auto m = gcNew<PraiaMap>();
            for (auto& [k, v] : pairs) {
                Value key(k);
                auto it = m->entries.find(key);
                if (it == m->entries.end()) {
                    m->entries[key] = Value(v);
                } else if (it->second.isArray()) {
                    it->second.asArray()->elements.push_back(Value(v));
                } else {
                    // Promote single-string to array on second occurrence.
                    auto arr = gcNew<PraiaArray>();
                    arr->elements.push_back(it->second);
                    arr->elements.push_back(Value(v));
                    it->second = Value(arr);
                }
            }
            return Value(m);
        }));

    // url.parseQueryAll — same input handling but ALWAYS returns
    // values as arrays. Use when downstream code wants a predictable
    // shape regardless of how many times each key appears.
    urlMap->entries[Value("parseQueryAll")] = Value(makeNative("url.parseQueryAll", 1,
        [parseQueryToPairs](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("url.parseQueryAll() requires a string", 0);
            auto pairs = parseQueryToPairs(args[0].asString());
            auto m = gcNew<PraiaMap>();
            for (auto& [k, v] : pairs) {
                Value key(k);
                auto it = m->entries.find(key);
                if (it == m->entries.end()) {
                    auto arr = gcNew<PraiaArray>();
                    arr->elements.push_back(Value(v));
                    m->entries[key] = Value(arr);
                } else {
                    it->second.asArray()->elements.push_back(Value(v));
                }
            }
            return Value(m);
        }));

    // url.build(parts) — compose a URL string from a component map.
    //
    // Accepted fields (all optional):
    //   scheme    "https" — lowercased on emit
    //   userinfo  "user:pass" — emitted verbatim (caller pre-encodes)
    //   host      "example.com" or "::1" — IPv6 auto-bracketed
    //   port      8080 — omitted if it matches the scheme default
    //   path      "/v1/items" — leading '/' added if scheme+host present
    //   query     either a string ("a=1&b=2", emitted verbatim) or a
    //             map (passed through buildQuery)
    //   fragment  "section"
    //
    // No field is required. With nothing, returns "". This makes it
    // easy to build relative URLs (just path + query) or full URLs.
    urlMap->entries[Value("build")] = Value(makeNative("url.build", 1,
        [percentEncode, buildQueryPair](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap())
                throw RuntimeError("url.build() requires a component map", 0);
            auto& m = args[0].asMap()->entries;
            auto get = [&](const std::string& k) -> const Value* {
                auto it = m.find(Value(k));
                return it == m.end() ? nullptr : &it->second;
            };

            std::string out;

            const Value* scheme = get("scheme");
            std::string schemeStr;
            if (scheme && scheme->isString() && !scheme->asString().empty()) {
                schemeStr = scheme->asString();
                for (auto& c : schemeStr)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            const Value* host = get("host");
            std::string hostStr = (host && host->isString()) ? host->asString() : "";

            // scheme + authority block only when both are present, OR
            // for the special opaque-URI case (scheme but no host:
            // emit "scheme:path" — covers mailto, tel, etc.).
            if (!schemeStr.empty() && !hostStr.empty()) {
                out += schemeStr;
                out += "://";
                const Value* userinfo = get("userinfo");
                if (userinfo && userinfo->isString() && !userinfo->asString().empty()) {
                    out += userinfo->asString();
                    out += '@';
                }
                // IPv6 literals (any host containing ':' that isn't a
                // bracketed form) need wrapping. If the user pre-
                // wrapped, don't double-wrap.
                if (!hostStr.empty() && hostStr.find(':') != std::string::npos &&
                    hostStr.front() != '[') {
                    out += '[';
                    out += hostStr;
                    out += ']';
                } else {
                    out += hostStr;
                }
                const Value* port = get("port");
                if (port && !port->isNil()) {
                    // Strict validation: a `port` field that's present
                    // but invalid is almost always a bug (port 0, an
                    // out-of-range integer like 99999, or a non-numeric
                    // value). Silently dropping it would hide that bug.
                    // Caller should omit the key entirely if they don't
                    // want a port in the output.
                    if (!port->isNumber())
                        throw RuntimeError("url.build: 'port' must be a number", 0);
                    double pd = port->asNumber();
                    // Range-check on the double BEFORE casting — values
                    // outside int64_t range (e.g. 1e30, inf) make the
                    // static_cast<int64_t> UB pre-C++23. NaN fails the
                    // integer check below since NaN != NaN.
                    if (!(pd >= 1.0 && pd <= 65535.0))
                        throw RuntimeError("url.build: 'port' must be in 1..65535", 0);
                    if (pd != std::floor(pd))
                        throw RuntimeError("url.build: 'port' must be an integer (got " +
                                           std::to_string(pd) + ")", 0);
                    int64_t p = static_cast<int64_t>(pd);
                    bool isDefault = (schemeStr == "http"  && p == 80) ||
                                      (schemeStr == "https" && p == 443);
                    if (!isDefault) {
                        out += ':';
                        out += std::to_string(p);
                    }
                }
            } else if (!schemeStr.empty()) {
                // Opaque URI form: "scheme:path"
                out += schemeStr;
                out += ':';
            }

            const Value* path = get("path");
            if (path && path->isString()) {
                const auto& p = path->asString();
                if (!schemeStr.empty() && !hostStr.empty() && !p.empty() && p.front() != '/')
                    out += '/';
                out += p;
            }

            const Value* query = get("query");
            if (query && !query->isNil()) {
                std::string qstr;
                if (query->isString()) {
                    qstr = query->asString();
                } else if (query->isMap()) {
                    for (const auto& [k, v] : query->asMap()->entries) {
                        if (!k.isString())
                            throw RuntimeError("url.build: query map keys must be strings", 0);
                        buildQueryPair(qstr, k.asString(), v);
                    }
                } else {
                    throw RuntimeError("url.build: 'query' must be a string or map", 0);
                }
                if (!qstr.empty()) {
                    out += '?';
                    out += qstr;
                }
            }

            const Value* frag = get("fragment");
            if (frag && frag->isString() && !frag->asString().empty()) {
                out += '#';
                out += percentEncode(frag->asString());
            }

            return Value(std::move(out));
        }));

    globals->define("url", Value(urlMap));

    // ── unicode namespace ──
    //
    // Phase 1 of the text/Unicode rework: normalization, display
    // width, and sortable keys. All backed by utf8proc. Real
    // locale-aware collation (Spanish "ll", Swedish ä-after-z,
    // Turkish dotless-i) needs ICU as a dependency and is out of
    // scope here — collateKey gives Unicode-default ordering, which
    // is the right answer for 90% of "sort these names" needs and
    // wrong for the long tail of locale-specific tailoring.

    auto unicodeMap = gcNew<PraiaMap>();

#ifdef HAVE_UTF8PROC
    // Shared helper: call utf8proc_map with a flag combo and return
    // the result as a std::string (taking ownership of the malloc'd
    // buffer utf8proc hands back).
    auto u8procMap = [](const std::string& in, int options) -> std::string {
        utf8proc_uint8_t* out = nullptr;
        utf8proc_ssize_t n = utf8proc_map(
            reinterpret_cast<const utf8proc_uint8_t*>(in.data()),
            static_cast<utf8proc_ssize_t>(in.size()),
            &out,
            static_cast<utf8proc_option_t>(options));
        if (n < 0) {
            if (out) free(out);
            throw RuntimeError(std::string("utf8proc failed: ") +
                               utf8proc_errmsg(n), 0);
        }
        std::string result(reinterpret_cast<char*>(out), static_cast<size_t>(n));
        free(out);
        return result;
    };

    // unicode.normalize(s, form) — NFC / NFD / NFKC / NFKD.
    //   NFC  = canonical composition (default in most pipelines)
    //   NFD  = canonical decomposition (combining marks separated)
    //   NFKC = compatibility composition (½ → 1/2-style folding)
    //   NFKD = compatibility decomposition
    // Pick NFC when you need a canonical form for storage or
    // comparison; NFD when you need to inspect combining marks
    // (or strip them); NFKC when you want forms that look the same
    // to compare equal ("ｦ" vs "ヲ"); NFKD likewise but decomposed.
    unicodeMap->entries[Value("normalize")] = Value(makeNative("unicode.normalize", 2,
        [u8procMap](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("unicode.normalize() requires a string", 0);
            if (!args[1].isString())
                throw RuntimeError("unicode.normalize(s, form): form must be 'NFC', 'NFD', 'NFKC', or 'NFKD'", 0);
            const auto& s = args[0].asString();
            const auto& form = args[1].asString();
            int opts = UTF8PROC_STABLE;
            if      (form == "NFC")  opts |= UTF8PROC_COMPOSE;
            else if (form == "NFD")  opts |= UTF8PROC_DECOMPOSE;
            else if (form == "NFKC") opts |= UTF8PROC_COMPOSE   | UTF8PROC_COMPAT;
            else if (form == "NFKD") opts |= UTF8PROC_DECOMPOSE | UTF8PROC_COMPAT;
            else throw RuntimeError("unicode.normalize: unknown form \"" + form +
                                    "\" (expected NFC / NFD / NFKC / NFKD)", 0);
            return Value(u8procMap(s, opts));
        }));

    // unicode.displayWidth(s) — sum of monospace cell widths across
    // grapheme clusters. ASCII = 1 per char, CJK / emoji = 2, combining
    // marks = 0 (they attach to the previous cluster). Approximation:
    // we take the width of each cluster's FIRST codepoint, so multi-
    // codepoint emoji ZWJ sequences (👨‍👩‍👧‍👦) render as the width of
    // the base emoji (2). Control chars contribute 0 — terminals
    // render tab/newline however they like; this function doesn't
    // expand them.
    unicodeMap->entries[Value("displayWidth")] = Value(makeNative("unicode.displayWidth", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("unicode.displayWidth() requires a string", 0);
            const auto& s = args[0].asString();
            int total = 0;
            for (const auto& g : utf8_graphemes(s)) {
                int32_t cp = utf8_first_codepoint(g);
                int w = utf8proc_charwidth(cp);
                if (w > 0) total += w;
            }
            return Value(static_cast<int64_t>(total));
        }));

    // unicode.collateKey(s) — NFD + casefold + STABLE. Use it as a
    // sort key: `sort(names, lam{ a, b in unicode.collateKey(a) <
    // unicode.collateKey(b) })`. Diacritic-SENSITIVE: "elan" and
    // "Élan" produce different keys, and "elan" sorts before "Élan".
    //
    // The decomposed combining marks have high lead bytes (0xCC..),
    // so accented variants of a letter sort to the END of that
    // letter's section rather than interleaving with un-accented
    // variants — e.g. ["elan", "Eve", "Élan"] in collateKey order,
    // not the strict UCA ordering of ["elan", "Élan", "Eve"]. That's
    // good enough for most user-facing sort needs (case-insensitive,
    // accent-tolerant); applications that need real locale-aware
    // collation (Spanish "ll", Swedish ä-after-z, Turkish dotless-i)
    // should link ICU and use ucol_*.
    unicodeMap->entries[Value("collateKey")] = Value(makeNative("unicode.collateKey", 1,
        [u8procMap](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("unicode.collateKey() requires a string", 0);
            return Value(u8procMap(args[0].asString(),
                                   UTF8PROC_STABLE | UTF8PROC_DECOMPOSE | UTF8PROC_CASEFOLD));
        }));

    // unicode.foldKey(s) — like collateKey but ALSO strips combining
    // marks (UTF8PROC_STRIPMARK). Diacritic-INSENSITIVE: "Élan" and
    // "elan" produce the same key, so they compare equal under
    // foldKey-based search. Useful for "search ignoring accents".
    unicodeMap->entries[Value("foldKey")] = Value(makeNative("unicode.foldKey", 1,
        [u8procMap](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("unicode.foldKey() requires a string", 0);
            return Value(u8procMap(args[0].asString(),
                                   UTF8PROC_STABLE | UTF8PROC_DECOMPOSE |
                                   UTF8PROC_CASEFOLD | UTF8PROC_STRIPMARK));
        }));
#else
    // utf8proc absent: every function throws so callers get a clear
    // signal instead of silently-wrong output. Building without
    // utf8proc is a degraded mode mainly for fuzz harnesses.
    auto needUtf8proc = [](const std::string& fnName) {
        return Value(makeNative(fnName, -1,
            [fnName](const std::vector<Value>&) -> Value {
                throw RuntimeError(fnName + " requires utf8proc (rebuild with HAVE_UTF8PROC)", 0);
            }));
    };
    unicodeMap->entries[Value("normalize")]    = needUtf8proc("unicode.normalize");
    unicodeMap->entries[Value("displayWidth")] = needUtf8proc("unicode.displayWidth");
    unicodeMap->entries[Value("collateKey")]   = needUtf8proc("unicode.collateKey");
    unicodeMap->entries[Value("foldKey")]      = needUtf8proc("unicode.foldKey");
#endif

    globals->define("unicode", Value(unicodeMap));

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

    auto secretsMap = gcNew<PraiaMap>();
    registerSecretsBuiltins(secretsMap);
    globals->define("secrets", Value(secretsMap));

    auto fmtMap = gcNew<PraiaMap>();
    registerFmtBuiltins(fmtMap);
    globals->define("fmt", Value(fmtMap));

    auto zlibMap = gcNew<PraiaMap>();
    registerZlibBuiltins(zlibMap);
    globals->define("zlib", Value(zlibMap));

    auto xmlMap = gcNew<PraiaMap>();
    registerXmlBuiltins(xmlMap);
    globals->define("xml", Value(xmlMap));

    auto plistMap = gcNew<PraiaMap>();
    registerPlistBuiltins(plistMap);
    globals->define("plist", Value(plistMap));

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
        }, {"min", "max"}));

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
        }, {"array"}));

    randomMap->entries[Value("shuffle")] = Value(makeNative("random.shuffle", 1,
        [rng](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("random.shuffle() requires an array", 0);
            auto& elems = args[0].asArray()->elements;
            std::shuffle(elems.begin(), elems.end(), *rng);
            return args[0];
        }, {"array"}));

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
        }, {"milliseconds"}));

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
        }, {"format", "timestamp", "utc"}));

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
        }, {"dateString", "format", "utc"}));

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

    // ── math namespace ──
    // Builtin globally, so users never need `use "math"`. (An earlier
    // grains/math.praia tried to "extend" this namespace with square
    // and cube, but the grain mechanism shadows rather than merges,
    // so the grain quietly wiped out PI/E/sqrt/.. for users who tried
    // it. square and cube live here now and the grain is gone.)

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
        }, {"a", "b"}));
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
        }, {"base", "exponent"}));

    mathMap->entries[Value("min")] = Value(makeNative("math.min", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("math.min() requires two numbers", 0);
            return Value(std::fmin(args[0].asNumber(), args[1].asNumber()));
        }, {"a", "b"}));

    mathMap->entries[Value("max")] = Value(makeNative("math.max", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("math.max() requires two numbers", 0);
            return Value(std::fmax(args[0].asNumber(), args[1].asNumber()));
        }, {"a", "b"}));

    mathMap->entries[Value("clamp")] = Value(makeNative("math.clamp", 3,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber() || !args[2].isNumber())
                throw RuntimeError("math.clamp() requires three numbers", 0);
            double x = args[0].asNumber(), lo = args[1].asNumber(), hi = args[2].asNumber();
            return Value(std::fmax(lo, std::fmin(x, hi)));
        }, {"x", "min", "max"}));

    mathMap->entries[Value("atan2")] = Value(makeNative("math.atan2", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("math.atan2() requires two numbers", 0);
            return Value(std::atan2(args[0].asNumber(), args[1].asNumber()));
        }, {"y", "x"}));

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
        }, {"a", "b", "epsilon"}));

    mathMap->entries[Value("square")] = Value(makeNative("math.square", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("math.square() requires a number", 0);
            double x = args[0].asNumber();
            return Value(x * x);
        }));

    mathMap->entries[Value("cube")] = Value(makeNative("math.cube", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("math.cube() requires a number", 0);
            double x = args[0].asNumber();
            return Value(x * x * x);
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

    // fs.readLines (canonical) + sys.readLines deprecated forwarder.
    FsImpl fsReadLines = [](const std::vector<Value>& args) -> Value {
        if (!args[0].isString())
            throw RuntimeError("fs.readLines() requires a string path", 0);
        std::ifstream f(args[0].asString());
        if (!f.is_open())
            throw RuntimeError("Cannot read file: " + args[0].asString(), 0);
        auto result = gcNew<PraiaArray>();
        std::string line;
        while (std::getline(f, line))
            result->elements.push_back(Value(std::move(line)));
        return Value(result);
    };
    fsMap->entries[Value("readLines")] = Value(makeNative("fs.readLines", 1, fsReadLines, {"path"}));
    {
        auto warned = std::make_shared<std::atomic<bool>>(false);
        sysMap->entries[Value("readLines")] = Value(makeNative("sys.readLines", 1,
            [warned, fsReadLines](const std::vector<Value>& args) -> Value {
                if (!warned->exchange(true))
                    std::cerr << "[deprecated] sys.readLines() is now fs.readLines(); "
                                 "update callers\n";
                return fsReadLines(args);
            }));
    }

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

    // sys.executable — canonical path of the running praia binary.
    // Captured at main() startup so it survives chdir; used by sand
    // to bake the right `exec <praia>` line into installed bin wrappers,
    // and by anyone else who needs to invoke a child interpreter at
    // the same version. nil only if main() failed to resolve it
    // (very unlikely in practice).
    if (!g_praiaExecPath.empty()) {
        sysMap->entries[Value("executable")] = Value(g_praiaExecPath);
    } else {
        sysMap->entries[Value("executable")] = Value();
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
        [](const std::vector<Value>&) -> Value {
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
                callSafe(*g_currentInterp, handler, {Value(name)});
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
    // The connection is exposed to Praia as an opaque PraiaExternal handle
    // (see praia_plugin.h). Wrapping sqlite3* + a `closed` flag in a small
    // struct lets explicit sqlite.close() and the GC-time deleter both be
    // idempotent — close-then-GC and double-close are no-ops, not
    // double-frees. This is the same pattern documented in PLUGINS.md;
    // the sqlite builtin is the in-tree reference implementation.
    struct SqliteConn {
        sqlite3* conn = nullptr;
        bool closed = false;
    };
    constexpr const char* kSqliteTypeTag = "sqlite.connection";

    // Binds params and validates: params must be an array; its length must
    // match the prepared statement's placeholder count; each bind call's
    // return code is checked so OOM / range errors surface. On any failure
    // the helper finalizes `stmt` before throwing so the caller doesn't
    // have to track partial cleanup.
    auto sqliteBind = [](sqlite3_stmt* stmt, const Value& paramsValue,
                         const char* fn) {
        if (!paramsValue.isArray()) {
            sqlite3_finalize(stmt);
            throw RuntimeError(std::string(fn) +
                "() params (argument 3) must be an array", 0);
        }
        auto& params = paramsValue.asArray()->elements;
        int expected = sqlite3_bind_parameter_count(stmt);
        if (static_cast<int>(params.size()) != expected) {
            sqlite3_finalize(stmt);
            throw RuntimeError(std::string(fn) + "() expected " +
                std::to_string(expected) + " bound parameter(s) but got " +
                std::to_string(params.size()), 0);
        }
        for (size_t i = 0; i < params.size(); i++) {
            int idx = static_cast<int>(i + 1);
            auto& p = params[i];
            int rc;
            if (p.isNil()) rc = sqlite3_bind_null(stmt, idx);
            else if (p.isBool()) rc = sqlite3_bind_int(stmt, idx, p.asBool() ? 1 : 0);
            // Bind ints as int64 so values above 2^53 round-trip exactly.
            // The original code coerced everything via sqlite3_bind_double,
            // losing precision on the way in.
            else if (p.isInt()) rc = sqlite3_bind_int64(stmt, idx, p.asInt());
            else if (p.isNumber()) rc = sqlite3_bind_double(stmt, idx, p.asNumber());
            else if (p.isString()) rc = sqlite3_bind_text(stmt, idx, p.asString().c_str(), -1, SQLITE_TRANSIENT);
            else rc = sqlite3_bind_text(stmt, idx, p.toString().c_str(), -1, SQLITE_TRANSIENT);
            if (rc != SQLITE_OK) {
                std::string err = sqlite3_errstr(rc);
                sqlite3_finalize(stmt);
                throw RuntimeError(std::string(fn) +
                    "() bind error on parameter " + std::to_string(idx) +
                    ": " + err, 0);
            }
        }
    };

    auto sqliteMap = gcNew<PraiaMap>();

    sqliteMap->entries[Value("open")] = Value(makeNative("sqlite.open", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sqlite.open() requires a path string", 0);

            auto* w = new SqliteConn();
            int rc = sqlite3_open(args[0].asString().c_str(), &w->conn);
            if (rc != SQLITE_OK) {
                // sqlite3_open can leave w->conn NULL on OOM in older
                // builds — guard before dereferencing.
                std::string err = w->conn ? sqlite3_errmsg(w->conn) : "out of memory";
                if (w->conn) sqlite3_close_v2(w->conn);
                delete w;
                throw RuntimeError("sqlite.open: " + err, 0);
            }
            return praia::makeExternal<SqliteConn>(w, kSqliteTypeTag,
                [](SqliteConn* p) {
                    if (!p->closed && p->conn) sqlite3_close_v2(p->conn);
                    delete p;
                });
        }));

    sqliteMap->entries[Value("query")] = Value(makeNative("sqlite.query", -1,
        [sqliteBind](const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || args.size() > 3)
                throw RuntimeError("sqlite.query() expected (conn, sql, params?)", 0);
            auto* w = praia::getExternal<SqliteConn>(args[0], kSqliteTypeTag);
            if (w->closed)
                throw RuntimeError("Database is closed", 0);
            if (!args[1].isString())
                throw RuntimeError("sqlite.query() argument 2 must be a string", 0);

            sqlite3_stmt* stmt = nullptr;
            int rc = sqlite3_prepare_v2(w->conn, args[1].asString().c_str(), -1, &stmt, nullptr);
            if (rc != SQLITE_OK) {
                std::string err = sqlite3_errmsg(w->conn);
                throw RuntimeError("SQL error: " + err, 0);
            }

            if (args.size() == 3) {
                sqliteBind(stmt, args[2], "sqlite.query");
            } else if (sqlite3_bind_parameter_count(stmt) != 0) {
                int expected = sqlite3_bind_parameter_count(stmt);
                sqlite3_finalize(stmt);
                throw RuntimeError("sqlite.query() expected " +
                    std::to_string(expected) +
                    " bound parameter(s) but params argument was omitted", 0);
            }

            auto rows = gcNew<PraiaArray>();
            int cols = sqlite3_column_count(stmt);

            int stepRc;
            while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
                auto row = gcNew<PraiaMap>();
                for (int c = 0; c < cols; c++) {
                    std::string name = sqlite3_column_name(stmt, c);
                    int type = sqlite3_column_type(stmt, c);
                    switch (type) {
                        case SQLITE_NULL:
                            row->entries[Value(name)] = Value();
                            break;
                        case SQLITE_INTEGER:
                            // int64 preserves the full 64-bit range; the
                            // prior code went through double and silently
                            // collapsed values above 2^53.
                            row->entries[Value(name)] = Value(static_cast<int64_t>(sqlite3_column_int64(stmt, c)));
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

            // Non-DONE terminal rc (BUSY, ERROR, MISUSE, interrupted, ...)
            // means the iteration aborted partway. Surface the error
            // instead of returning silently truncated results — sqlite.run
            // does the same check below.
            if (stepRc != SQLITE_DONE) {
                std::string err = sqlite3_errmsg(w->conn);
                sqlite3_finalize(stmt);
                throw RuntimeError("SQL error: " + err, 0);
            }
            sqlite3_finalize(stmt);
            return Value(rows);
        }));

    sqliteMap->entries[Value("run")] = Value(makeNative("sqlite.run", -1,
        [sqliteBind](const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || args.size() > 3)
                throw RuntimeError("sqlite.run() expected (conn, sql, params?)", 0);
            auto* w = praia::getExternal<SqliteConn>(args[0], kSqliteTypeTag);
            if (w->closed)
                throw RuntimeError("Database is closed", 0);
            if (!args[1].isString())
                throw RuntimeError("sqlite.run() argument 2 must be a string", 0);

            sqlite3_stmt* stmt = nullptr;
            int rc = sqlite3_prepare_v2(w->conn, args[1].asString().c_str(), -1, &stmt, nullptr);
            if (rc != SQLITE_OK) {
                std::string err = sqlite3_errmsg(w->conn);
                throw RuntimeError("SQL error: " + err, 0);
            }

            if (args.size() == 3) {
                sqliteBind(stmt, args[2], "sqlite.run");
            } else if (sqlite3_bind_parameter_count(stmt) != 0) {
                int expected = sqlite3_bind_parameter_count(stmt);
                sqlite3_finalize(stmt);
                throw RuntimeError("sqlite.run() expected " +
                    std::to_string(expected) +
                    " bound parameter(s) but params argument was omitted", 0);
            }

            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);

            if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                std::string err = sqlite3_errmsg(w->conn);
                throw RuntimeError("SQL error: " + err, 0);
            }

            auto result = gcNew<PraiaMap>();
            result->entries[Value("changes")] = Value(static_cast<int64_t>(sqlite3_changes(w->conn)));
            result->entries[Value("lastId")] = Value(static_cast<int64_t>(sqlite3_last_insert_rowid(w->conn)));
            return Value(result);
        }));

    sqliteMap->entries[Value("close")] = Value(makeNative("sqlite.close", 1,
        [](const std::vector<Value>& args) -> Value {
            auto* w = praia::getExternal<SqliteConn>(args[0], kSqliteTypeTag);
            if (!w->closed) {
                // sqlite3_close_v2 (vs the v1 close) defers actual cleanup
                // until outstanding prepared statements finalize, so we
                // can mark closed unconditionally and never see BUSY.
                if (w->conn) sqlite3_close_v2(w->conn);
                w->conn = nullptr;
                w->closed = true;
            }
            return Value();
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

            // Cache fast-path runs BEFORE the shutdown guard: a
            // cache hit returns an already-loaded module, which
            // doesn't enqueue any new hook, so it's safe during
            // teardown. Only a fresh load (which would push a hook
            // the drain loop has already passed) needs to be
            // refused.
            auto it = g_pluginCache.find(absPath);
            if (it != g_pluginCache.end())
                return Value(it->second);

            // Refuse a fresh load once teardown has begun. By the
            // time runPluginExitHooks flips this flag, the drain loop
            // has already published the snapshot it intends to call;
            // a hook pushed after that point would leak. In practice
            // no engine thread should still be running this far into
            // exit (the Interpreter destructor blocks on outstanding
            // futures first), so the throw is a defensive signal
            // rather than a hot path.
            if (g_pluginShutdown) {
                throw RuntimeError(
                    "loadNative(): cannot load plugins during process "
                    "exit — '" + path + "'", 0);
            }

            // dlopen
            void* handle = dlopen(absPath.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!handle) {
                std::string err = dlerror();
                throw RuntimeError("loadNative(): failed to load '" + path + "': " + err, 0);
            }

            // ABI version check — refuse plugins whose
            // PRAIA_DECLARE_ABI()-recorded version doesn't match this
            // praia. Same dlclose-safety as below: no plugin code has
            // run yet, so we can cleanly unload on mismatch. A missing
            // praia_abi_version symbol means the plugin was built
            // against pre-versioning headers (or forgot
            // PRAIA_DECLARE_ABI()) — either way, refuse so we don't
            // silently load something whose Value/PraiaMap layout we
            // can't vouch for.
            using AbiFn = int (*)();
            dlerror();
            auto abiFn = reinterpret_cast<AbiFn>(dlsym(handle, "praia_abi_version"));
            if (!abiFn) {
                dlclose(handle);
                throw RuntimeError(
                    "loadNative(): plugin '" + path +
                    "' does not declare an ABI version. Add "
                    "PRAIA_DECLARE_ABI(); at file scope and rebuild "
                    "(this praia expects ABI version " +
                    std::to_string(PRAIA_PLUGIN_ABI_VERSION) + ").", 0);
            }
            int pluginAbi = abiFn();
            if (pluginAbi != PRAIA_PLUGIN_ABI_VERSION) {
                dlclose(handle);
                throw RuntimeError(
                    "loadNative(): plugin '" + path +
                    "' declares ABI version " + std::to_string(pluginAbi) +
                    " but this praia expects version " +
                    std::to_string(PRAIA_PLUGIN_ABI_VERSION) +
                    ". Rebuild the plugin against the current headers.", 0);
            }

            // dlsym for the entry point. If this fails or returns null, no
            // plugin code has run yet - safe to dlclose so the handle doesn't
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
                // Don't dlclose here - plugin code may still be referenced
                // by std::function deleters in the partially-populated
                // moduleMap. Just wrap and re-throw with context.
                throw RuntimeError(
                    "loadNative(): plugin '" + path +
                    "' threw during registration: " + std::string(e.what()), 0);
            }

            // Optional plugin metadata. Each symbol is dlsym'd
            // independently so a plugin can declare any subset
            // (e.g. version but no description). When at least one
            // resolves to a non-null string, attach a `_meta`
            // sub-map to the module. Plugins that don't invoke
            // PRAIA_PLUGIN_METADATA() leave the module map exactly
            // as praia_register populated it.
            using MetaFn = const char* (*)();
            auto metaProbe = [&](const char* sym) -> const char* {
                dlerror();
                auto fn = reinterpret_cast<MetaFn>(dlsym(handle, sym));
                if (!fn) { dlerror(); return nullptr; }
                return fn();
            };
            const char* metaName = metaProbe("praia_plugin_name");
            const char* metaVer  = metaProbe("praia_plugin_version");
            const char* metaDesc = metaProbe("praia_plugin_description");
            if (metaName || metaVer || metaDesc) {
                auto meta = gcNew<PraiaMap>();
                if (metaName) meta->entries[Value("name")] = Value(std::string(metaName));
                if (metaVer)  meta->entries[Value("version")] = Value(std::string(metaVer));
                if (metaDesc) meta->entries[Value("description")] = Value(std::string(metaDesc));
                moduleMap->entries[Value("_meta")] = Value(meta);
            }

            // Success — cache the module
            g_pluginCache[absPath] = moduleMap;

            // Optional process-exit hook. dlsym independently of
            // registration; presence is an opt-in. Registered LAST,
            // after the cache assignment, so a throw between
            // praia_register and here (metadata probing or cache
            // alloc) doesn't leave a stale hook. If the same plugin
            // is then loaded a second time, the cache path returns
            // early and we never reach here again — a single
            // dlsym + push per plugin. Symbol is fixed: extern "C"
            // void praia_at_exit(void).
            using AtExitFn = void (*)();
            dlerror();
            if (auto atExitFn = reinterpret_cast<AtExitFn>(dlsym(handle, "praia_at_exit"))) {
                g_pluginAtExitHooks.push_back({atExitFn, absPath});
            } else {
                dlerror(); // swallow "symbol not found" — hook is optional
            }

            return Value(moduleMap);
        })));
}

// Process-exit teardown for plugins that exported praia_at_exit.
// Called from main() on every normal return path via a stack-local
// RAII guard, so it fires for both clean exits and sys.exit()
// returns. Idempotent — clears the registry after running so a
// nested guard (shouldn't happen, but defensively) doesn't call
// hooks twice.
//
// Iterates in reverse load order so plugins loaded later — which
// may depend on earlier plugins' state — tear down first. Errors
// from individual hooks are logged and swallowed; one bad plugin
// doesn't strand the others' cleanup.
void runPluginExitHooks() {
    // Flip the shutdown flag once, before any drain iteration, so a
    // concurrent loadNative (theoretical under current ordering)
    // observes shutdown and refuses to enqueue a new hook the drain
    // would never reach.
    {
        std::lock_guard<std::mutex> lock(g_pluginMutex);
        g_pluginShutdown = true;
    }
    while (true) {
        // Hold g_pluginMutex only long enough to pop the next hook.
        // loadNative takes the same mutex for the duration of a load;
        // serialising the pop against the push prevents a data race
        // if a still-running engine thread (e.g. an unawaited async
        // task) happens to call loadNative during exit. In practice
        // Interpreter's destructor blocks on outstanding futures
        // before PluginExitGuard fires, so the mutex is uncontended
        // here — but matching the loadNative side's locking
        // convention is cheap and removes the UB-shaped footgun if
        // the threading model ever changes.
        //
        // CRITICALLY: drop the lock BEFORE invoking entry.fn(). A
        // hook is plugin-controlled code that may take a long time
        // (flush, fsync, join worker threads). Holding the mutex
        // across that call would block any concurrent loadNative
        // indefinitely; even if the current architecture rules that
        // out, the lock-then-callback pattern is the kind of latent
        // deadlock generator we don't want to leave behind.
        PluginAtExitEntry entry;
        {
            std::lock_guard<std::mutex> lock(g_pluginMutex);
            if (g_pluginAtExitHooks.empty()) return;
            // Pop before invoking so a hook that re-enters loadNative
            // (shouldn't, but defensively) doesn't observe its own
            // half-run state.
            entry = std::move(g_pluginAtExitHooks.back());
            g_pluginAtExitHooks.pop_back();
        }
        try {
            entry.fn();
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[praia_at_exit] %s: %s\n",
                entry.pluginPath.c_str(), e.what());
        } catch (...) {
            std::fprintf(stderr,
                "[praia_at_exit] %s: unknown exception\n",
                entry.pluginPath.c_str());
        }
    }
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
