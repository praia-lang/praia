#include "../builtins.h"
#include "../unicode.h"
#include "../value.h"
#include "../gc_heap.h"
#include <cctype>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>

// XML parser + serializer for the subset most callers actually need:
//
//   - Elements with attributes, mixed content, self-closing tags
//   - Five standard entities (&lt; &gt; &amp; &quot; &apos;)
//   - Numeric character references (&#65; and &#x41;)
//   - CDATA sections preserved verbatim
//   - Comments / processing instructions / DOCTYPE skipped on parse
//
// Out of scope (would need libxml2 or significant additional code):
//
//   - Namespace resolution (xmlns:foo is parsed as a plain attribute;
//     the element/attribute "name" field carries the qualified name
//     verbatim, e.g. "foo:bar")
//   - Custom entity declarations / DTDs
//   - XPath, XSLT, XQuery
//   - Schema validation
//
// Data model: an element is a Praia map
//   {tag: "div", attrs: {class: "box"}, children: [...]}
// where `children` is an array of either nested element maps or
// plain strings (text nodes). Comments and PIs are dropped, so
// roundtrip is faithful for "data" XML (config files, RSS, plist)
// but lossy for documents that depend on them.

namespace {

constexpr int MAX_DEPTH = 200;

class XmlParser {
public:
    XmlParser(const std::string& s) : src_(s) {}

    Value parse() {
        skipProlog();
        skipWhitespace();
        if (pos_ >= src_.size())
            fail("empty document (no root element)");
        Value root = parseElement();
        // Skip trailing comments / PIs / whitespace; if anything else
        // follows the root, that's a malformed multi-root document.
        while (pos_ < src_.size()) {
            skipWhitespace();
            if (pos_ + 4 <= src_.size() && src_.compare(pos_, 4, "<!--") == 0) skipComment();
            else if (pos_ + 2 <= src_.size() && src_.compare(pos_, 2, "<?") == 0) skipPI();
            else if (pos_ < src_.size())
                fail("unexpected content after root element");
            else break;
        }
        return root;
    }

private:
    const std::string& src_;
    size_t pos_ = 0;
    int depth_ = 0;

    [[noreturn]] void fail(const std::string& msg) {
        // Show ~20 bytes of context around the failure to make the
        // error message actionable. Without it, "expected '>'" on a
        // 50KB doc is useless.
        size_t start = pos_ > 20 ? pos_ - 20 : 0;
        size_t end = std::min(src_.size(), pos_ + 20);
        std::string ctx;
        for (size_t i = start; i < end; ++i) {
            char c = src_[i];
            if (i == pos_) ctx += '|';
            if (c == '\n') ctx += "\\n";
            else if (c == '\t') ctx += "\\t";
            else ctx += c;
        }
        throw RuntimeError("xml.parse: " + msg + " at byte " +
                           std::to_string(pos_) + ", near '" + ctx + "'", 0);
    }

    char peek(size_t off = 0) const {
        return pos_ + off < src_.size() ? src_[pos_ + off] : '\0';
    }

    bool startsWith(const char* s) const {
        size_t n = std::strlen(s);
        return pos_ + n <= src_.size() && src_.compare(pos_, n, s) == 0;
    }

    void skipWhitespace() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    // Prolog: optional BOM + XML declaration + optional DOCTYPE +
    // comments / PIs. We skip but otherwise ignore all of these —
    // the parser doesn't try to honor encoding/version declarations
    // (everything must already be UTF-8) or DTD-defined entities.
    void skipProlog() {
        // UTF-8 BOM.
        if (src_.size() >= 3 &&
            (unsigned char)src_[0] == 0xEF &&
            (unsigned char)src_[1] == 0xBB &&
            (unsigned char)src_[2] == 0xBF) {
            pos_ = 3;
        }
        while (true) {
            skipWhitespace();
            if (startsWith("<?")) skipPI();
            else if (startsWith("<!--")) skipComment();
            else if (startsWith("<!DOCTYPE")) skipDoctype();
            else break;
        }
    }

