#pragma once

// Fiber must be included before system headers (it defines _XOPEN_SOURCE)
#include "fiber.h"

#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

// Forward declarations
struct Callable;
struct PraiaArray;
struct PraiaMap;
struct PraiaInstance;
struct PraiaFuture;
struct PraiaGenerator;
struct PraiaTagged;
class Interpreter;
class Environment;

// Runtime error with line info
struct RuntimeError : std::runtime_error {
    int line;
    int column;
    RuntimeError(const std::string& msg, int line, int column = 0)
        : std::runtime_error(msg), line(line), column(column) {}
};

inline std::string formatLocation(int line, int column = 0) {
    if (column > 0)
        return "[line " + std::to_string(line) + ":col " + std::to_string(column) + "]";
    return "[line " + std::to_string(line) + "]";
}

// The universal Praia value type
struct Value {
    using Data = std::variant<
        std::nullptr_t,
        bool,
        int64_t,
        double,
        std::string,
        std::shared_ptr<Callable>,
        std::shared_ptr<PraiaArray>,
        std::shared_ptr<PraiaMap>,
        std::shared_ptr<PraiaInstance>,
        std::shared_ptr<PraiaFuture>,
        std::shared_ptr<PraiaGenerator>,
        std::shared_ptr<PraiaTagged>
    >;

    Data data;

    Value() : data(nullptr) {}
    Value(std::nullptr_t) : data(nullptr) {}
    Value(bool b) : data(b) {}
    Value(int64_t i) : data(i) {}
    Value(int i) : data(static_cast<int64_t>(i)) {}
    Value(double d) : data(d) {}
    Value(const std::string& s) : data(s) {}
    Value(std::string&& s) : data(std::move(s)) {}
    Value(const char* s) : data(std::string(s)) {}
    Value(std::shared_ptr<Callable> c) : data(std::move(c)) {}
    Value(std::shared_ptr<PraiaArray> a) : data(std::move(a)) {}
    Value(std::shared_ptr<PraiaMap> m) : data(std::move(m)) {}
    Value(std::shared_ptr<PraiaInstance> i) : data(std::move(i)) {}
    Value(std::shared_ptr<PraiaFuture> f) : data(std::move(f)) {}
    Value(std::shared_ptr<PraiaGenerator> g) : data(std::move(g)) {}
    Value(std::shared_ptr<PraiaTagged> t) : data(std::move(t)) {}

    bool isNil()      const { return std::holds_alternative<std::nullptr_t>(data); }
    bool isBool()     const { return std::holds_alternative<bool>(data); }
    bool isInt()      const { return std::holds_alternative<int64_t>(data); }
    bool isDouble()   const { return std::holds_alternative<double>(data); }
    bool isNumber()   const { return isInt() || isDouble(); }
    bool isString()   const { return std::holds_alternative<std::string>(data); }
    bool isCallable() const { return std::holds_alternative<std::shared_ptr<Callable>>(data); }
    bool isArray()    const { return std::holds_alternative<std::shared_ptr<PraiaArray>>(data); }
    bool isMap()      const { return std::holds_alternative<std::shared_ptr<PraiaMap>>(data); }
    bool isInstance() const { return std::holds_alternative<std::shared_ptr<PraiaInstance>>(data); }
    bool isFuture()   const { return std::holds_alternative<std::shared_ptr<PraiaFuture>>(data); }
    bool isGenerator() const { return std::holds_alternative<std::shared_ptr<PraiaGenerator>>(data); }
    bool isTagged()   const { return std::holds_alternative<std::shared_ptr<PraiaTagged>>(data); }

    bool                        asBool()     const { return std::get<bool>(data); }
    int64_t                     asInt()      const { return std::get<int64_t>(data); }
    // asNumber() returns double regardless of int/double storage
    double                      asNumber()   const {
        if (isInt()) return static_cast<double>(std::get<int64_t>(data));
        return std::get<double>(data);
    }
    const std::string&          asString()   const { return std::get<std::string>(data); }
    std::shared_ptr<Callable>   asCallable() const { return std::get<std::shared_ptr<Callable>>(data); }
    std::shared_ptr<PraiaArray> asArray()    const { return std::get<std::shared_ptr<PraiaArray>>(data); }
    std::shared_ptr<PraiaMap>      asMap()      const { return std::get<std::shared_ptr<PraiaMap>>(data); }
    std::shared_ptr<PraiaInstance> asInstance() const { return std::get<std::shared_ptr<PraiaInstance>>(data); }
    std::shared_ptr<PraiaFuture>   asFuture()   const { return std::get<std::shared_ptr<PraiaFuture>>(data); }
    std::shared_ptr<PraiaGenerator> asGenerator() const { return std::get<std::shared_ptr<PraiaGenerator>>(data); }
    std::shared_ptr<PraiaTagged>   asTagged()   const { return std::get<std::shared_ptr<PraiaTagged>>(data); }

    // Declared here, defined after PraiaArray/PraiaMap (needs complete types)
    bool isTruthy() const;
    std::string toString() const;
    bool operator==(const Value& o) const;
    bool operator!=(const Value& o) const { return !(*this == o); }
};

// Concrete types — defined after Value so they can hold Values
struct PraiaArray {
    std::vector<Value> elements;
};

inline bool isHashable(const Value& v) {
    return v.isNil() || v.isBool() || v.isInt() || v.isDouble() || v.isString();
}

struct ValueHash {
    size_t operator()(const Value& v) const {
        if (v.isNil()) return 0;
        if (v.isBool()) return std::hash<bool>{}(v.asBool());
        // Hash all numbers as double to be consistent with operator==
        // (which compares int and double via asNumber())
        if (v.isInt() || v.isDouble()) return std::hash<double>{}(v.asNumber());
        if (v.isString()) return std::hash<std::string>{}(v.asString());
        if (v.isTagged()) return hashTagged(v);
        return 0;
    }
    static size_t hashTagged(const Value& v);
};

