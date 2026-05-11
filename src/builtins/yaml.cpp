#include "../builtins.h"
#include "../value.h"
#include <cctype>
#include <memory>
#include <sstream>
#include <string>
#include "../gc_heap.h"

namespace {

class YamlParser {
    const std::string& src;
    size_t pos = 0;
    int depth = 0;

    // Cap on mapping/sequence nesting — same defense as JSON's parser.
    // Deeply-nested YAML (an attacker-controllable shape) would otherwise
    // blow the C++ stack via mutual recursion between parseValue,
    // parseSequence, and parseMapping. 200 is plenty for any reasonable
    // human- or machine-authored YAML.
    static constexpr int MAX_DEPTH = 200;

    // RAII helper: bumps depth on construction, decrements on destruction
    // so exceptions still restore depth correctly. Throws when exceeding
    // MAX_DEPTH.
    struct DepthGuard {
        YamlParser& p;
        DepthGuard(YamlParser& parser) : p(parser) {
            if (p.depth >= MAX_DEPTH)
                throw RuntimeError("YAML nesting too deep (max " +
                                   std::to_string(MAX_DEPTH) + ")", 0);
            p.depth++;
        }
        ~DepthGuard() { p.depth--; }
    };

    char peek() { return pos < src.size() ? src[pos] : '\0'; }
    char advance() { return pos < src.size() ? src[pos++] : '\0'; }
    bool atEnd() { return pos >= src.size(); }

    void skipSpaces() { while (pos < src.size() && src[pos] == ' ') pos++; }

    void skipComment() {
        if (pos < src.size() && src[pos] == '#')
            while (pos < src.size() && src[pos] != '\n') pos++;
    }

    void skipBlankLines() {
        while (pos < src.size()) {
            size_t lineStart = pos;
            skipSpaces();
            skipComment();
            if (pos < src.size() && src[pos] == '\n') { pos++; continue; }
            pos = lineStart;
            break;
        }
    }

    int currentIndent() {
        size_t saved = pos;
        int indent = 0;
        while (pos < src.size() && src[pos] == ' ') { indent++; pos++; }
        pos = saved;
        return indent;
    }

    std::string readLine() {
        std::string line;
        while (pos < src.size() && src[pos] != '\n') line += src[pos++];
        if (pos < src.size()) pos++; // consume \n
        // Strip trailing comment (skip escaped quotes and quoted regions)
        size_t comment = std::string::npos;
        bool inQuote = false;
        char quoteChar = 0;
        for (size_t i = 0; i < line.size(); i++) {
            if (inQuote) {
                if (line[i] == '\\' && quoteChar == '"') { i++; continue; } // skip escaped char
                if (line[i] == quoteChar) inQuote = false;
            } else {
                if (line[i] == '"' || line[i] == '\'') { inQuote = true; quoteChar = line[i]; }
                else if (line[i] == '#') { comment = i; break; }
            }
        }
        if (comment != std::string::npos) line = line.substr(0, comment);
        // Strip trailing whitespace
        while (!line.empty() && std::isspace(line.back())) line.pop_back();
        return line;
    }

