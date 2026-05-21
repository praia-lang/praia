#include "praia_plugin.h"
#include <cmath>

PRAIA_DECLARE_ABI();
PRAIA_PLUGIN_METADATA("mathext", "0.1.0",
                     "Extra math functions: gcd, lcm, fibonacci, hypot, sum, filter");

extern "C" void praia_register(PraiaMap* module) {
    // mathext.gcd(a, b) — greatest common divisor. Declares named
    // parameters so user code can call `mathext.gcd(a: 48, b: 18)`
    // as well as positionally. Uses praia::requireNumber to validate
    // input — same error wording as Praia's own builtins.
    module->entries["gcd"] = Value(makeNative("mathext.gcd", 2,
        [](const std::vector<Value>& args) -> Value {
            int64_t a = static_cast<int64_t>(
                praia::requireNumber(args, 0, "mathext.gcd"));
            int64_t b = static_cast<int64_t>(
                praia::requireNumber(args, 1, "mathext.gcd"));
            while (b != 0) { int64_t t = b; b = a % b; a = t; }
            return Value(a < 0 ? -a : a);
        },
        {"a", "b"}));

    // mathext.lcm(a, b) — least common multiple
    module->entries["lcm"] = Value(makeNative("mathext.lcm", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("mathext.lcm() requires two numbers", 0);
            int64_t a = static_cast<int64_t>(args[0].asNumber());
            int64_t b = static_cast<int64_t>(args[1].asNumber());
            if (a == 0 || b == 0) return Value(static_cast<int64_t>(0));
            int64_t g = a;
            int64_t tmp = b;
            while (tmp != 0) { int64_t t = tmp; tmp = g % tmp; g = t; }
            return Value(static_cast<int64_t>(std::abs(a / g * b)));
        }));

    // mathext.fibonacci(n) — nth Fibonacci number
    module->entries["fibonacci"] = Value(makeNative("mathext.fibonacci", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("mathext.fibonacci() requires a number", 0);
            int n = static_cast<int>(args[0].asNumber());
            if (n < 0) throw RuntimeError("mathext.fibonacci() requires non-negative number", 0);
            if (n <= 1) return Value(static_cast<int64_t>(n));
            int64_t a = 0, b = 1;
            for (int i = 2; i <= n; i++) { int64_t t = a + b; a = b; b = t; }
            return Value(b);
        }));

    // mathext.hypot(a, b) — hypotenuse
    module->entries["hypot"] = Value(makeNative("mathext.hypot", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("mathext.hypot() requires two numbers", 0);
            return Value(std::hypot(args[0].asNumber(), args[1].asNumber()));
        }));

    // mathext.sum(array) — sum all numbers in an array
    module->entries["sum"] = Value(makeNative("mathext.sum", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("mathext.sum() requires an array", 0);
            bool allInt = true;
            int64_t intTotal = 0;
            double floatTotal = 0;
            for (auto& elem : args[0].asArray()->elements) {
                if (!elem.isNumber())
                    throw RuntimeError("mathext.sum(): array must contain only numbers", 0);
                if (elem.isInt()) {
                    intTotal += elem.asInt();
                    floatTotal += static_cast<double>(elem.asInt());
                } else {
                    allInt = false;
                    floatTotal += elem.asNumber();
                }
            }
            if (allInt) return Value(intTotal);
            return Value(floatTotal);
        }));

    // mathext.filter(array, predicate) — keep elements where the
    // user-supplied predicate returns truthy. Demonstrates calling
    // back into Praia code from a native plugin via praia::call.
    //
    // We snapshot the input array's elements before iterating so a
    // predicate that mutates the source (e.g. pushes/pops on it via
    // a closed-over reference) doesn't invalidate our iterator or
    // race with the std::vector reallocating mid-loop.
    module->entries["filter"] = Value(makeNative("mathext.filter", 2,
        [](const std::vector<Value>& args) -> Value {
            auto arr  = praia::requireArray   (args, 0, "mathext.filter");
            auto pred = praia::requireCallable(args, 1, "mathext.filter");
            std::vector<Value> snapshot = arr->elements;
            auto out = gcNew<PraiaArray>();
            for (auto& elem : snapshot) {
                Value keep = praia::call(pred, {elem});
                if (keep.isTruthy()) out->elements.push_back(elem);
            }
            return Value(out);
        },
        {"array", "predicate"}));
}
