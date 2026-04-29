#include "../builtins.h"
#include "../unicode.h"
#include "../value.h"
#include "../vm/vm.h"
#include <algorithm>
#include <memory>
#include <string>
#include "../gc_heap.h"

#ifdef HAVE_RE2
#include <re2/re2.h>
#else
#include <regex>
#endif

Value getStringMethod(const std::string& strRef,
                      const std::string& name, int line) {
    // Share the string data via shared_ptr — avoids deep-copying the string
    // into every lambda capture. The shared_ptr copy is O(1).
    auto str = std::make_shared<std::string>(strRef);

    if (name == "upper") {
        return Value(makeNative("upper", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
#ifdef HAVE_UTF8PROC
            return Value(utf8_upper(str));
#else
            std::string r = str;
            std::transform(r.begin(), r.end(), r.begin(), ::toupper);
            return Value(std::move(r));
#endif
        }));
    }
    if (name == "lower") {
        return Value(makeNative("lower", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
#ifdef HAVE_UTF8PROC
            return Value(utf8_lower(str));
#else
            std::string r = str;
            std::transform(r.begin(), r.end(), r.begin(), ::tolower);
            return Value(std::move(r));
#endif
        }));
    }
    if (name == "strip") {
        return Value(makeNative("strip", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
            size_t start = str.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) return Value(std::string(""));
            size_t end = str.find_last_not_of(" \t\n\r");
            return Value(str.substr(start, end - start + 1));
        }));
    }
    if (name == "split") {
        return Value(makeNative("split", 1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (!args[0].isString())
                throw RuntimeError("split() separator must be a string", 0);
            auto& sep = args[0].asString();
            auto arr = gcNew<PraiaArray>();
            if (sep.empty()) {
#ifdef HAVE_UTF8PROC
                for (auto& g : utf8_graphemes(str))
                    arr->elements.push_back(Value(std::move(g)));
#else
                for (char c : str)
                    arr->elements.push_back(Value(std::string(1, c)));
#endif
                return Value(arr);
            }
            size_t pos = 0, found;
            while ((found = str.find(sep, pos)) != std::string::npos) {
                arr->elements.push_back(Value(str.substr(pos, found - pos)));
                pos = found + sep.size();
            }
            arr->elements.push_back(Value(str.substr(pos)));
            return Value(arr);
        }));
    }
    if (name == "contains") {
        return Value(makeNative("contains", 1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (!args[0].isString())
                throw RuntimeError("contains() argument must be a string", 0);
            return Value(str.find(args[0].asString()) != std::string::npos);
        }));
    }
    if (name == "replace") {
        return Value(makeNative("replace", 2, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("replace() arguments must be strings", 0);
            auto& from = args[0].asString();
            auto& to = args[1].asString();
            std::string result = str;
            size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos) {
                result.replace(pos, from.size(), to);
                pos += to.size();
            }
            return Value(std::move(result));
        }));
    }
    if (name == "startsWith") {
        return Value(makeNative("startsWith", 1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (!args[0].isString())
                throw RuntimeError("startsWith() argument must be a string", 0);
            auto& prefix = args[0].asString();
            return Value(str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0);
        }));
    }
    if (name == "endsWith") {
        return Value(makeNative("endsWith", 1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (!args[0].isString())
                throw RuntimeError("endsWith() argument must be a string", 0);
            auto& suffix = args[0].asString();
            return Value(str.size() >= suffix.size() &&
                         str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0);
        }));
    }
    if (name == "title") {
        return Value(makeNative("title", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
#ifdef HAVE_UTF8PROC
            return Value(utf8_title(str));
#else
            std::string r = str;
            bool capNext = true;
            for (auto& c : r) {
                if (std::isspace(c)) { capNext = true; }
                else if (capNext) { c = std::toupper(c); capNext = false; }
                else { c = std::tolower(c); }
            }
            return Value(std::move(r));
#endif
        }));
    }
    if (name == "capitalize") {
        return Value(makeNative("capitalize", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
#ifdef HAVE_UTF8PROC
            if (str.empty()) return Value(str);
            size_t first_len = utf8_first_grapheme_bytes(str);
            return Value(utf8_upper(str.substr(0, first_len)) +
                         utf8_lower(str.substr(first_len)));
#else
            std::string r = str;
            if (!r.empty()) {
                r[0] = std::toupper(r[0]);
                for (size_t i = 1; i < r.size(); i++)
                    r[i] = std::tolower(r[i]);
            }
            return Value(std::move(r));
#endif
        }));
    }
    if (name == "capitalizeFirst") {
        return Value(makeNative("capitalizeFirst", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
#ifdef HAVE_UTF8PROC
            if (str.empty()) return Value(str);
            size_t first_len = utf8_first_grapheme_bytes(str);
            return Value(utf8_upper(str.substr(0, first_len)) + str.substr(first_len));
#else
            std::string r = str;
            if (!r.empty()) r[0] = std::toupper(r[0]);
            return Value(std::move(r));
#endif
        }));
    }
    if (name == "charCode") {
        return Value(makeNative("charCode", -1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            int idx = 0;
            if (!args.empty() && args[0].isNumber())
                idx = static_cast<int>(args[0].asNumber());
#ifdef HAVE_UTF8PROC
            auto gs = utf8_graphemes(str);
            int len = static_cast<int>(gs.size());
            if (idx < 0) idx += len;
            if (idx < 0 || idx >= len)
                throw RuntimeError("charCode index out of bounds", 0);
            return Value(static_cast<int64_t>(utf8_first_codepoint(gs[idx])));
#else
            if (idx < 0) idx += static_cast<int>(str.size());
            if (idx < 0 || idx >= static_cast<int>(str.size()))
                throw RuntimeError("charCode index out of bounds", 0);
            return Value(static_cast<int64_t>(static_cast<unsigned char>(str[idx])));
#endif
        }));
    }
    if (name == "test") {
        return Value(makeNative("test", 1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (!args[0].isString())
                throw RuntimeError("test() pattern must be a string", 0);
#ifdef HAVE_RE2
            RE2 re(args[0].asString());
            if (!re.ok()) throw RuntimeError("Invalid regex: " + re.error(), 0);
            return Value(RE2::PartialMatch(str, re));
#else
            try {
                std::regex re(args[0].asString());
                return Value(std::regex_search(str, re));
            } catch (const std::regex_error& e) {
                throw RuntimeError(std::string("Invalid regex: ") + e.what(), 0);
            }
#endif
        }));
    }
    if (name == "match") {
        return Value(makeNative("match", 1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (!args[0].isString())
                throw RuntimeError("match() pattern must be a string", 0);
#ifdef HAVE_RE2
            std::string pattern = args[0].asString();
            RE2 re(pattern);
            if (!re.ok()) throw RuntimeError("Invalid regex: " + re.error(), 0);
            int ngroups = re.NumberOfCapturingGroups();
            std::vector<re2::StringPiece> groups(ngroups + 1);
            re2::StringPiece input(str);
            if (!re.Match(input, 0, str.size(), RE2::UNANCHORED, groups.data(), ngroups + 1))
                return Value();
            auto result = gcNew<PraiaMap>();
            result->entries[Value("match")] = Value(std::string(groups[0]));
            size_t matchPos = static_cast<size_t>(groups[0].data() - str.data());
#ifdef HAVE_UTF8PROC
            result->entries[Value("index")] = Value(static_cast<int64_t>(
                utf8_byte_to_grapheme_index(str, matchPos)));
#else
            result->entries[Value("index")] = Value(static_cast<int64_t>(matchPos));
#endif
            auto grpArr = gcNew<PraiaArray>();
            for (int i = 1; i <= ngroups; i++)
                grpArr->elements.push_back(Value(std::string(groups[i])));
            result->entries[Value("groups")] = Value(grpArr);
            return Value(result);
#else
            try {
                std::regex re(args[0].asString());
                std::smatch m;
                if (!std::regex_search(str, m, re)) return Value();
                auto result = gcNew<PraiaMap>();
                result->entries[Value("match")] = Value(m[0].str());
#ifdef HAVE_UTF8PROC
                result->entries[Value("index")] = Value(static_cast<int64_t>(
                    utf8_byte_to_grapheme_index(str, static_cast<size_t>(m.position(0)))));
#else
                result->entries[Value("index")] = Value(static_cast<int64_t>(m.position(0)));
#endif
                auto grpArr = gcNew<PraiaArray>();
                for (size_t i = 1; i < m.size(); i++)
                    grpArr->elements.push_back(Value(m[i].str()));
                result->entries[Value("groups")] = Value(grpArr);
                return Value(result);
            } catch (const std::regex_error& e) {
                throw RuntimeError(std::string("Invalid regex: ") + e.what(), 0);
            }
#endif
        }));
    }
    if (name == "matchAll") {
        return Value(makeNative("matchAll", 1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (!args[0].isString())
                throw RuntimeError("matchAll() pattern must be a string", 0);
#ifdef HAVE_RE2
            RE2 re(args[0].asString());
            if (!re.ok()) throw RuntimeError("Invalid regex: " + re.error(), 0);
            int ngroups = re.NumberOfCapturingGroups();
            auto results = gcNew<PraiaArray>();
            re2::StringPiece input(str);
            std::vector<re2::StringPiece> groups(ngroups + 1);
            size_t startPos = 0;
            while (startPos <= str.size() &&
                   re.Match(input, startPos, str.size(), RE2::UNANCHORED, groups.data(), ngroups + 1)) {
                auto entry = gcNew<PraiaMap>();
                entry->entries[Value("match")] = Value(std::string(groups[0]));
                size_t matchPos = static_cast<size_t>(groups[0].data() - str.data());
#ifdef HAVE_UTF8PROC
                entry->entries[Value("index")] = Value(static_cast<int64_t>(
                    utf8_byte_to_grapheme_index(str, matchPos)));
#else
                entry->entries[Value("index")] = Value(static_cast<int64_t>(matchPos));
#endif
                auto grpArr = gcNew<PraiaArray>();
                for (int i = 1; i <= ngroups; i++)
                    grpArr->elements.push_back(Value(std::string(groups[i])));
                entry->entries[Value("groups")] = Value(grpArr);
                results->elements.push_back(Value(entry));
                // Advance past this match (avoid infinite loop on zero-length match)
                size_t matchEnd = matchPos + groups[0].size();
                startPos = (groups[0].empty()) ? matchEnd + 1 : matchEnd;
            }
            return Value(results);
#else
            try {
                std::regex re(args[0].asString());
                auto results = gcNew<PraiaArray>();
                auto begin = std::sregex_iterator(str.begin(), str.end(), re);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end; ++it) {
                    auto entry = gcNew<PraiaMap>();
                    entry->entries[Value("match")] = Value((*it)[0].str());
#ifdef HAVE_UTF8PROC
                    entry->entries[Value("index")] = Value(static_cast<int64_t>(
                        utf8_byte_to_grapheme_index(str, static_cast<size_t>(it->position(0)))));
#else
                    entry->entries[Value("index")] = Value(static_cast<int64_t>(it->position(0)));
#endif
                    auto grpArr = gcNew<PraiaArray>();
                    for (size_t i = 1; i < it->size(); i++)
                        grpArr->elements.push_back(Value((*it)[i].str()));
                    entry->entries[Value("groups")] = Value(grpArr);
                    results->elements.push_back(Value(entry));
                }
                return Value(results);
            } catch (const std::regex_error& e) {
                throw RuntimeError(std::string("Invalid regex: ") + e.what(), 0);
            }
#endif
        }));
    }
    if (name == "replacePattern") {
        return Value(makeNative("replacePattern", 2, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("replacePattern() requires string arguments", 0);
#ifdef HAVE_RE2
            RE2 re(args[0].asString());
            if (!re.ok()) throw RuntimeError("Invalid regex: " + re.error(), 0);
            // Convert $N backreferences to \N for RE2 compatibility
            // $0 → \0 (whole match), $1 → \1, ..., $$ → literal $
            std::string rewrite;
            std::string src = args[1].asString();
            for (size_t i = 0; i < src.size(); i++) {
                if (src[i] == '$' && i + 1 < src.size()) {
                    if (src[i + 1] == '$') { rewrite += '$'; i++; } // $$ → $
                    else if (std::isdigit(src[i + 1])) { rewrite += '\\'; } // $N → \N
                    else rewrite += src[i];
                } else rewrite += src[i];
            }
            std::string result = str;
            RE2::GlobalReplace(&result, re, rewrite);
            return Value(std::move(result));
#else
            try {
                std::regex re(args[0].asString());
                return Value(std::regex_replace(str, re, args[1].asString()));
            } catch (const std::regex_error& e) {
                throw RuntimeError(std::string("Invalid regex: ") + e.what(), 0);
            }
#endif
        }));
    }
    if (name == "slice") {
        return Value(makeNative("slice", -1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("slice() requires a start index", 0);
#ifdef HAVE_UTF8PROC
            auto gs = utf8_graphemes(str);
            int len = static_cast<int>(gs.size());
            int start = static_cast<int>(args[0].asNumber());
            if (start < 0) start += len;
            if (start < 0) start = 0;
            if (start >= len) return Value(std::string(""));
            int end = len;
            if (args.size() > 1 && args[1].isNumber()) {
                end = static_cast<int>(args[1].asNumber());
                if (end < 0) end += len;
                if (end <= start) return Value(std::string(""));
                if (end > len) end = len;
            }
            std::string result;
            for (int i = start; i < end; i++) result += gs[i];
            return Value(std::move(result));
#else
            int len = static_cast<int>(str.size());
            int start = static_cast<int>(args[0].asNumber());
            if (start < 0) start += len;
            if (start < 0) start = 0;
            if (start >= len) return Value(std::string(""));
            if (args.size() > 1 && args[1].isNumber()) {
                int end = static_cast<int>(args[1].asNumber());
                if (end < 0) end += len;
                if (end <= start) return Value(std::string(""));
                if (end > len) end = len;
                return Value(str.substr(start, end - start));
            }
            return Value(str.substr(start));
#endif
        }));
    }
    if (name == "indexOf") {
        return Value(makeNative("indexOf", -1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (args.empty() || !args[0].isString())
                throw RuntimeError("indexOf() requires a string argument", 0);
#ifdef HAVE_UTF8PROC
            size_t startByte = 0;
            if (args.size() > 1 && args[1].isNumber()) {
                // Convert grapheme start index to byte offset
                int gi = static_cast<int>(args[1].asNumber());
                auto gs = utf8_graphemes(str);
                if (gi < 0) gi += static_cast<int>(gs.size());
                if (gi < 0 || gi >= static_cast<int>(gs.size())) return Value(static_cast<int64_t>(-1));
                for (int i = 0; i < gi; i++) startByte += gs[i].size();
            }
            auto pos = str.find(args[0].asString(), startByte);
            if (pos == std::string::npos) return Value(static_cast<int64_t>(-1));
            return Value(static_cast<int64_t>(utf8_byte_to_grapheme_index(str, pos)));
#else
            size_t startPos = 0;
            if (args.size() > 1 && args[1].isNumber())
                startPos = static_cast<size_t>(args[1].asNumber());
            auto pos = str.find(args[0].asString(), startPos);
            return Value(pos == std::string::npos ? static_cast<int64_t>(-1) : static_cast<int64_t>(pos));
#endif
        }));
    }
    if (name == "lastIndexOf") {
        return Value(makeNative("lastIndexOf", 1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (!args[0].isString())
                throw RuntimeError("lastIndexOf() requires a string argument", 0);
            auto pos = str.rfind(args[0].asString());
#ifdef HAVE_UTF8PROC
            if (pos == std::string::npos) return Value(static_cast<int64_t>(-1));
            return Value(static_cast<int64_t>(utf8_byte_to_grapheme_index(str, pos)));
#else
            return Value(pos == std::string::npos ? static_cast<int64_t>(-1) : static_cast<int64_t>(pos));
#endif
        }));
    }
    if (name == "repeat") {
        return Value(makeNative("repeat", 1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (!args[0].isNumber())
                throw RuntimeError("repeat() requires a number", 0);
            int count = static_cast<int>(args[0].asNumber());
            if (count < 0) throw RuntimeError("repeat() count cannot be negative", 0);
            std::string result;
            result.reserve(str.size() * count);
            for (int i = 0; i < count; i++) result += str;
            return Value(std::move(result));
        }));
    }
    if (name == "padStart") {
        return Value(makeNative("padStart", -1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("padStart() requires a length", 0);
            int target = static_cast<int>(args[0].asNumber());
            std::string pad = " ";
            if (args.size() > 1 && args[1].isString()) pad = args[1].asString();
            std::string result = str;
#ifdef HAVE_UTF8PROC
            int currentLen = static_cast<int>(utf8_grapheme_count(result));
            int padLen = static_cast<int>(utf8_grapheme_count(pad));
            if (padLen < 1) padLen = 1; // avoid infinite loop on empty pad
            while (currentLen < target) {
                result = pad + result;
                currentLen += padLen;
            }
#else
            while (static_cast<int>(result.size()) < target)
                result = pad + result;
#endif
            return Value(std::move(result));
        }));
    }
    if (name == "padEnd") {
        return Value(makeNative("padEnd", -1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("padEnd() requires a length", 0);
            int target = static_cast<int>(args[0].asNumber());
            std::string pad = " ";
            if (args.size() > 1 && args[1].isString()) pad = args[1].asString();
            std::string result = str;
#ifdef HAVE_UTF8PROC
            int currentLen = static_cast<int>(utf8_grapheme_count(result));
            int padLen = static_cast<int>(utf8_grapheme_count(pad));
            if (padLen < 1) padLen = 1; // avoid infinite loop on empty pad
            while (currentLen < target) {
                result += pad;
                currentLen += padLen;
            }
#else
            while (static_cast<int>(result.size()) < target)
                result += pad;
#endif
            return Value(std::move(result));
        }));
    }
    if (name == "trimStart") {
        return Value(makeNative("trimStart", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
            size_t start = str.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) return Value(std::string(""));
            return Value(str.substr(start));
        }));
    }
    if (name == "trimEnd") {
        return Value(makeNative("trimEnd", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
            size_t end = str.find_last_not_of(" \t\n\r");
            if (end == std::string::npos) return Value(std::string(""));
            return Value(str.substr(0, end + 1));
        }));
    }
    if (name == "graphemes") {
        return Value(makeNative("graphemes", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
            auto arr = gcNew<PraiaArray>();
#ifdef HAVE_UTF8PROC
            for (auto& g : utf8_graphemes(str))
                arr->elements.push_back(Value(std::move(g)));
#else
            for (char c : str)
                arr->elements.push_back(Value(std::string(1, c)));
#endif
            return Value(arr);
        }));
    }
    if (name == "codepoints") {
        return Value(makeNative("codepoints", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
            auto arr = gcNew<PraiaArray>();
#ifdef HAVE_UTF8PROC
            for (int32_t cp : utf8_codepoints(str))
                arr->elements.push_back(Value(static_cast<int64_t>(cp)));
#else
            for (unsigned char c : str)
                arr->elements.push_back(Value(static_cast<int64_t>(c)));
#endif
            return Value(arr);
        }));
    }
    if (name == "bytes") {
        return Value(makeNative("bytes", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
            auto arr = gcNew<PraiaArray>();
            for (unsigned char c : str)
                arr->elements.push_back(Value(static_cast<int64_t>(c)));
            return Value(arr);
        }));
    }
    if (name == "count") {
        return Value(makeNative("count", 1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (!args[0].isString())
                throw RuntimeError("count() requires a string argument", 0);
            auto& sub = args[0].asString();
            if (sub.empty()) return Value(static_cast<int64_t>(0));
            int64_t count = 0;
            size_t pos = 0;
            while ((pos = str.find(sub, pos)) != std::string::npos) {
                count++;
                pos += sub.size();
            }
            return Value(count);
        }));
    }
    if (name == "center") {
        return Value(makeNative("center", -1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("center() requires a width", 0);
            int target = static_cast<int>(args[0].asNumber());
            std::string pad = " ";
            if (args.size() > 1 && args[1].isString()) pad = args[1].asString();
#ifdef HAVE_UTF8PROC
            int currentLen = static_cast<int>(utf8_grapheme_count(str));
#else
            int currentLen = static_cast<int>(str.size());
#endif
            if (currentLen >= target) return Value(str);
            int total = target - currentLen;
            int left = total / 2;
            int right = total - left;
            std::string result;
            for (int i = 0; i < left; i++) result += pad;
            result += str;
            for (int i = 0; i < right; i++) result += pad;
            return Value(std::move(result));
        }));
    }
    throw RuntimeError("String has no method '" + name + "'", line);
}