struct PraiaMap {
    std::unordered_map<Value, Value, ValueHash> entries;
};

struct PraiaFuture {
    std::shared_future<Value> future;
};

struct PraiaGenerator {
    enum class State { CREATED, SUSPENDED, RUNNING, COMPLETED };
    State state = State::CREATED;
    Value lastYielded;
    Value sendValue;
    std::string errorMessage; // stores error from generator body for propagation
    bool done = false;
    bool isVM = false;

    // Tree-walker: fiber-based coroutine (ucontext)
    std::unique_ptr<Fiber> fiber;
    std::shared_ptr<Environment> fiberEnv; // kept alive for GC marking

    // VM: snapshot state (filled on yield, restored on next)
    std::vector<Value> savedStack;
    int savedBaseSlot = 0;
    int savedFrameCount = 0;
    const uint8_t* savedIp = nullptr;
    void* vmClosure = nullptr; // ObjClosure* — stored as void* to avoid circular include

    // prevent GC of closures/upvalues used by the generator
    std::vector<std::shared_ptr<void>> ownedResources;

    ~PraiaGenerator();  // defined in interpreter_callables.cpp (needs Fiber complete type)

    // Eagerly release the fiber stack and captured environment once the
    // generator has run to completion. The shared_ptr<PraiaGenerator> may
    // outlive the actual generation (held by the GC, by user code that
    // forgot to drop the reference, etc.) — without this, each abandoned
    // generator would hold its 256KB fiber stack until the next GC cycle.
    // Safe to call only when state == COMPLETED. No-op if already released.
    void releaseAfterCompletion();  // defined in interpreter_callables.cpp
};

struct PraiaClass;  // defined in interpreter.h (it's a Callable)

struct PraiaInstance {
    std::shared_ptr<PraiaClass> klass;
    std::unordered_map<std::string, Value> fields;
};

struct PraiaTagged {
    std::string tag;
    std::vector<Value> values;
};

inline size_t ValueHash::hashTagged(const Value& v) {
    return std::hash<std::string>{}(v.asTagged()->tag);
}

// ── Value method definitions (need complete types) ──

inline bool Value::isTruthy() const {
    if (isNil()) return false;
    if (isBool()) return asBool();
    return true;
}

inline std::string Value::toString() const {
    if (isNil())    return "nil";
    if (isBool())   return asBool() ? "true" : "false";
    if (isInt())    return std::to_string(asInt());
    if (isDouble()) { std::ostringstream o; o << asNumber(); return o.str(); }
    if (isString()) return asString();
    if (isCallable()) return "<function>";
    if (isInstance()) return "<instance>";
    if (isFuture()) return "<future>";
    if (isGenerator()) return "<generator>";
    if (isArray()) {
        std::ostringstream o;
        o << "[";
        auto& elems = asArray()->elements;
        for (size_t i = 0; i < elems.size(); i++) {
            if (i > 0) o << ", ";
            if (elems[i].isString()) o << "\"" << elems[i].toString() << "\"";
            else o << elems[i].toString();
        }
        o << "]";
        return o.str();
    }
    if (isMap()) {
        std::ostringstream o;
        o << "{";
        bool first = true;
        for (auto& [k, v] : asMap()->entries) {
            if (!first) o << ", ";
            first = false;
            if (k.isString()) o << k.asString();
            else o << k.toString();
            o << ": ";
            if (v.isString()) o << "\"" << v.toString() << "\"";
            else o << v.toString();
        }
        o << "}";
        return o.str();
    }
    if (isTagged()) {
        const auto& t = asTagged();
        std::ostringstream o;
        o << t->tag << "(";
        for (size_t i = 0; i < t->values.size(); i++) {
            if (i > 0) o << ", ";
            if (t->values[i].isString()) o << "\"" << t->values[i].toString() << "\"";
            else o << t->values[i].toString();
        }
        o << ")";
        return o.str();
    }
    return "<unknown>";
}

inline bool Value::operator==(const Value& o) const {
    if (isNil()    && o.isNil())    return true;
    if (isNil()    || o.isNil())    return false;
    if (isBool()   && o.isBool())   return asBool()   == o.asBool();
    if (isNumber() && o.isNumber()) return asNumber()  == o.asNumber();
    if (isString() && o.isString()) return asString()  == o.asString();
    if (isArray()  && o.isArray()) {
        auto& a = asArray()->elements;
        auto& b = o.asArray()->elements;
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (a[i] != b[i]) return false;
        return true;
    }
    if (isMap() && o.isMap()) {
        auto& a = asMap()->entries;
        auto& b = o.asMap()->entries;
        if (a.size() != b.size()) return false;
        for (auto& [k, v] : a) {
            auto it = b.find(k);
            if (it == b.end() || it->second != v) return false;
        }
        return true;
    }
    // Instances: reference equality
    if (isInstance() && o.isInstance())
        return asInstance() == o.asInstance();
    if (isTagged() && o.isTagged()) {
        const auto& a = asTagged();
        const auto& b = o.asTagged();
        if (a->tag != b->tag || a->values.size() != b->values.size()) return false;
        for (size_t i = 0; i < a->values.size(); i++)
            if (a->values[i] != b->values[i]) return false;
        return true;
    }
    return false;
}

// Interface for anything that can be called (user functions + built-ins)
struct Callable {
    virtual ~Callable() = default;
    virtual Value call(Interpreter& interp, const std::vector<Value>& args) = 0;
    virtual int arity() const = 0;   // -1 = variadic
    virtual std::string name() const = 0;
    virtual const std::vector<std::string>* paramNames() const { return nullptr; }
};