    void skipComment() {
        // <!-- ... -->
        pos_ += 4;  // skip "<!--"
        while (pos_ + 3 <= src_.size()) {
            if (src_.compare(pos_, 3, "-->") == 0) {
                pos_ += 3;
                return;
            }
            ++pos_;
        }
        fail("unterminated comment");
    }

    void skipPI() {
        // <? ... ?>  — covers the XML declaration and any other PIs.
        pos_ += 2;
        while (pos_ + 2 <= src_.size()) {
            if (src_.compare(pos_, 2, "?>") == 0) {
                pos_ += 2;
                return;
            }
            ++pos_;
        }
        fail("unterminated processing instruction");
    }

    void skipDoctype() {
        // Skip "<!DOCTYPE ... >" — we ignore the internal subset
        // entirely. Track '[' depth so DTD internal subsets with
        // nested '>' inside don't trip us up.
        pos_ += 9;  // "<!DOCTYPE"
        int subsetDepth = 0;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '[') ++subsetDepth;
            else if (c == ']') --subsetDepth;
            else if (c == '>' && subsetDepth <= 0) return;
        }
        fail("unterminated DOCTYPE");
    }

    bool isNameStartChar(char c) {
        // XML 1.0 §2.3 simplified to ASCII (the full table includes
        // big Unicode ranges; we accept ASCII letters + ':' + '_').
        // Non-ASCII bytes (UTF-8 lead bytes) are accepted as a
        // pragmatic concession; we don't enforce the full Name
        // production. Malformed Unicode names will be silently
        // accepted as long as they don't contain whitespace / '<' /
        // '>' / '=' / '/' / '"' / "'".
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               c == '_' || c == ':' || (static_cast<unsigned char>(c) >= 0x80);
    }
    bool isNameChar(char c) {
        return isNameStartChar(c) ||
               (c >= '0' && c <= '9') ||
               c == '-' || c == '.';
    }

    std::string parseName() {
        if (pos_ >= src_.size() || !isNameStartChar(src_[pos_]))
            fail("expected element or attribute name");
        size_t start = pos_++;
        while (pos_ < src_.size() && isNameChar(src_[pos_])) ++pos_;
        return src_.substr(start, pos_ - start);
    }

    // Parse an attribute value. Supports both '...' and "..." quoting.
    // Entity references are expanded.
    std::string parseAttrValue() {
        if (pos_ >= src_.size() || (src_[pos_] != '"' && src_[pos_] != '\''))
            fail("attribute values must be quoted");
        char quote = src_[pos_++];
        std::string out;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == quote) { ++pos_; return out; }
            if (c == '<') fail("'<' is not allowed in attribute values");
            if (c == '&') { out += decodeEntity(); continue; }
            out += c;
            ++pos_;
        }
        fail("unterminated attribute value");
    }

    // Resolve an entity reference starting at the '&'. Advances pos_
    // past the closing ';'. Handles the 5 standard entities + numeric
    // character references; custom entity declarations from a DTD are
    // not supported.
    std::string decodeEntity() {
        size_t start = pos_;
        if (peek() != '&') fail("expected '&'");
        ++pos_;
        if (peek() == '#') {
            // Numeric character reference.
            ++pos_;
            int32_t cp = 0;
            if (peek() == 'x' || peek() == 'X') {
                ++pos_;
                if (!std::isxdigit(static_cast<unsigned char>(peek())))
                    fail("hex character reference must contain at least one digit");
                while (std::isxdigit(static_cast<unsigned char>(peek()))) {
                    char h = src_[pos_++];
                    int d = (h >= '0' && h <= '9') ? h - '0'
                          : (h >= 'a' && h <= 'f') ? 10 + (h - 'a')
                          :                          10 + (h - 'A');
                    cp = (cp << 4) | d;
                    if (cp > 0x10FFFF) fail("numeric character reference out of range");
                }
            } else {
                if (!std::isdigit(static_cast<unsigned char>(peek())))
                    fail("character reference must contain at least one digit");
                while (std::isdigit(static_cast<unsigned char>(peek()))) {
                    cp = cp * 10 + (src_[pos_++] - '0');
                    if (cp > 0x10FFFF) fail("numeric character reference out of range");
                }
            }
            if (peek() != ';') fail("character reference must end with ';'");
            ++pos_;
            // Reject XML-illegal codepoints (C0 controls except tab/LF/CR;
            // surrogate halves). XML 1.0 §2.2.
            if ((cp < 0x20 && cp != '\t' && cp != '\n' && cp != '\r') ||
                (cp >= 0xD800 && cp <= 0xDFFF)) {
                pos_ = start;
                fail("character reference resolves to a forbidden codepoint");
            }
            return utf8_encode(cp);
        }
        // Named entity — only the 5 builtins.
        size_t nameStart = pos_;
        while (pos_ < src_.size() && src_[pos_] != ';') ++pos_;
        if (pos_ >= src_.size())
            fail("unterminated entity reference");
        std::string name = src_.substr(nameStart, pos_ - nameStart);
        ++pos_;  // skip ';'
        if (name == "lt")   return "<";
        if (name == "gt")   return ">";
        if (name == "amp")  return "&";
        if (name == "quot") return "\"";
        if (name == "apos") return "'";
        pos_ = start;
        fail("unknown entity reference '&" + name + ";' (only the 5 standard ones and numeric refs are supported)");
    }

    Value parseElement() {
        if (depth_ >= MAX_DEPTH)
            fail("XML nesting too deep (max " + std::to_string(MAX_DEPTH) + ")");
        ++depth_;

        if (peek() != '<') fail("expected element start");
        ++pos_;
        std::string tag = parseName();
        auto attrs = gcNew<PraiaMap>();

        // Attributes.
        while (true) {
            skipWhitespace();
            char c = peek();
            if (c == '>' || c == '/') break;
            std::string aname = parseName();
            skipWhitespace();
            if (peek() != '=') fail("expected '=' after attribute name");
            ++pos_;
            skipWhitespace();
            std::string aval = parseAttrValue();
            attrs->entries[Value(aname)] = Value(aval);
        }

        auto children = gcNew<PraiaArray>();

        if (peek() == '/') {
            // Self-closing element. No children.
            ++pos_;
            if (peek() != '>') fail("expected '>' after '/'");
            ++pos_;
            --depth_;
            auto m = gcNew<PraiaMap>();
            m->entries[Value("tag")]      = Value(tag);
            m->entries[Value("attrs")]    = Value(attrs);
            m->entries[Value("children")] = Value(children);
            return Value(m);
        }

        if (peek() != '>') fail("expected '>'");
        ++pos_;

        // Children: text, nested elements, CDATA, comments.
        while (true) {
            // Read text up to the next '<' or end-of-input.
            std::string text;
            while (pos_ < src_.size() && src_[pos_] != '<') {
                if (src_[pos_] == '&') text += decodeEntity();
                else                   text += src_[pos_++];
            }
            if (!text.empty()) children->elements.push_back(Value(text));

            if (pos_ >= src_.size())
                fail("unterminated element <" + tag + ">");

            if (startsWith("</")) {
                // Closing tag.
                pos_ += 2;
                std::string closeTag = parseName();
                if (closeTag != tag)
                    fail("mismatched closing tag </" + closeTag +
                         "> (expected </" + tag + ">)");
                skipWhitespace();
                if (peek() != '>') fail("expected '>' on closing tag");
                ++pos_;
                --depth_;
                auto m = gcNew<PraiaMap>();
                m->entries[Value("tag")]      = Value(tag);
                m->entries[Value("attrs")]    = Value(attrs);
                m->entries[Value("children")] = Value(children);
                return Value(m);
            }
            if (startsWith("<!--")) { skipComment(); continue; }
            if (startsWith("<![CDATA[")) {
                pos_ += 9;
                std::string cdata;
                while (pos_ + 3 <= src_.size()) {
                    if (src_.compare(pos_, 3, "]]>") == 0) {
                        pos_ += 3;
                        children->elements.push_back(Value(cdata));
                        goto next_iter;
                    }
                    cdata += src_[pos_++];
                }
                fail("unterminated CDATA section");
            }
            if (startsWith("<?")) { skipPI(); continue; }
            if (startsWith("<!")) {
                // <!ENTITY, <!DOCTYPE inside content — unusual, just skip
                // to the matching '>' to be forgiving.
                while (pos_ < src_.size() && src_[pos_] != '>') ++pos_;
                if (pos_ < src_.size()) ++pos_;
                continue;
            }
            // Nested element.
            children->elements.push_back(parseElement());

        next_iter:;
        }
    }
};

