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

// ── Streaming JSON parser ──────────────────────────────────────
//
// Pull-parser style: each .next() call advances the state machine
// and returns one event map describing the token just consumed.
// Callers drive the loop themselves (no native generator backing),
// which keeps the implementation engine-agnostic and lets them stop
// early without paying for the rest of the document.
//
// Input is either a string (in-memory, no refilling) or a Praia
// file handle (with a .read(n) native method — typically the value
// returned by fs.open). For the handle path we top up an internal
// buffer in 16 KiB chunks via the handle's read() so files larger
// than RAM stream through one chunk at a time.
//
// NDJSON is supported transparently: after a complete top-level
// value, the parser resets `expect` back to "value" and accepts
// the next record. The .nextValue() helper consumes one whole top-
// level value per call, which is the common loop body for
// newline-delimited streams.

class JsonStreamParser {
public:
    enum class EventType {
        ObjectStart, ObjectEnd,
        ArrayStart,  ArrayEnd,
        Key, String,
        Number, Bool, Null,
        Eof
    };

    struct Event {
        EventType type;
        std::string strVal;
        int64_t  intVal  = 0;
        double   numVal  = 0;
        bool     isInt   = false;
        bool     boolVal = false;

        // Tag-only constructor for events with no payload (objectStart,
        // arrayEnd, eof, null). Silences -Wmissing-field-initializers
        // and reads cleaner than `Event{type, {}, 0, 0, false, false}`.
        explicit Event(EventType t) : type(t) {}
        Event(EventType t, std::string s) : type(t), strVal(std::move(s)) {}
        Event(EventType t, std::string s, int64_t iv, double nv, bool ii, bool bv)
            : type(t), strVal(std::move(s)),
              intVal(iv), numVal(nv), isInt(ii), boolVal(bv) {}
    };

    // Source: either a raw string or a callable that returns the
    // next chunk of bytes when invoked with (size_t maxBytes). The
    // callable receives a Praia Value (int) for the max bytes and
    // returns a Value(string).
    JsonStreamParser(std::string sourceString)
        : buf_(std::move(sourceString)), sourceExhausted_(true) {}

    JsonStreamParser(std::shared_ptr<NativeFunction> readFn, Value handleVal)
        : readFn_(std::move(readFn)),
          handleVal_(std::move(handleVal)) {}

    bool eof() {
        skipWhitespace();
        return pos_ >= buf_.size() && sourceExhausted_;
    }