    Value parseScalar(const std::string& s) {
        if (s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL") return Value();
        if (s == "true" || s == "True" || s == "TRUE") return Value(true);
        if (s == "false" || s == "False" || s == "FALSE") return Value(false);
        // Single-quoted string — literal, no escapes
        if (s.front() == '\'' && s.back() == '\'')
            return Value(s.substr(1, s.size() - 2));
        // Double-quoted string — process escape sequences
        if (s.front() == '"' && s.back() == '"') {
            std::string raw = s.substr(1, s.size() - 2);
            std::string out;
            for (size_t i = 0; i < raw.size(); i++) {
                if (raw[i] == '\\' && i + 1 < raw.size()) {
                    switch (raw[++i]) {
                        case 'n':  out += '\n'; break;
                        case 't':  out += '\t'; break;
                        case 'r':  out += '\r'; break;
                        case '\\': out += '\\'; break;
                        case '"':  out += '"';  break;
                        case '0':  out += '\0'; break;
                        case 'a':  out += '\a'; break;
                        case 'b':  out += '\b'; break;
                        default:   out += '\\'; out += raw[i]; break;
                    }
                } else {
                    out += raw[i];
                }
            }
            return Value(out);
        }
        // Try number
        try {
            size_t p = 0;
            double d = std::stod(s, &p);
            if (p == s.size()) return Value(d);
        } catch (...) {}
        return Value(s); // plain string
    }

    Value parseValue(int minIndent) {
        DepthGuard g(*this);
        skipBlankLines();
        if (atEnd()) return Value();

        int indent = currentIndent();
        if (indent < minIndent) return Value();

        // Check if it's a sequence (- item)
        if (pos + indent < src.size() && src[pos + indent] == '-' &&
            (pos + indent + 1 >= src.size() || src[pos + indent + 1] == ' ' || src[pos + indent + 1] == '\n')) {
            return parseSequence(indent);
        }

        // Check if it's a mapping (key: value)
        size_t saved = pos;
        skipSpaces();
        std::string line = readLine();
        auto colonPos = line.find(": ");
        if (colonPos == std::string::npos && !line.empty() && line.back() == ':')
            colonPos = line.size() - 1;

        if (colonPos != std::string::npos) {
            pos = saved;
            return parseMapping(indent);
        }

        // It's a scalar
        return parseScalar(line);
    }

    Value parseSequence(int indent) {
        // No DepthGuard — parseSequence is always reached from parseValue,
        // which has already incremented depth for this nesting level.
        // Iteration within the sequence is a loop, not recursion.
        auto arr = gcNew<PraiaArray>();
        while (!atEnd()) {
            skipBlankLines();
            if (atEnd()) break;
            int curIndent = currentIndent();
            if (curIndent != indent) break;
            if (src[pos + curIndent] != '-') break;

            pos += curIndent + 1; // skip spaces + '-'
            if (pos < src.size() && src[pos] == ' ') pos++; // skip space after -

            // Check if inline value or nested block
            skipBlankLines();
            if (atEnd() || src[pos] == '\n') {
                pos++;
                arr->elements.push_back(parseValue(indent + 2));
            } else {
                int valIndent = currentIndent();
                // Inline: read rest of line, could be a scalar or start of nested mapping
                size_t saved = pos;
                std::string rest = readLine();
                auto c = rest.find(": ");
                if (c == std::string::npos && !rest.empty() && rest.back() == ':')
                    c = rest.size() - 1;

                if (c != std::string::npos) {
                    // Inline mapping start — reparse
                    pos = saved;
                    arr->elements.push_back(parseMapping(valIndent >= 0 ? valIndent : indent + 2));
                } else {
                    arr->elements.push_back(parseScalar(rest));
                }
            }
        }
        return Value(arr);
    }

    Value parseMapping(int indent) {
        // No DepthGuard — same reasoning as parseSequence above. The
        // direct parseSequence → parseMapping path (inline mapping inside
        // a sequence item) doesn't increment depth, but the next nesting
        // level inevitably reaches parseValue which does.
        auto map = gcNew<PraiaMap>();
        while (!atEnd()) {
            skipBlankLines();
            if (atEnd()) break;
            int curIndent = currentIndent();
            if (curIndent != indent) break;

            skipSpaces();
            std::string line = readLine();
            auto colonPos = line.find(": ");
            bool colonAtEnd = false;
            if (colonPos == std::string::npos && !line.empty() && line.back() == ':') {
                colonPos = line.size() - 1;
                colonAtEnd = true;
            }
            if (colonPos == std::string::npos) break;

            std::string key = line.substr(0, colonPos);
            // Strip quotes from key
            if ((key.front() == '"' && key.back() == '"') ||
                (key.front() == '\'' && key.back() == '\''))
                key = key.substr(1, key.size() - 2);

            if (colonAtEnd || colonPos + 2 >= line.size()) {
                // Value is on next lines
                map->entries[Value(key)] = parseValue(indent + 1);
            } else {
                std::string val = line.substr(colonPos + 2);
                // Check for inline flow sequence [a, b]
                if (!val.empty() && val.front() == '[' && val.back() == ']') {
                    auto arr = gcNew<PraiaArray>();
                    std::string inner = val.substr(1, val.size() - 2);
                    std::istringstream ss(inner);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        while (!item.empty() && item.front() == ' ') item.erase(0, 1);
                        while (!item.empty() && item.back() == ' ') item.pop_back();
                        arr->elements.push_back(parseScalar(item));
                    }
                    map->entries[Value(key)] = Value(arr);
                } else {
                    map->entries[Value(key)] = parseScalar(val);
                }
            }
        }
        return Value(map);
    }

public:
    YamlParser(const std::string& s) : src(s) {}
    Value parse() { return parseValue(0); }
};

} // namespace

