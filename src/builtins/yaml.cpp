#include "../builtins.h"
#include "../value.h"
#include <cctype>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
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

    // Column of `pos` in its line — i.e. the count of characters from the
    // previous '\n' (or start-of-input) to `pos`. Unlike `currentIndent()`
    // this works from any position on the line, not just the start. The
    // inline-mapping case inside a sequence item (`- key: value`) needs
    // this: after we've consumed `  - `, the next key starts at the column
    // where `key` lives, and sibling keys on subsequent lines must align
    // there. Pre-fix, the inline mapping was parsed with indent=0, so
    // anything indented (which is everything) failed the indent check
    // and the mapping ended after one entry.
    int colAtPos() const {
        int col = 0;
        size_t i = pos;
        while (i > 0 && src[i - 1] != '\n') { --i; ++col; }
        return col;
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
            // Skip any whitespace after the dash. YAML allows multiple
            // spaces (`-     foo`) and aligns the value's column at the
            // first non-space.
            while (pos < src.size() && src[pos] == ' ') pos++;

            // Check if inline value or nested block
            skipBlankLines();
            if (atEnd() || src[pos] == '\n') {
                pos++;
                arr->elements.push_back(parseValue(indent + 2));
            } else {
                // Inline: read rest of line, could be a scalar or start of nested mapping
                size_t saved = pos;
                int valCol = colAtPos();
                std::string rest = readLine();
                auto c = rest.find(": ");
                if (c == std::string::npos && !rest.empty() && rest.back() == ':')
                    c = rest.size() - 1;

                if (c != std::string::npos) {
                    // Inline mapping start — reparse the first key (and any
                    // sibling keys on the following lines that align at
                    // valCol, the column of the first key's name).
                    pos = saved;
                    arr->elements.push_back(parseMapping(valCol));
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
        // The inline-mapping-in-sequence path enters here mid-line, with
        // pos sitting at the first key character — no leading whitespace
        // to count, so currentIndent() returns 0 even though we're at
        // the intended column. Skip the indent check on the first
        // iteration; subsequent iterations always start at the head of
        // a line and check normally.
        bool first = true;
        while (!atEnd()) {
            skipBlankLines();
            if (atEnd()) break;
            int curIndent = currentIndent();
            if (!first && curIndent != indent) break;
            first = false;

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
                // Block scalar indicator. `|` = literal (keep newlines),
                // `>` = folded (single \n → ' ', \n\n → \n). Only the
                // bare indicator is recognized — chomping indicators
                // (`|-`, `|+`, `>-`, `>+`) and explicit indentation
                // indicators (`|2`, `|+3`, etc.) are silently ignored
                // along with any other suffix, and the result uses
                // default ("clip") chomping (single trailing newline).
                // Without this branch, `run: |` and its multi-line body
                // parsed as the literal string "|" and the body was
                // attributed to the parent — silently losing whole
                // script blocks in CI workflows.
                bool isBlock = !val.empty() && (val[0] == '|' || val[0] == '>');
                if (isBlock) {
                    char style = val[0];
                    map->entries[Value(key)] = parseBlockScalar(indent, style);
                } else if (!val.empty() && val.front() == '[' && val.back() == ']') {
                    // Inline flow sequence [a, b]
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

    // Read a block scalar — the text following a `|` or `>` indicator.
    // `parentIndent` is the indent of the line that carried the indicator;
    // body lines must be more indented than that. The *content* indent is
    // taken from the first non-blank body line, and that many leading
    // characters are stripped from every line (blank lines included).
    Value parseBlockScalar(int parentIndent, char style) {
        std::string out;
        int contentIndent = -1;
        // parseMapping's readLine() has already consumed through the
        // newline that terminated `key: |`, so pos is at the start of
        // the first body line — no extra newline-eating to do here.

        while (!atEnd()) {
            size_t lineStart = pos;
            // Measure the leading spaces without consuming them.
            int lead = 0;
            size_t scan = pos;
            while (scan < src.size() && src[scan] == ' ') { scan++; lead++; }
            bool blank = (scan >= src.size() || src[scan] == '\n');

            if (!blank) {
                if (contentIndent < 0) {
                    if (lead <= parentIndent) { pos = lineStart; break; }
                    contentIndent = lead;
                } else if (lead < contentIndent) {
                    pos = lineStart; break;
                }
            }

            // Skip `contentIndent` (or `lead` if shorter) leading spaces,
            // then read the remainder of the line.
            int skip = (contentIndent < 0) ? lead
                     : (blank ? std::min(lead, contentIndent) : contentIndent);
            pos = lineStart + skip;
            std::string body;
            while (pos < src.size() && src[pos] != '\n') body += src[pos++];
            if (pos < src.size()) pos++; // consume \n

            if (style == '|') {
                out += body;
                out += '\n';
            } else {
                // Folded: blank → newline, otherwise join consecutive
                // non-blank lines with spaces. We don't have direct
                // access to "previous line was blank?", so we encode
                // intermediate newlines and fix up at the end.
                out += body;
                out += '\n';
            }
        }

        if (style == '>') {
            // Convert single \n → ' ', double \n → single \n.
            std::string folded;
            for (size_t i = 0; i < out.size(); i++) {
                if (out[i] == '\n') {
                    // Count consecutive newlines.
                    int run = 1;
                    while (i + 1 < out.size() && out[i+1] == '\n') { run++; i++; }
                    if (i + 1 >= out.size()) {
                        // Trailing newlines — preserve one.
                        folded += '\n';
                    } else {
                        if (run == 1) folded += ' ';
                        else for (int k = 0; k < run - 1; k++) folded += '\n';
                    }
                } else folded += out[i];
            }
            out = folded;
        }

        // Default chomping (clip): exactly one trailing \n if the block
        // is non-empty. Strip extras.
        while (out.size() >= 2 && out[out.size()-1] == '\n' && out[out.size()-2] == '\n')
            out.pop_back();
        return Value(out);
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

// Recursive helper. `visited` holds container pointers on the active
// path; cycles throw rather than emit a placeholder (YAML's `&anchor`
// syntax could in principle represent cycles, but our stringifier
// doesn't emit anchors, so the only safe answer is to refuse).
static std::string yamlStringifyRec(const Value& val, int depth,
                                    std::unordered_set<const void*>& visited) {
    std::string pad(depth * 2, ' ');

    if (val.isNil()) return "null";
    if (val.isBool()) return val.asBool() ? "true" : "false";
    if (val.isNumber()) { std::ostringstream o; o << val.asNumber(); return o.str(); }
    if (val.isString()) {
        auto& s = val.asString();
        // Leading/trailing whitespace (including a string consisting
        // entirely of whitespace) must be quoted — YAML's plain-scalar
        // parser strips surrounding whitespace, so a value like " " or
        // "  hi  " would lose its bookends on re-read.
        bool boundaryWS = !s.empty() &&
            (s.front() == ' ' || s.front() == '\t' ||
             s.back()  == ' ' || s.back()  == '\t');
        bool needsQuote = s.empty() || s.find(": ") != std::string::npos ||
                          s.find('#') != std::string::npos || s.find('\n') != std::string::npos ||
                          s == "true" || s == "false" || s == "null" ||
                          boundaryWS;
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
        const void* key = static_cast<const void*>(val.asArray().get());
        if (!visited.insert(key).second)
            throw RuntimeError("yaml.stringify: cyclic reference", 0);
        auto& elems = val.asArray()->elements;
        if (elems.empty()) { visited.erase(key); return "[]"; }
        std::string r;
        for (size_t i = 0; i < elems.size(); i++) {
            if (i > 0 || depth > 0) r += pad;
            r += "- ";
            if (elems[i].isMap() || elems[i].isArray()) {
                r += "\n" + yamlStringifyRec(elems[i], depth + 1, visited);
            } else {
                r += yamlStringifyRec(elems[i], depth + 1, visited) + "\n";
            }
        }
        visited.erase(key);
        return r;
    }
    if (val.isMap()) {
        const void* key = static_cast<const void*>(val.asMap().get());
        if (!visited.insert(key).second)
            throw RuntimeError("yaml.stringify: cyclic reference", 0);
        auto& entries = val.asMap()->entries;
        if (entries.empty()) { visited.erase(key); return "{}"; }

        // Quote YAML keys that contain special characters
        auto yamlQuoteKey = [](const std::string& k) -> std::string {
            if (k.empty()) return "\"\"";
            bool needsQuote = false;
            for (char c : k) {
                if (c == ':' || c == '#' || c == '{' || c == '}' ||
                    c == '[' || c == ']' || c == ',' || c == '&' ||
                    c == '*' || c == '?' || c == '|' || c == '>' ||
                    c == '\'' || c == '"' || c == '\\' || c == '\n' ||
                    c == '\r' || c == '\t') {
                    needsQuote = true;
                    break;
                }
            }
            if (!needsQuote && (k.front() == ' ' || k.back() == ' ' ||
                                k.front() == '-' || k.front() == '!'))
                needsQuote = true;
            if (!needsQuote) return k;
            // Double-quote with escapes
            std::string r = "\"";
            for (char c : k) {
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
                r += "\n" + yamlStringifyRec(v, depth + 1, visited);
            } else {
                r += " " + yamlStringifyRec(v, depth + 1, visited) + "\n";
            }
        }
        visited.erase(key);
        return r;
    }
    return "null";
}

std::string yamlStringify(const Value& val, int depth) {
    std::unordered_set<const void*> visited;
    return yamlStringifyRec(val, depth, visited);
}
