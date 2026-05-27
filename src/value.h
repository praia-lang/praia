#pragma once

// Fiber must be included before system headers (it defines _XOPEN_SOURCE)
#include "fiber.h"

#include <cmath>
#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// Forward declarations
struct Callable;
struct PraiaArray;
struct PraiaMap;
struct PraiaSet;
struct PraiaInstance;
struct PraiaFuture;
struct PraiaGenerator;
struct PraiaTagged;
struct PraiaExternal;
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
        std::shared_ptr<PraiaSet>,
        std::shared_ptr<PraiaInstance>,
        std::shared_ptr<PraiaFuture>,
        std::shared_ptr<PraiaGenerator>,
        std::shared_ptr<PraiaTagged>,
        std::shared_ptr<PraiaExternal>
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
    Value(std::shared_ptr<PraiaSet> s) : data(std::move(s)) {}
    Value(std::shared_ptr<PraiaInstance> i) : data(std::move(i)) {}
    Value(std::shared_ptr<PraiaFuture> f) : data(std::move(f)) {}
    Value(std::shared_ptr<PraiaGenerator> g) : data(std::move(g)) {}
    Value(std::shared_ptr<PraiaTagged> t) : data(std::move(t)) {}
    Value(std::shared_ptr<PraiaExternal> e) : data(std::move(e)) {}

    bool isNil()      const { return std::holds_alternative<std::nullptr_t>(data); }
    bool isBool()     const { return std::holds_alternative<bool>(data); }
    bool isInt()      const { return std::holds_alternative<int64_t>(data); }
    bool isDouble()   const { return std::holds_alternative<double>(data); }
    bool isNumber()   const { return isInt() || isDouble(); }
    bool isString()   const { return std::holds_alternative<std::string>(data); }
    bool isCallable() const { return std::holds_alternative<std::shared_ptr<Callable>>(data); }
    bool isArray()    const { return std::holds_alternative<std::shared_ptr<PraiaArray>>(data); }
    bool isMap()      const { return std::holds_alternative<std::shared_ptr<PraiaMap>>(data); }
    bool isSet()      const { return std::holds_alternative<std::shared_ptr<PraiaSet>>(data); }
    bool isInstance() const { return std::holds_alternative<std::shared_ptr<PraiaInstance>>(data); }
    bool isFuture()   const { return std::holds_alternative<std::shared_ptr<PraiaFuture>>(data); }
    bool isGenerator() const { return std::holds_alternative<std::shared_ptr<PraiaGenerator>>(data); }
    bool isTagged()   const { return std::holds_alternative<std::shared_ptr<PraiaTagged>>(data); }
    bool isExternal() const { return std::holds_alternative<std::shared_ptr<PraiaExternal>>(data); }

    bool                        asBool()     const { return std::get<bool>(data); }
    int64_t                     asInt()      const { return std::get<int64_t>(data); }
    // asNumber() returns double regardless of int/double storage
    double                      asNumber()   const {
        if (isInt()) return static_cast<double>(std::get<int64_t>(data));
        return std::get<double>(data);
    }
    // For bitwise ops: returns int64 directly if isInt() (no precision
    // loss for |n| > 2^53), otherwise rounds the double via static_cast.
    // Callers must verify isNumber() first.
    int64_t                     toInt64ForBitwise() const {
        if (isInt()) return std::get<int64_t>(data);
        return static_cast<int64_t>(std::get<double>(data));
    }
    const std::string&          asString()   const { return std::get<std::string>(data); }
    std::shared_ptr<Callable>   asCallable() const { return std::get<std::shared_ptr<Callable>>(data); }
    std::shared_ptr<PraiaArray> asArray()    const { return std::get<std::shared_ptr<PraiaArray>>(data); }
    std::shared_ptr<PraiaMap>      asMap()      const { return std::get<std::shared_ptr<PraiaMap>>(data); }
    std::shared_ptr<PraiaSet>      asSet()      const { return std::get<std::shared_ptr<PraiaSet>>(data); }
    std::shared_ptr<PraiaInstance> asInstance() const { return std::get<std::shared_ptr<PraiaInstance>>(data); }
    std::shared_ptr<PraiaFuture>   asFuture()   const { return std::get<std::shared_ptr<PraiaFuture>>(data); }
    std::shared_ptr<PraiaGenerator> asGenerator() const { return std::get<std::shared_ptr<PraiaGenerator>>(data); }
    std::shared_ptr<PraiaTagged>   asTagged()   const { return std::get<std::shared_ptr<PraiaTagged>>(data); }
    std::shared_ptr<PraiaExternal> asExternal() const { return std::get<std::shared_ptr<PraiaExternal>>(data); }

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

