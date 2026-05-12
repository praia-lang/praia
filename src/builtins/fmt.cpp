#include "../builtins.h"
#include "../gc_heap.h"
#include "../unicode.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

// Go-style formatter. The supported surface:
//
//   verbs:   d  b  o  x  X      integer (base 10/2/8/16, X uppercase hex)
//            f  F  e  E  g  G   float (fixed / scientific / shortest)
//            s  q                string (raw / Go-quoted)
//            t                   bool
//            c                   integer codepoint → UTF-8 character
//            v                   default formatting (Praia toString)
//            T                   value's type name (mirrors type() builtin)
//            %%                  literal '%'
//
//   flags:   -                   left-align inside width
//            +                   force sign on positive numbers
//            ' '                 leading space for positive (sign placeholder)
//            0                   zero-pad numeric verbs (ignored with '-')
//            #                   alternate form (0b / 0o / 0x prefix for %b/%o/%x)
//
//   width:        leading-decimal-int (or 0 for unspecified)
//   precision:    `.<int>` after width (truncates strings, sets decimal
//                 digits for floats, ignored for integers other than %s)
//
// Width counting for %s/%q/%v/%T uses grapheme clusters, consistent with
// Praia's len() and indexing. Numeric verbs only emit ASCII so it doesn't
// matter for them.

namespace {

class FmtError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct FmtSpec {
    bool minus = false;
    bool plus  = false;
    bool space = false;
    bool zero  = false;
    bool hash  = false;
    int width  = -1;
    int precision = -1;
    char verb  = 0;
};

// Advance past flags, optional width, optional `.precision`, and the
// verb. Returns the index just past the verb. Throws on a truncated
// format like "%5.2" with no verb at the end.
size_t parseSpec(const std::string& fmt, size_t start, FmtSpec& spec) {
    size_t i = start;
    while (i < fmt.size()) {
        char c = fmt[i];
        if      (c == '-') spec.minus = true;
        else if (c == '+') spec.plus  = true;
        else if (c == ' ') spec.space = true;
        else if (c == '0') spec.zero  = true;
        else if (c == '#') spec.hash  = true;
        else break;
        ++i;
    }
    if (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
        spec.width = 0;
        while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
            spec.width = spec.width * 10 + (fmt[i] - '0');
            ++i;
        }
    }
    if (i < fmt.size() && fmt[i] == '.') {
        ++i;
        spec.precision = 0;
        while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
            spec.precision = spec.precision * 10 + (fmt[i] - '0');
            ++i;
        }
    }
    if (i >= fmt.size())
        throw FmtError("incomplete format specifier (expected a verb after '%')");
    spec.verb = fmt[i];
    return i + 1;
}

#ifdef HAVE_UTF8PROC
size_t displayLen(const std::string& s) { return utf8_grapheme_count(s); }
#else
size_t displayLen(const std::string& s) { return s.size(); }
#endif

// Right- or left-pad `body` to `spec.width` graphemes with spaces.
// (Zero-padding for numeric verbs is handled where the body is built,
// so we don't intermix sign/prefix and zeros incorrectly.)
std::string applyPadding(const std::string& body, const FmtSpec& spec) {
    if (spec.width < 0) return body;
    size_t cells = displayLen(body);
    if (static_cast<int>(cells) >= spec.width) return body;
    int pad = spec.width - static_cast<int>(cells);
    if (spec.minus) return body + std::string(pad, ' ');
    return std::string(pad, ' ') + body;
}

// Truncate a string to `n` grapheme clusters (consistent with how
// Praia counts string length elsewhere). Used by %.<n>s.
std::string truncGraphemes(const std::string& s, int n) {
    if (n < 0) return s;
#ifdef HAVE_UTF8PROC
    auto gs = utf8_graphemes(s);
    if (static_cast<int>(gs.size()) <= n) return s;
    std::string out;
    for (int i = 0; i < n; ++i) out += gs[i];
    return out;
#else
    if (static_cast<int>(s.size()) <= n) return s;
    return s.substr(0, n);
#endif
}