Value getArrayMethod(std::shared_ptr<PraiaArray> arr,
                     const std::string& name, int line,
                     Interpreter* interp, VM* vm) {
    if (name == "push") {
        return Value(makeNative("push", 1, [arr](const std::vector<Value>& args) -> Value {
            arr->elements.push_back(args[0]);
            return Value();
        }));
    }
    if (name == "pop") {
        return Value(makeNative("pop", 0, [arr](const std::vector<Value>&) -> Value {
            if (arr->elements.empty())
                throw RuntimeError("pop() on empty array", 0);
            Value last = arr->elements.back();
            arr->elements.pop_back();
            return last;
        }));
    }
    if (name == "contains") {
        return Value(makeNative("contains", 1, [arr](const std::vector<Value>& args) -> Value {
            for (auto& e : arr->elements)
                if (e == args[0]) return Value(true);
            return Value(false);
        }));
    }
    if (name == "join") {
        return Value(makeNative("join", 1, [arr](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("join() separator must be a string", 0);
            auto& sep = args[0].asString();
            std::string result;
            for (size_t i = 0; i < arr->elements.size(); i++) {
                if (i > 0) result += sep;
                result += arr->elements[i].toString();
            }
            return Value(std::move(result));
        }));
    }
    if (name == "reverse") {
        return Value(makeNative("reverse", 0, [arr](const std::vector<Value>&) -> Value {
            std::reverse(arr->elements.begin(), arr->elements.end());
            return Value();
        }));
    }
    if (name == "shift") {
        return Value(makeNative("shift", 0, [arr](const std::vector<Value>&) -> Value {
            if (arr->elements.empty())
                throw RuntimeError("shift() on empty array", 0);
            Value first = arr->elements.front();
            arr->elements.erase(arr->elements.begin());
            return first;
        }));
    }
    if (name == "unshift") {
        return Value(makeNative("unshift", 1, [arr](const std::vector<Value>& args) -> Value {
            arr->elements.insert(arr->elements.begin(), args[0]);
            return Value();
        }));
    }
    if (name == "slice") {
        return Value(makeNative("slice", -1, [arr](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("slice() requires a start index", 0);
            int len = static_cast<int>(arr->elements.size());
            int start = static_cast<int>(args[0].asNumber());
            if (start < 0) start += len;
            if (start < 0) start = 0;
            int end = len;
            if (args.size() > 1 && args[1].isNumber()) {
                end = static_cast<int>(args[1].asNumber());
                if (end < 0) end += len;
            }
            if (start >= len || end <= start)
                return Value(gcNew<PraiaArray>());
            if (end > len) end = len;
            auto result = gcNew<PraiaArray>();
            result->elements.assign(arr->elements.begin() + start, arr->elements.begin() + end);
            return Value(result);
        }));
    }
    if (name == "indexOf") {
        return Value(makeNative("indexOf", 1, [arr](const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < arr->elements.size(); i++)
                if (arr->elements[i] == args[0]) return Value(static_cast<int64_t>(i));
            return Value(static_cast<int64_t>(-1));
        }));
    }
    if (name == "find") {
        return Value(makeNative("find", 1, [arr, interp, vm](const std::vector<Value>& args) -> Value {
            if (!args[0].isCallable())
                throw RuntimeError("find() requires a function", 0);
            auto pred = args[0].asCallable();
            for (auto& elem : arr->elements) {
                Value result = vm ? callWithVM(*vm, pred, {elem})
                                  : callSafe(*interp, pred, {elem});
                if (result.isTruthy()) return elem;
            }
            return Value();
        }));
    }
    if (name == "sort") {
        return Value(makeNative("sort", -1, [arr, interp, vm](const std::vector<Value>& args) -> Value {
            auto sorted = gcNew<PraiaArray>();
            sorted->elements = arr->elements;
            auto& elems = sorted->elements;
            if (!args.empty() && args[0].isCallable()) {
                auto cmp = args[0].asCallable();
                std::sort(elems.begin(), elems.end(),
                    [&cmp, interp, vm](const Value& a, const Value& b) -> bool {
                        Value result = vm ? callWithVM(*vm, cmp, {a, b})
                                          : callSafe(*interp, cmp, {a, b});
                        if (result.isNumber()) return result.asNumber() < 0;
                        return result.isTruthy();
                    });
            } else {
                std::sort(elems.begin(), elems.end(), [](const Value& a, const Value& b) {
                    if (a.isNumber() && b.isNumber()) return a.asNumber() < b.asNumber();
                    return a.toString() < b.toString();
                });
            }
            return Value(sorted);
        }));
    }
    throw RuntimeError("Array has no method '" + name + "'", line);
}