// ── Numeric comparison + hashing helpers ────────────────────────────────
//
// 64-bit integers and IEEE-754 doubles can't be losslessly intermixed for
// equality/ordering/hashing without care. The naive approach of routing
// everything through `asNumber()` (double) is wrong for `int` values whose
// magnitude exceeds 2^53 — adjacent ints round to the same double and
// compare equal, collapse to one map key, and hash to one bucket.
//
// The rules below preserve exact int64 semantics for pure-int operations
// while still letting mixed int↔double compare in the obvious way:
//
//   - int  vs int  : compare as int64 (exact).
//   - dbl  vs dbl  : compare as double (IEEE 754).
//   - int  vs dbl  : if the double is integer-valued and within i64 range,
//                    convert the double to int64 and compare exactly.
//                    Otherwise (fractional, infinite, or out of i64 range)
//                    promote the int to double and compare as double.
//
// For hashing to be consistent with `numbersEqual`, an int that is
// *exactly* representable as a double must hash to the same value as that
// double. The check `(int64_t)(double)n == n` decides whether n is
// representable. Ints outside that range hash via their int64
// representation, which can't collide with any double (since no double
// equals them under the rules above).
//
// `numbersEqualNaNEq` is the map-key variant that treats NaN == NaN, like
// JavaScript Map / Java HashMap / Swift Dictionary. The user-visible `==`
// keeps IEEE 754 NaN != NaN.

inline bool numbersEqualHelper(const Value& a, const Value& b, bool nanEq) {
    if (a.isInt() && b.isInt()) return a.asInt() == b.asInt();
    if (a.isDouble() && b.isDouble()) {
        double da = a.asNumber(), db = b.asNumber();
        if (nanEq && std::isnan(da) && std::isnan(db)) return true;
        return da == db;
    }
    // Mixed. Pick the int side and the double side.
    int64_t i = a.isInt() ? a.asInt() : b.asInt();
    double  d = a.isDouble() ? std::get<double>(a.data) : std::get<double>(b.data);
    if (nanEq && std::isnan(d)) return false; // int can't be NaN
    // If the double is integer-valued and fits in [INT64_MIN, INT64_MAX],
    // compare as int — exact even for big ints whose double-form would lose
    // precision.
    if (std::isfinite(d) && d == std::trunc(d) &&
        d >= -9223372036854775808.0 && d <  9223372036854775808.0) {
        return i == static_cast<int64_t>(d);
    }
    // The fallthrough cases all mean "no int64 equals d exactly":
    //   • d is NaN or ±Inf
    //   • d is fractional
    //   • d is integer-valued but outside [-2^63, 2^63) — for
    //     example d == 2^63, which is what (double)INT64_MAX
    //     rounds up to.
    //
    // The historical `(double)i == d` fallback let case 3 return
    // true: INT64_MAX compared equal to 2^63 (and hashed to a
    // different bucket, so map insertion silently overwrote
    // adjacent ints). Returning false keeps the hash/equality
    // contract consistent with hashNumber above.
    return false;
}