    // Pull the next event. Returns an Event with type=Eof once the
    // stream is exhausted; subsequent calls keep returning Eof.
    Event next() {
        skipWhitespace();
        if (!ensure(1)) return Event{EventType::Eof};

        char c = buf_[pos_];

        // Container closers: only valid when we expect either
        // CommaOrEnd (after a value) or KeyOrEnd (immediately after '{').
        if (c == '}' || c == ']') {
            if (stack_.empty())
                fail("unexpected '" + std::string(1, c) + "' at top level");
            char open = stack_.back();
            if ((c == '}' && open != '{') || (c == ']' && open != '['))
                fail("mismatched closer '" + std::string(1, c) +
                     "' (expected closer for '" + std::string(1, open) + "')");
            // Reject trailing commas: `[1,]` ends in ValueAfterComma,
            // `{"k":1,}` ends in KeyAfterComma — both illegal per
            // RFC 8259. Colon / ValueAfterKey would mean we have a
            // dangling "key:" with nothing after.
            if (expect_ == Expect::Colon || expect_ == Expect::ValueAfterKey ||
                expect_ == Expect::ValueAfterComma ||
                expect_ == Expect::KeyAfterComma)
                fail("unexpected '" + std::string(1, c) + "' (expected a value)");
            pos_++;
            stack_.pop_back();
            popExpectAfterValue();
            return Event{c == '}' ? EventType::ObjectEnd : EventType::ArrayEnd};
        }

        // Separators eaten silently — we never expose them as events.
        if (c == ',') {
            if (expect_ != Expect::CommaOrEnd)
                fail("unexpected ','");
            pos_++;
            if (!stack_.empty() && stack_.back() == '{') expect_ = Expect::KeyAfterComma;
            else expect_ = Expect::ValueAfterComma;
            return next();
        }
        if (c == ':') {
            if (expect_ != Expect::Colon)
                fail("unexpected ':'");
            pos_++;
            expect_ = Expect::ValueAfterKey;
            return next();
        }

        // A string here is either a key or a value depending on state.
        if (c == '"') {
            bool isKey = (expect_ == Expect::Key || expect_ == Expect::KeyAfterComma);
            std::string s = parseStringBody();
            if (isKey) {
                expect_ = Expect::Colon;
                return Event{EventType::Key, std::move(s)};
            }
            // String value.
            requireValueAllowed();
            popExpectAfterValue();
            return Event{EventType::String, std::move(s)};
        }

        // Object / array openers — must be in a value-allowed position.
        if (c == '{') {
            requireValueAllowed();
            // Match the non-streaming parser's depth cap (200); blocks
            // the `[[[[[[...` DoS without limiting any human-written
            // or sensible machine-generated JSON.
            if (stack_.size() >= 200)
                fail("JSON nesting too deep (max 200)");
            pos_++;
            stack_.push_back('{');
            // Empty object handled by the closer branch above next call.
            expect_ = Expect::Key;
            // But '{' itself was consumed; mark "value just produced" so a
            // following ',' is illegal until we close the open object. The
            // CommaOrEnd is set by popExpectAfterValue once the closer is
            // reached.
            return Event{EventType::ObjectStart};
        }
        if (c == '[') {
            requireValueAllowed();
            if (stack_.size() >= 200)
                fail("JSON nesting too deep (max 200)");
            pos_++;
            stack_.push_back('[');
            expect_ = Expect::Value;
            return Event{EventType::ArrayStart};
        }

        // Literals: true / false / null.
        if (c == 't' || c == 'f' || c == 'n') {
            return parseLiteral();
        }

        // Number: -? digit ... (with optional frac and exp).
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parseNumber();
        }

        fail(std::string("unexpected character '") + c + "'");
    }

    // Materialize one whole top-level value from a stream of events.
    // Used by NDJSON loops where each record is a full value.
    // Returns Value() (nil) at EOF; callers can disambiguate from a
    // literal null by calling eof() before nextValue().
    Value nextValue() {
        Event ev = next();
        if (ev.type == EventType::Eof) return Value();
        return buildValueFromEvent(ev);
    }

    void close() { /* nothing to release; handle stays caller-owned */ }