Value yamlParse(const std::string& src) {
    YamlParser parser(src);
    return parser.parse();
}

std::string yamlStringify(const Value& val, int depth) {
    std::string pad(depth * 2, ' ');

    if (val.isNil()) return "null";
    if (val.isBool()) return val.asBool() ? "true" : "false";
    if (val.isNumber()) { std::ostringstream o; o << val.asNumber(); return o.str(); }
    if (val.isString()) {
        auto& s = val.asString();
        bool needsQuote = s.empty() || s.find(": ") != std::string::npos ||
                          s.find('#') != std::string::npos || s.find('\n') != std::string::npos ||
                          s == "true" || s == "false" || s == "null";
        if (needsQuote) {
            std::string r = "\"";
            for (char c : s) {
                if (c == '"') r += "\\\"";
                else if (c == '\\') r += "\\\\";
                else if (c == '\n') r += "\\n";
                else r += c;
            }
            return r + "\"";
        }
        return s;
    }
    if (val.isArray()) {
        auto& elems = val.asArray()->elements;
        if (elems.empty()) return "[]";
        std::string r;
        for (size_t i = 0; i < elems.size(); i++) {
            if (i > 0 || depth > 0) r += pad;
            r += "- ";
            if (elems[i].isMap() || elems[i].isArray()) {
                r += "\n" + yamlStringify(elems[i], depth + 1);
            } else {
                r += yamlStringify(elems[i], depth + 1) + "\n";
            }
        }
        return r;
    }
    if (val.isMap()) {
        auto& entries = val.asMap()->entries;
        if (entries.empty()) return "{}";

        // Quote YAML keys that contain special characters
        auto yamlQuoteKey = [](const std::string& key) -> std::string {
            if (key.empty()) return "\"\"";
            bool needsQuote = false;
            for (char c : key) {
                if (c == ':' || c == '#' || c == '{' || c == '}' ||
                    c == '[' || c == ']' || c == ',' || c == '&' ||
                    c == '*' || c == '?' || c == '|' || c == '>' ||
                    c == '\'' || c == '"' || c == '\\' || c == '\n' ||
                    c == '\r' || c == '\t') {
                    needsQuote = true;
                    break;
                }
            }
            if (!needsQuote && (key.front() == ' ' || key.back() == ' ' ||
                                key.front() == '-' || key.front() == '!'))
                needsQuote = true;
            if (!needsQuote) return key;
            // Double-quote with escapes
            std::string r = "\"";
            for (char c : key) {
                if (c == '"') r += "\\\"";
                else if (c == '\\') r += "\\\\";
                else if (c == '\n') r += "\\n";
                else if (c == '\t') r += "\\t";
                else if (c == '\r') r += "\\r";
                else r += c;
            }
            return r + "\"";
        };

        std::string r;
        for (auto& [k, v] : entries) {
            if (!k.isString())
                throw RuntimeError("YAML keys must be strings", 0);
            if (depth > 0) r += pad;
            r += yamlQuoteKey(k.asString()) + ":";
            if (v.isMap() || v.isArray()) {
                r += "\n" + yamlStringify(v, depth + 1);
            } else {
                r += " " + yamlStringify(v, depth + 1) + "\n";
            }
        }
        return r;
    }
    return "null";
}
