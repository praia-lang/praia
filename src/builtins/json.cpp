#include "../builtins.h"
#include "../value.h"
#include <cctype>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include "../gc_heap.h"

namespace {

class JsonParser {
    const std::string& src;
    size_t pos = 0;
    int depth = 0;

    // Cap on object/array nesting to prevent stack overflow on hostile input
    // (e.g. `[[[[[[...` with thousands of brackets blows the C++ stack and
    // crashes the process). Matches conservative defaults used elsewhere
    // (Python json: ~1000, JSON Schema: 200). 200 is plenty for any
    // human-written or sensible machine-generated JSON.
    static constexpr int MAX_DEPTH = 200;

    // RAII helper: bumps depth on construction, decrements on destruction
    // (so exceptions still restore depth correctly). Throws if we'd exceed
    // MAX_DEPTH.
    struct DepthGuard {
        JsonParser& p;
        DepthGuard(JsonParser& parser) : p(parser) {
            if (p.depth >= MAX_DEPTH)
                p.fail("JSON nesting too deep (max " + std::to_string(MAX_DEPTH) + ")");
            p.depth++;
        }
        ~DepthGuard() { p.depth--; }
    };

    void skipWhitespace() { while (pos < src.size() && std::isspace(src[pos])) pos++; }
    char peek() { return pos < src.size() ? src[pos] : '\0'; }
    char advance() { return src[pos++]; }

    // "at position 42, near 'foo|bar'" where | marks the current position
    std::string context() const {
        std::string s = " at position " + std::to_string(pos) + ", near '";
        size_t start = pos > 10 ? pos - 10 : 0;
        size_t end = std::min(src.size(), pos + 10);
        for (size_t i = start; i < end; i++) {
            char c = src[i];
            if (i == pos) s += '|';
            if (c == '\n') s += "\\n";
            else if (c == '\t') s += "\\t";
            else s += c;
        }
        if (pos >= src.size()) s += "|";
        return s + "'";
    }

    [[noreturn]] void fail(const std::string& msg) const {
        throw RuntimeError(msg + context(), 0);
    }

    Value parseValue() {
        skipWhitespace();
        char c = peek();
        if (c == '"') return parseString();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        if (c == '-' || std::isdigit(c)) return parseNumber();
        if (c == '\0') fail("Unexpected end of JSON input");
        fail(std::string("Unexpected character '") + c + "' in JSON");
    }

    Value parseString() {
        advance(); // opening "
        std::string result;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\') {
                pos++;
                if (pos >= src.size()) fail("Unterminated escape in JSON string");
                switch (src[pos]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case 'r': result += '\r'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'u': {
                        // Helper: parse exactly 4 hex digits at pos+1..pos+4.
                        // On entry, pos points at the 'u'. On success, pos
                        // advances to the last hex digit (loop's pos++ then
                        // moves past it).
                        auto parseHex4 = [&](int& cp) {
                            if (pos + 4 >= src.size())
                                fail("Truncated unicode escape in JSON string");
                            cp = 0;
                            for (int i = 1; i <= 4; i++) {
                                char ch = src[pos + i];
                                int d;
                                if (ch >= '0' && ch <= '9') d = ch - '0';
                                else if (ch >= 'a' && ch <= 'f') d = 10 + (ch - 'a');
                                else if (ch >= 'A' && ch <= 'F') d = 10 + (ch - 'A');
                                else fail(std::string("Invalid hex digit '") + ch + "' in unicode escape");
                                cp = (cp << 4) | d;
                            }
                            pos += 4; // +1 from the loop increment below = 5 total past 'u'
                        };

                        int cp;
                        parseHex4(cp);

                        // Surrogate-pair handling: high surrogate must be
                        // followed by `\uXXXX` low surrogate; combine to a
                        // supplementary-plane codepoint.
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // Need to peek past the current loop position. The
                            // loop will pos++ before exiting this iteration, so
                            // the next char is at pos+1. Require "\u".
                            if (pos + 2 >= src.size() ||
                                src[pos + 1] != '\\' || src[pos + 2] != 'u') {
                                fail("Lone high surrogate in JSON string");
                            }
                            pos += 2; // skip "\u" (loop pos++ will then sit at last hex)
                            int low;
                            parseHex4(low);
                            if (low < 0xDC00 || low > 0xDFFF)
                                fail("Invalid low surrogate after high surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            fail("Lone low surrogate in JSON string");
                        }

                        // Encode codepoint as UTF-8.
                        if (cp < 0x80) {
                            result += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xF0 | (cp >> 18));
                            result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: fail(std::string("Invalid escape '\\") + src[pos] + "' in JSON string");
                }
            } else {
                result += src[pos];
            }
            pos++;
        }
        if (pos >= src.size()) fail("Unterminated JSON string");
        pos++; // closing "
        return Value(std::move(result));
    }