// %d / %b / %o / %x / %X — integer in the given base. Handles sign,
// alternate-form prefix (0b/0o/0x), and width/zero-pad together so
// the sign and prefix never get separated from their digits.
std::string formatInt(int64_t n, int base, bool upperHex, const FmtSpec& spec) {
    // Special-case the most negative int64 so negating doesn't overflow.
    bool negative = n < 0;
    uint64_t mag = negative ? -static_cast<uint64_t>(n) : static_cast<uint64_t>(n);
    static const char* lc = "0123456789abcdef";
    static const char* uc = "0123456789ABCDEF";
    const char* alpha = upperHex ? uc : lc;
    std::string digits;
    if (mag == 0) digits = "0";
    else {
        while (mag > 0) {
            digits.insert(digits.begin(), alpha[mag % base]);
            mag /= base;
        }
    }
    std::string prefix;
    if (negative)         prefix = "-";
    else if (spec.plus)   prefix = "+";
    else if (spec.space)  prefix = " ";
    if (spec.hash) {
        if      (base == 2)  prefix += "0b";
        else if (base == 8)  prefix += "0o";
        else if (base == 16) prefix += upperHex ? "0X" : "0x";
    }
    // Zero-padding fills between prefix and digits, not before the sign.
    if (spec.zero && !spec.minus && spec.width > 0) {
        int target = spec.width - static_cast<int>(prefix.size());
        if (target > static_cast<int>(digits.size()))
            digits = std::string(target - digits.size(), '0') + digits;
    }
    std::string out = prefix + digits;
    // Only space-pad if zero-pad didn't already fill the width.
    if (!spec.zero || spec.minus) out = applyPadding(out, spec);
    return out;
}

// %f / %F / %e / %E / %g / %G — float. NaN/Inf get explicit names;
// finite values go through snprintf with a reconstructed format spec
// so we don't reinvent the floating-point pretty-printer.
std::string formatFloat(double v, char verb, const FmtSpec& spec) {
    int prec = spec.precision < 0 ? 6 : spec.precision;
    if (std::isnan(v)) {
        return applyPadding(std::string("NaN"), spec);
    }
    if (std::isinf(v)) {
        std::string body = v < 0 ? "-Inf" : (spec.plus ? "+Inf" : (spec.space ? " Inf" : "Inf"));
        return applyPadding(body, spec);
    }
    // Build a clean format spec from the parsed flags. We never pass
    // the user's format string to printf directly — only a synthesized
    // one with exactly the flags we accepted.
    char fmtBuf[24];
    int p = 0;
    fmtBuf[p++] = '%';
    if (spec.plus)       fmtBuf[p++] = '+';
    else if (spec.space) fmtBuf[p++] = ' ';
    fmtBuf[p++] = '.';
    p += std::snprintf(fmtBuf + p, sizeof(fmtBuf) - p, "%d", prec);
    fmtBuf[p++] = verb;
    fmtBuf[p]   = '\0';

    // Grow buffer until it fits — most floats fit in 64 bytes, but a
    // huge precision can blow past that.
    std::string body(64, '\0');
    int n = std::snprintf(body.data(), body.size(), fmtBuf, v);
    if (n < 0) throw FmtError("snprintf failed for float verb");
    if (static_cast<size_t>(n) >= body.size()) {
        body.resize(static_cast<size_t>(n) + 1);
        std::snprintf(body.data(), body.size(), fmtBuf, v);
    }
    body.resize(static_cast<size_t>(n));

    // Zero-pad after the sign (mirrors how Go does %010.2f of -3.5 = "-000003.50").
    if (spec.zero && !spec.minus && spec.width > static_cast<int>(body.size())) {
        size_t signLen = 0;
        if (!body.empty() && (body[0] == '-' || body[0] == '+' || body[0] == ' '))
            signLen = 1;
        int pad = spec.width - static_cast<int>(body.size());
        body = body.substr(0, signLen) + std::string(pad, '0') + body.substr(signLen);
        return body;
    }
    return applyPadding(body, spec);
}