private:
    // Where the parser thinks it is inside the current container.
    // Top-level is treated as a virtual "outer array" that accepts
    // any number of values separated by whitespace (which is what
    // NDJSON is in practice).
    enum class Expect {
        Value,              // very top-level, or after '['
        ValueAfterComma,    // after ',' inside an array
        ValueAfterKey,      // after ':' in an object
        Key,                // after '{' (or KeyAfterComma)
        KeyAfterComma,      // after ',' inside an object
        Colon,              // after a key
        CommaOrEnd          // after a value in any container
    };

    // Source state. For a string source, sourceExhausted_ is true
    // from the start and refill() is a no-op. For a handle source,
    // refill() pulls chunks until the source returns "".
    std::shared_ptr<NativeFunction> readFn_;
    Value handleVal_;
    std::string buf_;
    size_t pos_ = 0;
    bool sourceExhausted_ = false;

    std::vector<char> stack_;
    Expect expect_ = Expect::Value;

    [[noreturn]] void fail(const std::string& msg) {
        throw RuntimeError("json.parser: " + msg + " at byte " +
                           std::to_string(pos_), 0);
    }

    void requireValueAllowed() {
        if (expect_ == Expect::Key || expect_ == Expect::KeyAfterComma ||
            expect_ == Expect::Colon || expect_ == Expect::CommaOrEnd)
            fail("unexpected value (expected " +
                 std::string(expect_ == Expect::Colon ? "':'" :
                             expect_ == Expect::CommaOrEnd ? "',' or closer" :
                             "a key string") + ")");
    }

    // After a complete value, the next slot depends on the
    // surrounding container:
    //   inside any container: expect ',' or closer
    //   at the top level:     accept another value (NDJSON), or EOF
    void popExpectAfterValue() {
        if (stack_.empty()) expect_ = Expect::Value;
        else expect_ = Expect::CommaOrEnd;
    }

    // Ensure at least `n` bytes are buffered beyond pos_. Returns
    // false only when the source is exhausted and we still can't
    // satisfy the request (callers treat that as EOF).
    bool ensure(size_t n) {
        while (pos_ + n > buf_.size() && !sourceExhausted_) refill();
        return pos_ + n <= buf_.size();
    }

    void refill() {
        if (sourceExhausted_) return;
        if (!readFn_) { sourceExhausted_ = true; return; }
        constexpr int CHUNK = 16384;
        std::vector<Value> args{Value(static_cast<int64_t>(CHUNK))};
        Value got = readFn_->fn(args);
        if (!got.isString()) {
            sourceExhausted_ = true;
            return;
        }
        const auto& s = got.asString();
        if (s.empty()) {
            sourceExhausted_ = true;
            return;
        }
        // Compact the buffer if we've consumed most of it, otherwise
        // appending forever grows it linearly with the file size.
        if (pos_ > buf_.size() / 2) {
            buf_.erase(0, pos_);
            pos_ = 0;
        }
        buf_.append(s);
    }

    void skipWhitespace() {
        while (true) {
            if (!ensure(1)) return;
            char c = buf_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { pos_++; continue; }
            return;
        }
    }

    // Parse a JSON string body. Pos points at the opening '"' on
    // entry; on return pos is past the closing '"'.
    std::string parseStringBody() {
        if (!ensure(1) || buf_[pos_] != '"') fail("expected '\"'");
        pos_++;  // opening quote
        std::string out;
        while (true) {
            if (!ensure(1)) fail("unterminated string");
            unsigned char c = static_cast<unsigned char>(buf_[pos_++]);
            if (c == '"') return out;
            if (c == '\\') {
                if (!ensure(1)) fail("dangling backslash in string");
                char esc = buf_[pos_++];
                switch (esc) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (!ensure(4)) fail("truncated \\u escape");
                        int32_t cp = parseHex4();
                        // Surrogate pair handling: a high surrogate must
                        // be followed by \uXXXX with a low surrogate.
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (!ensure(6) || buf_[pos_] != '\\' || buf_[pos_+1] != 'u')
                                fail("high surrogate \\u" +
                                     std::to_string(cp) + " not followed by \\u low surrogate");
                            pos_ += 2;
                            int32_t low = parseHex4();
                            if (low < 0xDC00 || low > 0xDFFF)
                                fail("invalid low surrogate after high surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            fail("lone low surrogate \\u" + std::to_string(cp));
                        }
                        // Emit as UTF-8.
                        if (cp < 0x80) {
                            out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xF0 | (cp >> 18));
                            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default:
                        fail(std::string("invalid escape \\") + esc);
                }
            } else if (c < 0x20) {
                fail("unescaped control character in string");
            } else {
                out += static_cast<char>(c);
            }
        }
    }

    int32_t parseHex4() {
        int32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            char h = buf_[pos_++];
            int d;
            if      (h >= '0' && h <= '9') d = h - '0';
            else if (h >= 'a' && h <= 'f') d = 10 + (h - 'a');
            else if (h >= 'A' && h <= 'F') d = 10 + (h - 'A');
            else fail(std::string("invalid hex digit in \\u escape: '") + h + "'");
            cp = (cp << 4) | d;
        }
        return cp;
    }

    Event parseLiteral() {
        // The current byte is 't', 'f', or 'n'. Validate position
        // (literals are only legal where a value is expected) BEFORE
        // consuming, so a bad-context error leaves pos_ pointing at
        // the offending char for the error message.
        requireValueAllowed();
        char first = buf_[pos_];
        const char* expected;
        size_t len;
        if      (first == 't') { expected = "true";  len = 4; }
        else if (first == 'f') { expected = "false"; len = 5; }
        else                   { expected = "null";  len = 4; }
        if (!ensure(len) || buf_.compare(pos_, len, expected) != 0)
            fail(std::string("invalid literal (expected ") + expected + ")");
        pos_ += len;
        popExpectAfterValue();
        if (first == 't') return Event(EventType::Bool, "", 0, 0, false, true);
        if (first == 'f') return Event(EventType::Bool, "", 0, 0, false, false);
        return Event{EventType::Null};
    }

    Event parseNumber() {
        // Validate position before consuming the first digit. Numbers
        // are only legal in value-allowed slots.
        requireValueAllowed();
        // Build up the textual form of the number, refilling as needed,
        // until we hit a non-numeric char. Then stoll/stod the captured
        // text — handing it to the standard library beats reimplementing
        // float parsing in a streaming context.
        std::string lexeme;
        bool isFloat = false;
        // Optional leading '-'.
        if (buf_[pos_] == '-') { lexeme += '-'; pos_++; }
        // Integer part: '0' alone or '1-9' followed by digits.
        if (!ensure(1)) fail("incomplete number");
        if (buf_[pos_] == '0') { lexeme += '0'; pos_++; }
        else if (buf_[pos_] >= '1' && buf_[pos_] <= '9') {
            while (ensure(1) && buf_[pos_] >= '0' && buf_[pos_] <= '9') {
                lexeme += buf_[pos_++];
            }
        }
        else fail("invalid number");
        // Optional fractional part.
        if (ensure(1) && buf_[pos_] == '.') {
            isFloat = true;
            lexeme += '.'; pos_++;
            bool sawDigit = false;
            while (ensure(1) && buf_[pos_] >= '0' && buf_[pos_] <= '9') {
                lexeme += buf_[pos_++]; sawDigit = true;
            }
            if (!sawDigit) fail("expected digits after '.'");
        }
        // Optional exponent.
        if (ensure(1) && (buf_[pos_] == 'e' || buf_[pos_] == 'E')) {
            isFloat = true;
            lexeme += buf_[pos_++];
            if (ensure(1) && (buf_[pos_] == '+' || buf_[pos_] == '-'))
                lexeme += buf_[pos_++];
            bool sawDigit = false;
            while (ensure(1) && buf_[pos_] >= '0' && buf_[pos_] <= '9') {
                lexeme += buf_[pos_++]; sawDigit = true;
            }
            if (!sawDigit) fail("expected exponent digits");
        }
        popExpectAfterValue();
        Event ev{EventType::Number};
        if (isFloat) {
            ev.numVal = std::stod(lexeme);
            ev.isInt = false;
        } else {
            try {
                ev.intVal = std::stoll(lexeme);
                ev.isInt = true;
            } catch (const std::out_of_range&) {
                // Integer too big for int64 — fall back to double.
                ev.numVal = std::stod(lexeme);
                ev.isInt = false;
            }
        }
        return ev;
    }

    // Recursive helper: given the current event, build up the full
    // Praia value, descending into nested containers via additional
    // .next() calls.
    Value buildValueFromEvent(const Event& ev) {
        switch (ev.type) {
            case EventType::Null:        return Value();
            case EventType::Bool:        return Value(ev.boolVal);
            case EventType::String:      return Value(ev.strVal);
            case EventType::Number:
                return ev.isInt ? Value(ev.intVal) : Value(ev.numVal);
            case EventType::ArrayStart: {
                auto arr = gcNew<PraiaArray>();
                while (true) {
                    Event inner = next();
                    if (inner.type == EventType::ArrayEnd) return Value(arr);
                    if (inner.type == EventType::Eof) fail("unexpected EOF inside array");
                    arr->elements.push_back(buildValueFromEvent(inner));
                }
            }
            case EventType::ObjectStart: {
                auto obj = gcNew<PraiaMap>();
                while (true) {
                    Event inner = next();
                    if (inner.type == EventType::ObjectEnd) return Value(obj);
                    if (inner.type != EventType::Key)
                        fail("expected key inside object");
                    Value v = buildValueFromEvent(next());
                    obj->entries[Value(inner.strVal)] = std::move(v);
                }
            }
            default:
                fail("unexpected event type inside value");
        }
    }
};

