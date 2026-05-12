#include "../builtins.h"
#include "../value.h"
#include "../gc_heap.h"
#include <cctype>
#include <cstdint>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_set>

// Apple-flavored XML property lists. plist is just XML with a fixed
// schema, so we layer it on top of xml.parse rather than re-implementing
// the parser. The supported tag set is the modern (XML 1.0) variant:
//
//   <plist version="1.0"> root wrapper, contains one child
//   <dict> map; alternating <key>name</key> + value
//   <array> array of values
//   <string>...</string>     → Praia string
//   <integer>123</integer>   → Praia int
//   <real>3.14</real>        → Praia float
//   <true/> / <false/>       → Praia bool
//   <data>BASE64</data>      → Praia bytes (decoded base64 in a string)
//   <date>2024-01-15T...</date> → Praia string (ISO 8601, NOT parsed
//                                  into a time value — Praia time is
//                                  Unix seconds; conversion can be
//                                  done at the call site if needed)
//
// Binary plists (the bplist00 magic format) are NOT supported — they're
// a different on-disk encoding that would warrant its own parser. The
// CLI tool `plutil -convert xml1` converts binary plists to XML on
// macOS if you have an arbitrary plist on hand.

namespace {

std::string base64DecodeIgnoreWs(const std::string& input) {
    auto decodeChar = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        int d = decodeChar(c);
        if (d < 0)
            throw RuntimeError("plist.parse: <data> contains non-base64 byte 0x" +
                               std::to_string(static_cast<int>(c)), 0);
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out += static_cast<char>((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return out;
}

// Note: plist.stringify doesn't emit <data> blocks. Praia strings
// double as byte sequences (no distinct bytes type), so we have no
// way to tell at stringify time whether a string is text or binary.
// On read we decode <data> into a Praia string; on write we always
// emit <string>. Callers who specifically need <data> on output
// can build the XML by hand via xml.stringify.

// Extract the single child element from another element, skipping
// whitespace-only text nodes. Throws if there's not exactly one child.
const Value& singleChildElement(const Value& el, const char* parentTag) {
    if (!el.isMap()) throw RuntimeError("plist.parse: not an element", 0);
    auto kids = el.asMap()->entries.find(Value("children"));
    if (kids == el.asMap()->entries.end() || !kids->second.isArray())
        throw RuntimeError("plist.parse: <" + std::string(parentTag) +
                           "> has no children array", 0);
    const Value* found = nullptr;
    for (auto& c : kids->second.asArray()->elements) {
        if (c.isString()) {
            // Whitespace-only text nodes are ignored; anything else
            // is a structural error.
            bool allWs = true;
            for (char ch : c.asString()) {
                if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') { allWs = false; break; }
            }
            if (!allWs)
                throw RuntimeError("plist.parse: <" + std::string(parentTag) +
                                   "> contains stray text", 0);
            continue;
        }
        if (found != nullptr)
            throw RuntimeError("plist.parse: <" + std::string(parentTag) +
                               "> must contain exactly one element", 0);
        found = &c;
    }
    if (!found)
        throw RuntimeError("plist.parse: <" + std::string(parentTag) +
                           "> is empty", 0);
    return *found;
}

// Concatenate all text-node children, ignoring nested elements. Used
// for <string>, <integer>, <real>, <date>, <data> bodies which are
// expected to be plain text.
std::string concatTextChildren(const Value& el) {
    auto kids = el.asMap()->entries.find(Value("children"));
    if (kids == el.asMap()->entries.end() || !kids->second.isArray()) return "";
    std::string out;
    for (auto& c : kids->second.asArray()->elements) {
        if (c.isString()) out += c.asString();
        // Nested elements inside a scalar body are unusual; we ignore
        // them (could be a malformed file). A strict parser would
        // throw — open an issue if anyone hits this.
    }
    return out;
}

std::string elementTag(const Value& el) {
    if (!el.isMap()) return "";
    auto it = el.asMap()->entries.find(Value("tag"));
    if (it == el.asMap()->entries.end() || !it->second.isString()) return "";
    return it->second.asString();
}

Value convertNode(const Value& el);

// Skip whitespace-only text nodes but reject substantive stray text.
// Returns true if `arr[i]` was a whitespace text node (caller should
// advance and retry); false if it's a real element.
bool skipWhitespaceText(const std::vector<Value>& arr, size_t i,
                        const char* parentTag) {
    if (!arr[i].isString()) return false;
    for (char c : arr[i].asString())
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            throw RuntimeError("plist.parse: <" + std::string(parentTag) +
                               "> contains stray text \"" + arr[i].asString() + "\"", 0);
    return true;
}

Value convertDict(const Value& el) {
    auto out = gcNew<PraiaMap>();
    auto kids = el.asMap()->entries.find(Value("children"));
    if (kids == el.asMap()->entries.end() || !kids->second.isArray())
        return Value(out);
    auto& arr = kids->second.asArray()->elements;
    size_t i = 0;
    while (i < arr.size()) {
        if (skipWhitespaceText(arr, i, "dict")) { ++i; continue; }
        if (elementTag(arr[i]) != "key")
            throw RuntimeError("plist.parse: <dict> expects alternating <key>/<value> (got <" +
                               elementTag(arr[i]) + ">)", 0);
        std::string keyName = concatTextChildren(arr[i]);
        ++i;
        while (i < arr.size() && skipWhitespaceText(arr, i, "dict")) ++i;
        if (i >= arr.size())
            throw RuntimeError("plist.parse: <key>" + keyName + "</key> has no following value", 0);
        out->entries[Value(keyName)] = convertNode(arr[i]);
        ++i;
    }
    return Value(out);
}

Value convertArray(const Value& el) {
    auto out = gcNew<PraiaArray>();
    auto kids = el.asMap()->entries.find(Value("children"));
    if (kids == el.asMap()->entries.end() || !kids->second.isArray())
        return Value(out);
    auto& arr = kids->second.asArray()->elements;
    for (size_t i = 0; i < arr.size(); ++i) {
        if (skipWhitespaceText(arr, i, "array")) continue;
        out->elements.push_back(convertNode(arr[i]));
    }
    return Value(out);
}

Value convertNode(const Value& el) {
    std::string tag = elementTag(el);
    if (tag == "dict")    return convertDict(el);
    if (tag == "array")   return convertArray(el);
    if (tag == "string")  return Value(concatTextChildren(el));
    if (tag == "true")    return Value(true);
    if (tag == "false")   return Value(false);
    if (tag == "integer") {
        std::string body = concatTextChildren(el);
        // Trim whitespace.
        size_t a = body.find_first_not_of(" \t\n\r");
        size_t b = body.find_last_not_of(" \t\n\r");
        if (a == std::string::npos)
            throw RuntimeError("plist.parse: <integer> is empty", 0);
        body = body.substr(a, b - a + 1);
        try {
            return Value(static_cast<int64_t>(std::stoll(body)));
        } catch (...) {
            throw RuntimeError("plist.parse: invalid <integer>: \"" + body + "\"", 0);
        }
    }
    if (tag == "real") {
        std::string body = concatTextChildren(el);
        size_t a = body.find_first_not_of(" \t\n\r");
        size_t b = body.find_last_not_of(" \t\n\r");
        if (a == std::string::npos)
            throw RuntimeError("plist.parse: <real> is empty", 0);
        body = body.substr(a, b - a + 1);
        try {
            return Value(std::stod(body));
        } catch (...) {
            throw RuntimeError("plist.parse: invalid <real>: \"" + body + "\"", 0);
        }
    }
    if (tag == "data") {
        // Base64-decoded into a Praia bytes string. plutil/Apple emits
        // <data> bodies with arbitrary whitespace (line wraps every 60
        // columns); our decoder tolerates that.
        return Value(base64DecodeIgnoreWs(concatTextChildren(el)));
    }
    if (tag == "date") {
        // Return the ISO 8601 string verbatim. We don't try to parse
        // dates because Praia's time namespace is Unix-seconds and
        // conversion involves date-arithmetic we'd rather not bake in.
        std::string body = concatTextChildren(el);
        size_t a = body.find_first_not_of(" \t\n\r");
        size_t b = body.find_last_not_of(" \t\n\r");
        return Value(a == std::string::npos ? std::string("") :
                     body.substr(a, b - a + 1));
    }
    throw RuntimeError("plist.parse: unknown plist element <" + tag + ">", 0);
}

// ── Stringifier ────────────────────────────────────────────────

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

void emitValue(std::string& out, const Value& v, int indent, int depth,
               std::unordered_set<const void*>& visited);

void pad(std::string& out, int indent, int depth) {
    if (indent > 0) out.append(static_cast<size_t>(indent * depth), ' ');
}

void emitDict(std::string& out, const std::shared_ptr<PraiaMap>& m,
              int indent, int depth, std::unordered_set<const void*>& visited) {
    pad(out, indent, depth);
    out += "<dict>";
    if (indent > 0) out += '\n';
    for (auto& [k, v] : m->entries) {
        if (!k.isString())
            throw RuntimeError("plist.stringify: dict keys must be strings", 0);
        pad(out, indent, depth + 1);
        out += "<key>";
        writeText(out, k.asString());
        out += "</key>";
        if (indent > 0) out += '\n';
        emitValue(out, v, indent, depth + 1, visited);
        if (indent > 0) out += '\n';
    }
    pad(out, indent, depth);
    out += "</dict>";
}

void emitArray(std::string& out, const std::shared_ptr<PraiaArray>& a,
               int indent, int depth, std::unordered_set<const void*>& visited) {
    pad(out, indent, depth);
    out += "<array>";
    if (indent > 0) out += '\n';
    for (auto& v : a->elements) {
        emitValue(out, v, indent, depth + 1, visited);
        if (indent > 0) out += '\n';
    }
    pad(out, indent, depth);
    out += "</array>";
}

void emitValue(std::string& out, const Value& v, int indent, int depth,
               std::unordered_set<const void*>& visited) {
    if (v.isMap()) {
        const void* k = static_cast<const void*>(v.asMap().get());
        if (!visited.insert(k).second)
            throw RuntimeError("plist.stringify: cyclic value graph", 0);
        emitDict(out, v.asMap(), indent, depth, visited);
        visited.erase(k);
        return;
    }
    if (v.isArray()) {
        const void* k = static_cast<const void*>(v.asArray().get());
        if (!visited.insert(k).second)
            throw RuntimeError("plist.stringify: cyclic value graph", 0);
        emitArray(out, v.asArray(), indent, depth, visited);
        visited.erase(k);
        return;
    }
    pad(out, indent, depth);
    if (v.isString()) {
        out += "<string>";
        writeText(out, v.asString());
        out += "</string>";
        return;
    }
    if (v.isInt()) {
        out += "<integer>";
        out += std::to_string(v.asInt());
        out += "</integer>";
        return;
    }
    if (v.isDouble()) {
        double d = v.asNumber();
        if (!std::isfinite(d))
            throw RuntimeError("plist.stringify: cannot encode NaN or Infinity as <real>", 0);
        // Round-trippable representation.
        std::ostringstream s;
        s.precision(17);
        s << d;
        out += "<real>";
        out += s.str();
        out += "</real>";
        return;
    }
    if (v.isBool()) {
        out += v.asBool() ? "<true/>" : "<false/>";
        return;
    }
    if (v.isNil())
        throw RuntimeError("plist.stringify: nil has no representation in plist (the format has no null)", 0);
    throw RuntimeError("plist.stringify: cannot encode value of type " +
                       std::string(v.isCallable() ? "function" :
                                   v.isTagged() ? "tagged" :
                                   v.isFuture() ? "future" : "unknown"), 0);
}

}  // namespace

void registerPlistBuiltins(std::shared_ptr<PraiaMap> plistMap) {
    plistMap->entries[Value("parse")] = Value(makeNative("plist.parse", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("plist.parse() requires a string", 0);
            const auto& src = args[0].asString();
            // Hard-reject binary plists: they start with "bplist".
            // The error message points users at `plutil` which is the
            // canonical conversion tool on macOS.
            if (src.size() >= 6 && src.compare(0, 6, "bplist") == 0)
                throw RuntimeError("plist.parse: binary plists (bplist) aren't supported. "
                                   "Convert with `plutil -convert xml1 file.plist`", 0);
            Value tree = xmlParse(src);
            if (elementTag(tree) != "plist")
                throw RuntimeError("plist.parse: root element must be <plist> (got <" +
                                   elementTag(tree) + ">)", 0);
            const Value& root = singleChildElement(tree, "plist");
            return convertNode(root);
        }));

    plistMap->entries[Value("stringify")] = Value(makeNative("plist.stringify", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty())
                throw RuntimeError("plist.stringify() requires a value", 0);
            int indent = 0;
            if (args.size() > 1 && args[1].isNumber())
                indent = static_cast<int>(args[1].toInt64ForBitwise());
            if (indent < 0) indent = 0;
            std::string out;
            out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
            out += "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                   "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
            out += "<plist version=\"1.0\">";
            if (indent > 0) out += '\n';
            std::unordered_set<const void*> visited;
            emitValue(out, args[0], indent, 0, visited);
            if (indent > 0) out += '\n';
            out += "</plist>\n";
            return Value(out);
        }));
}