// %q — wrap the string in double quotes with Go-style escapes for
// the control characters, backslash, and quote. Non-ASCII bytes pass
// through verbatim (assumed UTF-8); we don't \u-escape them, because
// %q exists to be human-readable and \u'd UTF-8 is anything but.
std::string formatQuoted(const std::string& v) {
    std::string out = "\"";
    for (size_t i = 0; i < v.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(v[i]);
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        }
        else out += static_cast<char>(c);
    }
    out += "\"";
    return out;
}

// Mirror the type() builtin's classification so %T and type() agree.
// Keep the names exactly in sync with src/interpreter_setup.cpp's
// type() registration — drift here is confusing for users who do
// `if (type(x) == fmt.sprintf("%T", x)) { ... }` style checks.
std::string typeName(const Value& v) {
    if (v.isNil())       return "nil";
    if (v.isBool())      return "bool";
    if (v.isInt())       return "int";
    if (v.isDouble())    return "float";
    if (v.isString())    return "string";
    if (v.isArray())     return "array";
    if (v.isMap())       return "map";
    if (v.isInstance())  return "instance";
    if (v.isTagged())    return "tagged";
    if (v.isFuture())    return "future";
    if (v.isGenerator()) return "generator";
    if (v.isCallable())  return "function";
    return "unknown";
}

// %d/%b/%o/%x/%X accept ints, and also accept doubles that happen
// to hold an integer value in range — Praia does some int↔number
// promotion at the language level, so being strict here would surprise
// callers who never thought about the distinction.
bool toInt64(const Value& v, int64_t& out) {
    if (v.isInt()) { out = v.asInt(); return true; }
    if (v.isDouble()) {
        double d = v.asNumber();
        if (!std::isfinite(d)) return false;
        if (d != std::floor(d)) return false;
        if (d < -9.2233720368547758e18 || d > 9.2233720368547758e18) return false;
        out = static_cast<int64_t>(d);
        return true;
    }
    return false;
}

std::string formatString(const std::string& fmt, const std::vector<Value>& args) {
    std::string out;
    size_t argIdx = 0;
    auto consumeArg = [&](char verb) -> const Value& {
        if (argIdx >= args.size())
            throw FmtError(std::string("not enough arguments for verb %") + verb);
        return args[argIdx++];
    };

    size_t i = 0;
    while (i < fmt.size()) {
        char c = fmt[i];
        if (c != '%') { out += c; ++i; continue; }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            out += '%';
            i += 2;
            continue;
        }
        FmtSpec spec;
        i = parseSpec(fmt, i + 1, spec);
        switch (spec.verb) {
            case 'd': {
                const Value& v = consumeArg('d');
                int64_t n;
                if (!toInt64(v, n))
                    throw FmtError("verb %d requires an integer (got " + typeName(v) + ")");
                out += formatInt(n, 10, false, spec);
                break;
            }
            case 'b': case 'o': case 'x': case 'X': {
                const Value& v = consumeArg(spec.verb);
                int64_t n;
                if (!toInt64(v, n))
                    throw FmtError(std::string("verb %") + spec.verb +
                                   " requires an integer (got " + typeName(v) + ")");
                int base = (spec.verb == 'b') ? 2 : (spec.verb == 'o') ? 8 : 16;
                bool upper = (spec.verb == 'X');
                out += formatInt(n, base, upper, spec);
                break;
            }
            case 'f': case 'F':
            case 'e': case 'E':
            case 'g': case 'G': {
                const Value& v = consumeArg(spec.verb);
                if (!v.isNumber())
                    throw FmtError(std::string("verb %") + spec.verb +
                                   " requires a number (got " + typeName(v) + ")");
                out += formatFloat(v.asNumber(), spec.verb, spec);
                break;
            }
            case 's': {
                const Value& v = consumeArg('s');
                std::string s = v.toString();
                if (spec.precision >= 0) s = truncGraphemes(s, spec.precision);
                out += applyPadding(s, spec);
                break;
            }
            case 'q': {
                const Value& v = consumeArg('q');
                out += applyPadding(formatQuoted(v.toString()), spec);
                break;
            }
            case 't': {
                const Value& v = consumeArg('t');
                if (!v.isBool())
                    throw FmtError("verb %t requires a bool (got " + typeName(v) + ")");
                out += applyPadding(v.asBool() ? "true" : "false", spec);
                break;
            }
            case 'c': {
                const Value& v = consumeArg('c');
                int64_t cp;
                if (!toInt64(v, cp) || cp < 0 || cp > 0x10FFFF)
                    throw FmtError("verb %c requires a Unicode codepoint (0..0x10FFFF)");
                out += applyPadding(utf8_encode(static_cast<int32_t>(cp)), spec);
                break;
            }
            case 'v': {
                const Value& v = consumeArg('v');
                out += applyPadding(v.toString(), spec);
                break;
            }
            case 'T': {
                const Value& v = consumeArg('T');
                out += applyPadding(typeName(v), spec);
                break;
            }
            default:
                throw FmtError(std::string("unknown verb %") + spec.verb);
        }
    }
    if (argIdx < args.size())
        throw FmtError("too many arguments for format string (" +
                       std::to_string(args.size() - argIdx) + " unused)");
    return out;
}

}  // namespace