// Translate an Event into the {type, value} map Praia callers see.
static Value eventToMap(const JsonStreamParser::Event& ev) {
    auto m = gcNew<PraiaMap>();
    switch (ev.type) {
        case JsonStreamParser::EventType::ObjectStart:
            m->entries[Value("type")] = Value("objectStart"); break;
        case JsonStreamParser::EventType::ObjectEnd:
            m->entries[Value("type")] = Value("objectEnd"); break;
        case JsonStreamParser::EventType::ArrayStart:
            m->entries[Value("type")] = Value("arrayStart"); break;
        case JsonStreamParser::EventType::ArrayEnd:
            m->entries[Value("type")] = Value("arrayEnd"); break;
        case JsonStreamParser::EventType::Key:
            m->entries[Value("type")]  = Value("key");
            m->entries[Value("value")] = Value(ev.strVal);
            break;
        case JsonStreamParser::EventType::String:
            m->entries[Value("type")]  = Value("string");
            m->entries[Value("value")] = Value(ev.strVal);
            break;
        case JsonStreamParser::EventType::Number:
            m->entries[Value("type")]  = Value("number");
            m->entries[Value("value")] = ev.isInt ? Value(ev.intVal) : Value(ev.numVal);
            break;
        case JsonStreamParser::EventType::Bool:
            m->entries[Value("type")]  = Value("bool");
            m->entries[Value("value")] = Value(ev.boolVal);
            break;
        case JsonStreamParser::EventType::Null:
            m->entries[Value("type")]  = Value("null");
            m->entries[Value("value")] = Value();
            break;
        case JsonStreamParser::EventType::Eof:
            return Value();  // nil
    }
    return Value(m);
}

} // namespace