    Value parseNumber() {
        size_t start = pos;
        if (src[pos] == '-') pos++;

        // Integer part: no leading zeros (except standalone 0)
        if (pos >= src.size() || !std::isdigit(src[pos]))
            fail("Invalid JSON number");
        if (src[pos] == '0') {
            pos++;
            if (pos < src.size() && std::isdigit(src[pos]))
                fail("Leading zeros not allowed in JSON numbers");
        } else {
            while (pos < src.size() && std::isdigit(src[pos])) pos++;
        }

        // Fractional part
        if (pos < src.size() && src[pos] == '.') {
            pos++;
            if (pos >= src.size() || !std::isdigit(src[pos]))
                fail("Expected digit after '.' in JSON number");
            while (pos < src.size() && std::isdigit(src[pos])) pos++;
        }

        // Exponent part
        if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
            pos++;
            if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) pos++;
            if (pos >= src.size() || !std::isdigit(src[pos]))
                fail("Expected digit in JSON number exponent");
            while (pos < src.size() && std::isdigit(src[pos])) pos++;
        }

        std::string numStr = src.substr(start, pos - start);
        // Integer if no decimal point or exponent
        if (numStr.find('.') == std::string::npos &&
            numStr.find('e') == std::string::npos &&
            numStr.find('E') == std::string::npos) {
            try { return Value(static_cast<int64_t>(std::stoll(numStr))); }
            catch (...) {} // fallback to double for very large integers
        }
        try {
            return Value(std::stod(numStr));
        } catch (const std::out_of_range&) {
            fail("Number out of range: " + numStr);
        }
        return Value(0.0); // unreachable, fail() throws
    }

    Value parseObject() {
        DepthGuard g(*this);
        advance(); // {
        auto map = gcNew<PraiaMap>();
        skipWhitespace();
        if (peek() == '}') { pos++; return Value(map); }
        while (true) {
            skipWhitespace();
            if (peek() != '"') fail("Expected string key in JSON object");
            Value key = parseString();
            skipWhitespace();
            if (peek() != ':') fail("Expected ':' after key in JSON object");
            advance();
            map->entries[Value(key.asString())] = parseValue();
            skipWhitespace();
            if (peek() == '}') { pos++; break; }
            if (peek() != ',') fail("Expected ',' or '}' in JSON object");
            advance();
        }
        return Value(map);
    }

    Value parseArray() {
        DepthGuard g(*this);
        advance(); // [
        auto arr = gcNew<PraiaArray>();
        skipWhitespace();
        if (peek() == ']') { pos++; return Value(arr); }
        while (true) {
            arr->elements.push_back(parseValue());
            skipWhitespace();
            if (peek() == ']') { pos++; break; }
            if (peek() != ',') fail("Expected ',' or ']' in JSON array");
            advance();
        }
        return Value(arr);
    }

    Value parseBool() {
        if (src.compare(pos, 4, "true") == 0) { pos += 4; return Value(true); }
        if (src.compare(pos, 5, "false") == 0) { pos += 5; return Value(false); }
        fail("Invalid JSON boolean (expected 'true' or 'false')");
    }

    Value parseNull() {
        if (src.compare(pos, 4, "null") == 0) { pos += 4; return Value(); }
        fail("Invalid JSON null (expected 'null')");
    }