void registerFmtBuiltins(std::shared_ptr<PraiaMap> fmtMap) {
    // fmt.sprintf(format, args...) — Go-style formatter; returns the
    // formatted string. Throws on format/argument mismatch rather than
    // silently producing %!d(string=x)-style "fail open" output, since
    // Praia callers almost always want the loud-fail behavior at the
    // language level.
    fmtMap->entries[Value("sprintf")] = Value(makeNative("fmt.sprintf", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("fmt.sprintf(format, args...) requires a format string", 0);
            std::vector<Value> rest(args.begin() + 1, args.end());
            try {
                return Value(formatString(args[0].asString(), rest));
            } catch (const FmtError& e) {
                throw RuntimeError(std::string("fmt.sprintf: ") + e.what(), 0);
            }
        }));

    // fmt.printf — same formatter, writes to stdout with NO trailing
    // newline (just like Go's fmt.Printf and C's printf). Use \n in
    // the format string if you want one.
    fmtMap->entries[Value("printf")] = Value(makeNative("fmt.printf", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("fmt.printf(format, args...) requires a format string", 0);
            std::vector<Value> rest(args.begin() + 1, args.end());
            try {
                std::cout << formatString(args[0].asString(), rest);
            } catch (const FmtError& e) {
                throw RuntimeError(std::string("fmt.printf: ") + e.what(), 0);
            }
            return Value();
        }));

    // fmt.println(args...) — no format string. Each arg is stringified
    // (via Praia's toString) and joined with single spaces; a newline
    // is appended. Matches Go's fmt.Println exactly.
    fmtMap->entries[Value("println")] = Value(makeNative("fmt.println", -1,
        [](const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].toString();
            }
            std::cout << "\n";
            return Value();
        }));

    // fmt.errorf — same body as sprintf; named so that `throw
    // fmt.errorf("bad input: %v", x)` reads naturally at call sites.
    fmtMap->entries[Value("errorf")] = Value(makeNative("fmt.errorf", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("fmt.errorf(format, args...) requires a format string", 0);
            std::vector<Value> rest(args.begin() + 1, args.end());
            try {
                return Value(formatString(args[0].asString(), rest));
            } catch (const FmtError& e) {
                throw RuntimeError(std::string("fmt.errorf: ") + e.what(), 0);
            }
        }));
}