Value getMapMethod(std::shared_ptr<PraiaMap> map,
                   const std::string& name, int line) {
    if (name == "has") {
        return Value(makeNative("has", 1, [map](const std::vector<Value>& args) -> Value {
            return Value(map->entries.find(args[0]) != map->entries.end());
        }));
    }
    if (name == "get") {
        return Value(makeNative("get", -1, [map](const std::vector<Value>& args) -> Value {
            if (args.empty())
                throw RuntimeError("get() requires at least a key argument", 0);
            auto it = map->entries.find(args[0]);
            if (it != map->entries.end()) return it->second;
            if (args.size() > 1) return args[1];
            return Value();
        }));
    }
    if (name == "delete") {
        return Value(makeNative("delete", 1, [map](const std::vector<Value>& args) -> Value {
            auto it = map->entries.find(args[0]);
            if (it == map->entries.end()) return Value(false);
            map->entries.erase(it);
            return Value(true);
        }));
    }
    if (name == "merge") {
        return Value(makeNative("merge", 1, [map](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap())
                throw RuntimeError("merge() requires a map argument", 0);
            auto result = gcNew<PraiaMap>();
            result->entries = map->entries;
            for (auto& [k, v] : args[0].asMap()->entries)
                result->entries[k] = v;
            return Value(result);
        }));
    }
    throw RuntimeError("Map has no method '" + name + "'", line);
}