inline bool numbersEqual(const Value& a, const Value& b) {
    return numbersEqualHelper(a, b, /*nanEq=*/false);
}

inline bool numbersLess(const Value& a, const Value& b) {
    if (a.isInt() && b.isInt()) return a.asInt() < b.asInt();
    if (a.isDouble() && b.isDouble()) return a.asNumber() < b.asNumber();
    // Mixed. Same int-side / double-side recovery as numbersEqual.
    bool aIsInt = a.isInt();
    int64_t i = aIsInt ? a.asInt() : b.asInt();
    double  d = aIsInt ? std::get<double>(b.data) : std::get<double>(a.data);
    if (std::isnan(d)) return false; // ordering with NaN is always false
    if (std::isfinite(d) && d == std::trunc(d) &&
        d >= -9223372036854775808.0 && d <  9223372036854775808.0) {
        int64_t di = static_cast<int64_t>(d);
        return aIsInt ? (i < di) : (di < i);
    }
    return aIsInt ? (static_cast<double>(i) < d) : (d < static_cast<double>(i));
}

inline size_t hashNumber(const Value& v) {
    if (v.isInt()) {
        int64_t n = v.asInt();
        // If n is exactly representable as a double, hash via double so
        // it collides with the equal double value. Otherwise hash int64.
        //
        // The double check needs explicit bounds before the int64
        // cast: (double)INT64_MAX rounds up to 2^63, and casting
        // 2^63 back to int64 is undefined behaviour. The < 2^63
        // bound here mirrors the equality logic in numbersEqualHelper
        // so hash and equality agree on which ints "look like" a
        // double — without it, n = INT64_MAX would erroneously hash
        // via double and silently collide with the float 2^63.
        double d = static_cast<double>(n);
        if (std::isfinite(d) &&
            d >= -9223372036854775808.0 && d < 9223372036854775808.0 &&
            static_cast<int64_t>(d) == n) {
            return std::hash<double>{}(d);
        }
        return std::hash<int64_t>{}(n);
    }
    double d = std::get<double>(v.data);
    if (std::isnan(d)) return 0;       // canonicalize NaN
    if (d == 0.0) return std::hash<double>{}(0.0); // canonicalize ±0
    return std::hash<double>{}(d);
}

struct ValueHash {
    size_t operator()(const Value& v) const {
        if (v.isNil()) return 0;
        if (v.isBool()) return std::hash<bool>{}(v.asBool());
        if (v.isInt() || v.isDouble()) return hashNumber(v);
        if (v.isString()) return std::hash<std::string>{}(v.asString());
        if (v.isTagged()) return hashTagged(v);
        return 0;
    }
    static size_t hashTagged(const Value& v);
};

// Key-equality predicate for hash tables. Differs from Value::operator== on
// one point: NaN compares equal to NaN here. This matches the convention used
// by JavaScript Map / Java HashMap / Swift Dictionary — keys round-trip even
// when the key is NaN. The user-visible `==` operator on Praia values still
// follows IEEE 754 (NaN != NaN); only PraiaMap lookups use this predicate.
struct ValueKeyEqual {
    bool operator()(const Value& a, const Value& b) const {
        if (a.isNumber() && b.isNumber())
            return numbersEqualHelper(a, b, /*nanEq=*/true);
        return a == b;
    }
};

struct PraiaMap {
    std::unordered_map<Value, Value, ValueHash, ValueKeyEqual> entries;
};