// json.parser(input) factory exposed publicly so interpreter_setup
// can wire it into the json namespace alongside parse/stringify.
Value jsonParserCreate(const Value& input) {
    std::shared_ptr<JsonStreamParser> p;
    if (input.isString()) {
        p = std::make_shared<JsonStreamParser>(input.asString());
    } else if (input.isMap()) {
        // Extract the .read method from a file-handle-shaped map.
        auto& entries = input.asMap()->entries;
        auto it = entries.find(Value("read"));
        if (it == entries.end() || !it->second.isCallable())
            throw RuntimeError("json.parser(): handle must have a .read(n) method", 0);
        auto callable = it->second.asCallable();
        auto native = std::dynamic_pointer_cast<NativeFunction>(callable);
        if (!native)
            throw RuntimeError("json.parser(): handle.read must be a native function", 0);
        p = std::make_shared<JsonStreamParser>(native, input);
    } else {
        throw RuntimeError("json.parser(input): input must be a string or a file handle", 0);
    }

    // Return a Praia map of methods that all close over the same
    // parser shared_ptr — same shared-mutable-state-via-shared_ptr
    // pattern as FileHandle in fs.open.
    auto handleMap = gcNew<PraiaMap>();

    handleMap->entries[Value("next")] = Value(makeNative("JsonParser.next", 0,
        [p](const std::vector<Value>&) -> Value {
            return eventToMap(p->next());
        }));

    handleMap->entries[Value("nextValue")] = Value(makeNative("JsonParser.nextValue", 0,
        [p](const std::vector<Value>&) -> Value {
            return p->nextValue();
        }));

    handleMap->entries[Value("eof")] = Value(makeNative("JsonParser.eof", 0,
        [p](const std::vector<Value>&) -> Value {
            return Value(p->eof());
        }));

    handleMap->entries[Value("close")] = Value(makeNative("JsonParser.close", 0,
        [p](const std::vector<Value>&) -> Value {
            p->close();
            return Value();
        }));

    return Value(handleMap);
}

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
