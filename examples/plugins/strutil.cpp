// Example: wrapping C standard library functions as a Praia plugin.
// Demonstrates how to expose C APIs (or any C library) to Praia.

#include "praia_plugin.h"
#include <cstring>   // C string functions
#include <cstdlib>   // atoi, atof, strtol
#include <cctype>    // isalpha, isdigit, etc.

PRAIA_DECLARE_ABI();

extern "C" void praia_register(PraiaMap* module) {
    // strutil.isAlpha(str) — true if all characters are alphabetic
    module->entries["isAlpha"] = Value(makeNative("strutil.isAlpha", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("strutil.isAlpha() requires a string", 0);
            auto& s = args[0].asString();
            if (s.empty()) return Value(false);
            for (char c : s) {
                if (!isalpha(static_cast<unsigned char>(c))) return Value(false);
            }
            return Value(true);
        }));

    // strutil.isDigit(str) — true if all characters are digits
    module->entries["isDigit"] = Value(makeNative("strutil.isDigit", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("strutil.isDigit() requires a string", 0);
            auto& s = args[0].asString();
            if (s.empty()) return Value(false);
            for (char c : s) {
                if (!isdigit(static_cast<unsigned char>(c))) return Value(false);
            }
            return Value(true);
        }));

    // strutil.parseBase(str, base) — parse integer in given base (2-36)
    module->entries["parseBase"] = Value(makeNative("strutil.parseBase", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isInt())
                throw RuntimeError("strutil.parseBase() requires (string, int)", 0);
            auto& s = args[0].asString();
            int base = static_cast<int>(args[1].asInt());
            if (base < 2 || base > 36)
                throw RuntimeError("strutil.parseBase(): base must be 2-36", 0);
            char* end;
            errno = 0;
            long long val = strtoll(s.c_str(), &end, base);
            if (end == s.c_str() || *end != '\0' || errno == ERANGE)
                throw RuntimeError("strutil.parseBase(): invalid number '" + s + "'", 0);
            return Value(static_cast<int64_t>(val));
        }));

    // strutil.toBase(num, base) — format integer in given base
    module->entries["toBase"] = Value(makeNative("strutil.toBase", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isInt() || !args[1].isInt())
                throw RuntimeError("strutil.toBase() requires (int, int)", 0);
            int64_t num = args[0].asInt();
            int base = static_cast<int>(args[1].asInt());
            if (base < 2 || base > 36)
                throw RuntimeError("strutil.toBase(): base must be 2-36", 0);
            const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
            bool negative = num < 0;
            uint64_t n = negative ? static_cast<uint64_t>(-num) : static_cast<uint64_t>(num);
            std::string result;
            do {
                result += digits[n % base];
                n /= base;
            } while (n > 0);
            if (negative) result += '-';
            std::reverse(result.begin(), result.end());
            return Value(std::move(result));
        }));
}