// Hash set of Praia values. Storage uses the same ValueHash +
// ValueKeyEqual predicates that PraiaMap uses for its keys, so
// membership semantics match (NaN==NaN, numeric coercion via
// numbersEqualHelper, etc.). Elements must be `isHashable` —
// nested sets/maps/arrays are rejected at insertion time, matching
// the existing rule for map keys.
struct PraiaSet {
    std::unordered_set<Value, ValueHash, ValueKeyEqual> elements;
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

// Opaque handle wrapping a C/C++ pointer plus a destructor.
//
// Plugins use this to return native resources (DB connections,
// file handles, OS objects, etc.) to user code without exposing
// the underlying layout. From Praia's perspective the value is
// opaque — passable, identity-comparable, but the data inside
// can only be read back through the plugin that produced it.
//
// `data` is the raw pointer. `typeName` is a stable tag the
// plugin uses to type-check the unwrap (convention:
// "module.type", e.g. "sqlite.connection"). `deleter` runs in
// the destructor when the last reference drops, courtesy of the
// GC sweep that releases the heap entry. Move-only: a
// PraiaExternal owns its `data`, copying would double-free.
struct PraiaExternal {
    void* data = nullptr;
    std::string typeName;
    std::function<void(void*)> deleter;

    PraiaExternal() = default;
    PraiaExternal(const PraiaExternal&) = delete;
    PraiaExternal& operator=(const PraiaExternal&) = delete;
    // Defaulted moves so the struct is move-only rather than
    // pinned. The user-declared destructor would otherwise
    // suppress the implicit move ops alongside the explicitly
    // deleted copy ones — leaving the type uncopyable AND
    // unmovable, which is more restrictive than "unique
    // ownership" implies and would block emplace/make_shared
    // patterns that hand off ownership at construction.
    PraiaExternal(PraiaExternal&&) = default;
    PraiaExternal& operator=(PraiaExternal&&) = default;
    ~PraiaExternal() { if (deleter && data) deleter(data); }
};

inline size_t ValueHash::hashTagged(const Value& v) {
    return std::hash<std::string>{}(v.asTagged()->tag);
}

// ── Value method definitions (need complete types) ──

// Cycle-aware recursive helper for Value::toString. Tracks the raw
// pointers of containers on the active recursion path; if a container
// re-enters itself, emits a placeholder marker instead of recursing
// forever. Pointers are erased on the way out so shared-but-non-cyclic
// sub-structures still serialize fully on each occurrence.
//
// (Free function rather than a private method to avoid changing the
// Value declaration; called only from Value::toString.)
inline std::string valueToStringRec(const Value& v, std::unordered_set<const void*>& visited);

inline bool Value::isTruthy() const {
    if (isNil()) return false;
    if (isBool()) return asBool();
    return true;
}

inline std::string Value::toString() const {
    std::unordered_set<const void*> visited;
    return valueToStringRec(*this, visited);
}

inline std::string valueToStringRec(const Value& v, std::unordered_set<const void*>& visited) {
    if (v.isNil())    return "nil";
    if (v.isBool())   return v.asBool() ? "true" : "false";
    if (v.isInt())    return std::to_string(v.asInt());
    if (v.isDouble()) { std::ostringstream o; o << v.asNumber(); return o.str(); }
    if (v.isString()) return v.asString();
    if (v.isCallable()) return "<function>";
    if (v.isInstance()) return "<instance>";
    if (v.isFuture()) return "<future>";
    if (v.isGenerator()) return "<generator>";
    if (v.isExternal()) {
        auto& tn = v.asExternal()->typeName;
        return tn.empty() ? "<external>" : "<external:" + tn + ">";
    }
    if (v.isArray()) {
        const void* key = static_cast<const void*>(v.asArray().get());
        if (!visited.insert(key).second) return "[...]";
        std::ostringstream o;
        o << "[";
        auto& elems = v.asArray()->elements;
        for (size_t i = 0; i < elems.size(); i++) {
            if (i > 0) o << ", ";
            if (elems[i].isString()) o << "\"" << valueToStringRec(elems[i], visited) << "\"";
            else o << valueToStringRec(elems[i], visited);
        }
        o << "]";
        visited.erase(key);
        return o.str();
    }
    if (v.isMap()) {
        const void* key = static_cast<const void*>(v.asMap().get());
        if (!visited.insert(key).second) return "{...}";
        std::ostringstream o;
        o << "{";
        bool first = true;
        for (auto& [k, val] : v.asMap()->entries) {
            if (!first) o << ", ";
            first = false;
            if (k.isString()) o << k.asString();
            else o << valueToStringRec(k, visited);
            o << ": ";
            if (val.isString()) o << "\"" << valueToStringRec(val, visited) << "\"";
            else o << valueToStringRec(val, visited);
        }
        o << "}";
        visited.erase(key);
        return o.str();
    }
    if (v.isSet()) {
        const void* key = static_cast<const void*>(v.asSet().get());
        if (!visited.insert(key).second) return "#{...}";
        std::ostringstream o;
        o << "#{";
        bool first = true;
        for (auto& e : v.asSet()->elements) {
            if (!first) o << ", ";
            first = false;
            if (e.isString()) o << "\"" << valueToStringRec(e, visited) << "\"";
            else o << valueToStringRec(e, visited);
        }
        o << "}";
        visited.erase(key);
        return o.str();
    }
    if (v.isTagged()) {
        const auto& t = v.asTagged();
        const void* key = static_cast<const void*>(t.get());
        if (!visited.insert(key).second) return t->tag + "(...)";
        std::ostringstream o;
        o << t->tag << "(";
        for (size_t i = 0; i < t->values.size(); i++) {
            if (i > 0) o << ", ";
            if (t->values[i].isString()) o << "\"" << valueToStringRec(t->values[i], visited) << "\"";
            else o << valueToStringRec(t->values[i], visited);
        }
        o << ")";
        visited.erase(key);
        return o.str();
    }
    return "<unknown>";
}

inline bool Value::operator==(const Value& o) const {
    if (isNil()    && o.isNil())    return true;
    if (isNil()    || o.isNil())    return false;
    if (isBool()   && o.isBool())   return asBool()   == o.asBool();
    if (isNumber() && o.isNumber()) return numbersEqual(*this, o);
    if (isString() && o.isString()) return asString()  == o.asString();
    if (isArray()  && o.isArray()) {
        // Identity short-circuit — covers `a == a` for cyclic `a`. Two
        // distinct cyclic structures that happen to share shape still
        // recurse and may stack-overflow; for the practical case
        // (comparing a value with itself) this terminates.
        if (asArray().get() == o.asArray().get()) return true;
        auto& a = asArray()->elements;
        auto& b = o.asArray()->elements;
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (a[i] != b[i]) return false;
        return true;
    }
    if (isMap() && o.isMap()) {
        if (asMap().get() == o.asMap().get()) return true;
        auto& a = asMap()->entries;
        auto& b = o.asMap()->entries;
        if (a.size() != b.size()) return false;
        for (auto& [k, v] : a) {
            auto it = b.find(k);
            if (it == b.end() || it->second != v) return false;
        }
        return true;
    }
    if (isSet() && o.isSet()) {
        if (asSet().get() == o.asSet().get()) return true;
        auto& a = asSet()->elements;
        auto& b = o.asSet()->elements;
        if (a.size() != b.size()) return false;
        for (auto& e : a)
            if (b.find(e) == b.end()) return false;
        return true;
    }
    // Instances: reference equality
    if (isInstance() && o.isInstance())
        return asInstance() == o.asInstance();
    if (isTagged() && o.isTagged()) {
        const auto& a = asTagged();
        const auto& b = o.asTagged();
        if (a.get() == b.get()) return true;
        if (a->tag != b->tag || a->values.size() != b->values.size()) return false;
        for (size_t i = 0; i < a->values.size(); i++)
            if (a->values[i] != b->values[i]) return false;
        return true;
    }
    // Externals: pointer identity only. The wrapped data is opaque
    // from Praia's perspective, so there's no meaningful deep
    // comparison — two handles are equal iff they're the same handle.
    if (isExternal() && o.isExternal())
        return asExternal().get() == o.asExternal().get();
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