// Recursive helper for stringify. `visited` tracks container pointers
// for cycle detection (a malicious map could otherwise loop forever).
void xmlStringifyRec(std::string& out, const Value& node, int indent, int depth,
                     std::unordered_set<const void*>& visited);

void writeAttr(std::string& out, const std::string& v) {
    for (char c : v) {
        switch (c) {
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '&':  out += "&amp;"; break;
            case '"':  out += "&quot;"; break;
            // Attribute values are always emitted in double quotes,
            // so single quotes don't need escaping.
            default:   out += c;
        }
    }
}

void writeText(std::string& out, const std::string& v) {
    for (char c : v) {
        switch (c) {
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '&':  out += "&amp;"; break;
            default:   out += c;
        }
    }
}

void xmlStringifyRec(std::string& out, const Value& node, int indent, int depth,
                     std::unordered_set<const void*>& visited) {
    // Strings → text node. Used for mixed-content children.
    if (node.isString()) {
        writeText(out, node.asString());
        return;
    }
    if (!node.isMap())
        throw RuntimeError("xml.stringify: nodes must be element maps or string text nodes "
                           "(got " + node.toString() + ")", 0);

    auto map = node.asMap();
    const void* key = static_cast<const void*>(map.get());
    if (!visited.insert(key).second)
        throw RuntimeError("xml.stringify: cyclic element graph", 0);
    struct Pop {
        std::unordered_set<const void*>& set;
        const void* key;
        ~Pop() { set.erase(key); }
    } pop{visited, key};

    auto& entries = map->entries;
    auto tagIt = entries.find(Value("tag"));
    if (tagIt == entries.end() || !tagIt->second.isString())
        throw RuntimeError("xml.stringify: element map must have a 'tag' string field", 0);
    const std::string& tag = tagIt->second.asString();

    std::string pad(indent * depth, ' ');
    out += pad;
    out += '<';
    out += tag;

    auto attrsIt = entries.find(Value("attrs"));
    if (attrsIt != entries.end() && attrsIt->second.isMap()) {
        // Praia maps are unordered (std::unordered_map), so attribute
        // emission order isn't guaranteed across parse → stringify.
        // The XML spec allows reordering attributes anyway, so this
        // doesn't violate well-formedness — just don't rely on it for
        // diff-friendly output.
        for (auto& [k, v] : attrsIt->second.asMap()->entries) {
            if (!k.isString())
                throw RuntimeError("xml.stringify: attribute keys must be strings", 0);
            out += ' ';
            out += k.asString();
            out += "=\"";
            writeAttr(out, v.isString() ? v.asString() : v.toString());
            out += '"';
        }
    }

    auto childIt = entries.find(Value("children"));
    bool hasChildren = childIt != entries.end() && childIt->second.isArray() &&
                       !childIt->second.asArray()->elements.empty();
    if (!hasChildren) {
        // Self-closing form: shorter on the wire, easier to read.
        out += "/>";
        return;
    }
    out += '>';

    auto& kids = childIt->second.asArray()->elements;
    // If indent > 0 and all children are elements (no text), put each
    // on its own line. If any child is a text node, keep the whole
    // thing inline to preserve mixed-content semantics.
    bool anyText = false;
    for (auto& c : kids) if (c.isString()) { anyText = true; break; }
    bool pretty = indent > 0 && !anyText;

    if (pretty) out += '\n';
    for (auto& c : kids) {
        if (pretty) {
            xmlStringifyRec(out, c, indent, depth + 1, visited);
            out += '\n';
        } else {
            xmlStringifyRec(out, c, 0, 0, visited);
        }
    }
    if (pretty) out += pad;

    out += "</";
    out += tag;
    out += '>';
}

}  // namespace