public:
    JsonParser(const std::string& s) : src(s) {}
    Value parse() {
        Value v = parseValue();
        skipWhitespace();
        if (pos != src.size())
            fail("Unexpected content after JSON value");
        return v;
    }
};

} // namespace

Value jsonParse(const std::string& src) {
    JsonParser parser(src);
    return parser.parse();
}

static std::string jsonQuote(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\t': r += "\\t"; break;
            case '\r': r += "\\r"; break;
            default: r += c;
        }
    }
    return r + "\"";
}

// Recursive helper. `visited` holds container pointers on the active
// path; cycles throw rather than emit a placeholder (JSON can't
// represent them, matching what Python's json.dumps does).
static std::string jsonStringifyRec(const Value& val, int indent, int depth,
                                    std::unordered_set<const void*>& visited) {
    std::string pad(depth * indent, ' ');
    std::string childPad((depth + 1) * indent, ' ');
    bool pretty = indent > 0;
    std::string nl = pretty ? "\n" : "";

    if (val.isNil()) return "null";
    if (val.isBool()) return val.asBool() ? "true" : "false";
    if (val.isInt()) return std::to_string(val.asInt());
    if (val.isDouble()) {
        double d = val.asNumber();
        // JSON has no representation for NaN or ±Infinity. Reject rather
        // than emit `nan` / `inf` (which most parsers fail on) or `null`
        // (which silently swallows the value). Matches Python's
        // json.dumps(allow_nan=False) behavior and our cycle rejection.
        if (!std::isfinite(d)) {
            const char* what = std::isnan(d) ? "NaN" :
                               (d > 0 ? "Infinity" : "-Infinity");
            throw RuntimeError(std::string("json.stringify: cannot represent ") +
                               what + " in JSON", 0);
        }
        std::ostringstream o;
        o << std::setprecision(17) << d;
        return o.str();
    }
    if (val.isString()) {
        return jsonQuote(val.asString());
    }
    if (val.isArray()) {
        const void* key = static_cast<const void*>(val.asArray().get());
        if (!visited.insert(key).second)
            throw RuntimeError("json.stringify: cyclic reference", 0);
        auto& elems = val.asArray()->elements;
        if (elems.empty()) { visited.erase(key); return "[]"; }
        std::string r = "[" + nl;
        for (size_t i = 0; i < elems.size(); i++) {
            if (i > 0) r += "," + nl;
            r += childPad + jsonStringifyRec(elems[i], indent, depth + 1, visited);
        }
        visited.erase(key);
        return r + nl + pad + "]";
    }
    if (val.isMap()) {
        const void* key = static_cast<const void*>(val.asMap().get());
        if (!visited.insert(key).second)
            throw RuntimeError("json.stringify: cyclic reference", 0);
        auto& entries = val.asMap()->entries;
        if (entries.empty()) { visited.erase(key); return "{}"; }
        std::string r = "{" + nl;
        bool first = true;
        for (auto& [k, v] : entries) {
            if (!k.isString())
                throw RuntimeError("JSON keys must be strings", 0);
            if (!first) r += "," + nl;
            first = false;
            r += childPad + jsonQuote(k.asString()) + ":" + (pretty ? " " : "");
            r += jsonStringifyRec(v, indent, depth + 1, visited);
        }
        visited.erase(key);
        return r + nl + pad + "}";
    }
    return "null"; // functions, futures, instances → null
}

std::string jsonStringify(const Value& val, int indent, int depth) {
    std::unordered_set<const void*> visited;
    return jsonStringifyRec(val, indent, depth, visited);
}