Value xmlParse(const std::string& src) {
    XmlParser p(src);
    return p.parse();
}

std::string xmlStringify(const Value& tree, int indent) {
    std::string out;
    std::unordered_set<const void*> visited;
    xmlStringifyRec(out, tree, indent, 0, visited);
    return out;
}

// Public helpers also exposed as xml.escape / xml.unescape.
std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '&':  out += "&amp;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
        }
    }
    return out;
}

std::string xmlUnescape(const std::string& s) {
    // Reuse the parser's entity decoder by faking a tiny element
    // context — simpler than duplicating the entity table.
    std::string out;
    size_t i = 0;
    auto fail = [&](const std::string& msg) {
        throw RuntimeError("xml.unescape: " + msg + " at byte " +
                           std::to_string(i), 0);
    };
    while (i < s.size()) {
        char c = s[i];
        if (c != '&') { out += c; ++i; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos)
            fail("unterminated entity reference");
        std::string body = s.substr(i + 1, semi - i - 1);
        if (!body.empty() && body[0] == '#') {
            int32_t cp = 0;
            if (body.size() >= 2 && (body[1] == 'x' || body[1] == 'X')) {
                for (size_t k = 2; k < body.size(); ++k) {
                    char h = body[k];
                    int d = (h >= '0' && h <= '9') ? h - '0'
                          : (h >= 'a' && h <= 'f') ? 10 + (h - 'a')
                          : (h >= 'A' && h <= 'F') ? 10 + (h - 'A') : -1;
                    if (d < 0) fail("invalid hex digit in &#x");
                    cp = (cp << 4) | d;
                    if (cp > 0x10FFFF) fail("character reference out of range");
                }
            } else {
                for (size_t k = 1; k < body.size(); ++k) {
                    char h = body[k];
                    if (h < '0' || h > '9') fail("invalid digit in &#");
                    cp = cp * 10 + (h - '0');
                    if (cp > 0x10FFFF) fail("character reference out of range");
                }
            }
            out += utf8_encode(cp);
        } else if (body == "lt")   out += '<';
        else  if (body == "gt")   out += '>';
        else  if (body == "amp")  out += '&';
        else  if (body == "quot") out += '"';
        else  if (body == "apos") out += '\'';
        else fail("unknown entity '&" + body + ";'");
        i = semi + 1;
    }
    return out;
}

void registerXmlBuiltins(std::shared_ptr<PraiaMap> xmlMap) {
    xmlMap->entries[Value("parse")] = Value(makeNative("xml.parse", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("xml.parse() requires a string", 0);
            return xmlParse(args[0].asString());
        }));

    xmlMap->entries[Value("stringify")] = Value(makeNative("xml.stringify", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty())
                throw RuntimeError("xml.stringify() requires an element tree", 0);
            int indent = 0;
            if (args.size() > 1 && args[1].isNumber())
                indent = static_cast<int>(args[1].toInt64ForBitwise());
            if (indent < 0) indent = 0;
            return Value(xmlStringify(args[0], indent));
        }));

    xmlMap->entries[Value("escape")] = Value(makeNative("xml.escape", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("xml.escape() requires a string", 0);
            return Value(xmlEscape(args[0].asString()));
        }));

    xmlMap->entries[Value("unescape")] = Value(makeNative("xml.unescape", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("xml.unescape() requires a string", 0);
            return Value(xmlUnescape(args[0].asString()));
        }));
}
