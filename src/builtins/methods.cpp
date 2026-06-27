#include "../builtins.h"
#include "../deprecation.h"
#include "../encoding.h"
#include "../interpreter.h"
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
            // An empty `from` would match at every position, and after
            // each replace `pos += to.size()` either fails to advance
            // (when `to` is also empty → infinite loop) or expands the
            // string indefinitely between every character. Both are
            // never what the caller meant — reject up front.
            if (from.empty())
                throw RuntimeError("replace() search string must not be empty", 0);
            std::string result = str;
            size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos) {
                result.replace(pos, from.size(), to);
                pos += to.size();
            }
            return Value(std::move(result));
        }, {"pattern", "replacement"}));
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
        }, {"index"}));
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
        }, {"pattern", "replacement"}));
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
        }, {"start", "end"}));
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
        }, {"substring", "start"}));
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
            // Empty pad is invalid input — there's no width to pad
            // with. Defaulting to a space is the least-surprising
            // behaviour for callers who reach `padStart(n, "")` from
            // a place where their pad var was lost (e.g. an unset
            // map field). Throwing would be equally defensible.
            if (pad.empty()) pad = " ";
            std::string result = str;
#ifdef HAVE_UTF8PROC
            int currentLen = static_cast<int>(utf8_grapheme_count(result));
            int need = target - currentLen;
            if (need > 0) {
                // Build a prefix of EXACTLY `need` graphemes by
                // repeating pad, then truncating. The old loop
                // appended whole `pad` chunks which overshot any
                // time `need % graphemeCount(pad) != 0`.
                std::string accum;
                while (static_cast<int>(utf8_grapheme_count(accum)) < need)
                    accum += pad;
                auto gs = utf8_graphemes(accum);
                std::string prefix;
                for (int i = 0; i < need && i < static_cast<int>(gs.size()); i++)
                    prefix += gs[i];
                result = prefix + result;
            }
#else
            int currentLen = static_cast<int>(result.size());
            int need = target - currentLen;
            if (need > 0) {
                std::string accum;
                while (static_cast<int>(accum.size()) < need)
                    accum += pad;
                result = accum.substr(0, need) + result;
            }
#endif
            return Value(std::move(result));
        }, {"width", "fillString"}));
    }
    if (name == "padEnd") {
        return Value(makeNative("padEnd", -1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("padEnd() requires a length", 0);
            int target = static_cast<int>(args[0].asNumber());
            std::string pad = " ";
            if (args.size() > 1 && args[1].isString()) pad = args[1].asString();
            if (pad.empty()) pad = " ";
            std::string result = str;
#ifdef HAVE_UTF8PROC
            int currentLen = static_cast<int>(utf8_grapheme_count(result));
            int need = target - currentLen;
            if (need > 0) {
                std::string accum;
                while (static_cast<int>(utf8_grapheme_count(accum)) < need)
                    accum += pad;
                auto gs = utf8_graphemes(accum);
                for (int i = 0; i < need && i < static_cast<int>(gs.size()); i++)
                    result += gs[i];
            }
#else
            int currentLen = static_cast<int>(result.size());
            int need = target - currentLen;
            if (need > 0) {
                std::string accum;
                while (static_cast<int>(accum.size()) < need)
                    accum += pad;
                result += accum.substr(0, need);
            }
#endif
            return Value(std::move(result));
        }, {"width", "fillString"}));
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
    if (name == "encode") {
        // Encode this UTF-8 string into bytes in the named encoding.
        // The returned value is a Praia bytes value (just a string at
        // the C++ level) — pass it to bytes.* helpers or write it to
        // a file. Throws on unencodable codepoints (e.g. emoji into
        // latin-1) or unknown encoding names.
        return Value(makeNative("encode", 1, [s=str](const std::vector<Value>& args) -> Value { const auto& str = *s;
            if (args.empty() || !args[0].isString())
                throw RuntimeError("encode() requires an encoding name (e.g. 'utf-8', 'latin-1', 'utf-16le')", 0);
            try {
                return Value(praia::encoding::encode(str, args[0].asString()));
            } catch (const praia::encoding::EncodingError& e) {
                throw RuntimeError(std::string("encode(): ") + e.what(), 0);
            }
        }));
    }
    if (name == "reverse") {
        // Grapheme-aware reverse: "héllo" (regardless of whether é is
        // precomposed or e+combining) round-trips correctly, and emoji
        // ZWJ sequences (👨‍👩‍👧‍👦) stay intact as a single cluster.
        // Falls back to byte-reverse when utf8proc isn't linked — the
        // ASCII case still works there.
        return Value(makeNative("reverse", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
#ifdef HAVE_UTF8PROC
            auto gs = utf8_graphemes(str);
            std::string out;
            out.reserve(str.size());
            for (auto it = gs.rbegin(); it != gs.rend(); ++it) out += *it;
            return Value(std::move(out));
#else
            return Value(std::string(str.rbegin(), str.rend()));
#endif
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
            if (pad.empty()) pad = " ";
#ifdef HAVE_UTF8PROC
            int currentLen = static_cast<int>(utf8_grapheme_count(str));
#else
            int currentLen = static_cast<int>(str.size());
#endif
            if (currentLen >= target) return Value(str);
            int total = target - currentLen;
            int left = total / 2;
            int right = total - left;
            // Previously this appended a full `pad` chunk per side per
            // iteration, which overshot whenever the side need wasn't
            // a multiple of pad's length and infinite-looped when pad
            // was empty. Now we build the exact `left` / `right`
            // sequences by repeating pad then truncating.
            auto buildChunk = [&pad](int need) -> std::string {
                std::string accum;
#ifdef HAVE_UTF8PROC
                while (static_cast<int>(utf8_grapheme_count(accum)) < need)
                    accum += pad;
                auto gs = utf8_graphemes(accum);
                std::string out;
                for (int i = 0; i < need && i < static_cast<int>(gs.size()); i++)
                    out += gs[i];
                return out;
#else
                while (static_cast<int>(accum.size()) < need)
                    accum += pad;
                return accum.substr(0, need);
#endif
            };
            return Value(buildChunk(left) + str + buildChunk(right));
        }, {"width", "fillString"}));
    }
    if (name == "isDigit") {
        return Value(makeNative("isDigit", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
            if (str.empty()) return Value(false);
            for (unsigned char c : str) if (!std::isdigit(c)) return Value(false);
            return Value(true);
        }));
    }
    if (name == "isAlpha") {
        return Value(makeNative("isAlpha", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
            if (str.empty()) return Value(false);
            for (unsigned char c : str) if (!std::isalpha(c)) return Value(false);
            return Value(true);
        }));
    }
    if (name == "isAlnum") {
        return Value(makeNative("isAlnum", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
            if (str.empty()) return Value(false);
            for (unsigned char c : str) if (!std::isalnum(c)) return Value(false);
            return Value(true);
        }));
    }
    if (name == "isSpace") {
        return Value(makeNative("isSpace", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
            if (str.empty()) return Value(false);
            for (unsigned char c : str) if (!std::isspace(c)) return Value(false);
            return Value(true);
        }));
    }
    if (name == "isUpper") {
        return Value(makeNative("isUpper", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
            if (str.empty()) return Value(false);
            for (unsigned char c : str) if (std::isalpha(c) && !std::isupper(c)) return Value(false);
            return Value(true);
        }));
    }
    if (name == "isLower") {
        return Value(makeNative("isLower", 0, [s=str](const std::vector<Value>&) -> Value { const auto& str = *s;
            if (str.empty()) return Value(false);
            for (unsigned char c : str) if (std::isalpha(c) && !std::islower(c)) return Value(false);
            return Value(true);
        }));
    }
    throw RuntimeError("String has no method '" + name + "'", line);
}

// Resolve which engine is driving this call and forward to the
// shared deprecation helper. Method-dispatch already threads both
// pointers (one will be null), so picking the right state set is a
// single nullness check.
static void noteDeprecatedMethod(Interpreter* interp, VM* vm,
                                 const std::string& oldName,
                                 const std::string& replacementPure,
                                 const std::string& replacementMutating,
                                 int line) {
    bool strict = vm ? vm->strictDeprecations()
                     : (interp ? interp->strictDeprecations() : false);
    if (!interp && !vm) return; // defensive — shouldn't happen at runtime
    auto& warned = vm ? vm->warnedDeprecationsSet()
                      : interp->warnedDeprecationsSet();
    praia::emitMethodDeprecation(strict, warned, oldName,
                                 replacementPure, replacementMutating,
                                 line);
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
    if (name == "reverseInPlace") {
        return Value(makeNative("reverseInPlace", 0, [arr](const std::vector<Value>&) -> Value {
            std::reverse(arr->elements.begin(), arr->elements.end());
            return Value();
        }));
    }
    if (name == "reversed") {
        return Value(makeNative("reversed", 0, [arr](const std::vector<Value>&) -> Value {
            auto out = gcNew<PraiaArray>();
            out->elements = arr->elements;
            std::reverse(out->elements.begin(), out->elements.end());
            return Value(out);
        }));
    }
    if (name == "reverse") {
        noteDeprecatedMethod(interp, vm, "reverse", "reversed", "reverseInPlace", line);
        return getArrayMethod(arr, "reverseInPlace", line, interp, vm);
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
        }, {"start", "end"}));
    }
    if (name == "indexOf") {
        return Value(makeNative("indexOf", 1, [arr](const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < arr->elements.size(); i++)
                if (arr->elements[i] == args[0]) return Value(static_cast<int64_t>(i));
            return Value(static_cast<int64_t>(-1));
        }));
    }
    if (name == "lastIndexOf") {
        return Value(makeNative("lastIndexOf", 1, [arr](const std::vector<Value>& args) -> Value {
            for (int64_t i = static_cast<int64_t>(arr->elements.size()) - 1; i >= 0; i--)
                if (arr->elements[i] == args[0]) return Value(i);
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
    // `sorted` (pure) and `sortInPlace` (mutating) share the same
    // comparator logic. Capture-by-reference of the same helper
    // would tie one's behaviour to the other; instead, factor the
    // comparator selection into an inline lambda used by both
    // method bodies.
    if (name == "sorted") {
        return Value(makeNative("sorted", -1, [arr, interp, vm](const std::vector<Value>& args) -> Value {
            auto out = gcNew<PraiaArray>();
            out->elements = arr->elements;
            auto& elems = out->elements;
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
            return Value(out);
        }, {"comparator"}));
    }
    if (name == "sortInPlace") {
        return Value(makeNative("sortInPlace", -1, [arr, interp, vm](const std::vector<Value>& args) -> Value {
            auto& elems = arr->elements;
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
            return Value();
        }, {"comparator"}));
    }
    if (name == "sort") {
        noteDeprecatedMethod(interp, vm, "sort", "sorted", "sortInPlace", line);
        return getArrayMethod(arr, "sorted", line, interp, vm);
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
        // Intentionally NOT named-callable: the native dispatch
        // pads omitted positions with nil before the lambda runs,
        // so a call like `m.get(default: 1)` would arrive here as
        // `args = [nil, 1]` and probe the map for a literal `nil`
        // key — silently wrong when nil is a valid map key. Until
        // there's a way to plumb a "this position was omitted"
        // signal into native bodies, we keep `get` positional only.
        // Callers write `m.get(key, default)` (or just `m.get(key)`).
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
    if (name == "mergeInPlace") {
        // Mutating sibling of `merge` — `other`'s values win on key
        // collision, matching `merge`'s precedence rule.
        return Value(makeNative("mergeInPlace", 1, [map](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap())
                throw RuntimeError("mergeInPlace() requires a map argument", 0);
            for (auto& [k, v] : args[0].asMap()->entries)
                map->entries[k] = v;
            return Value();
        }));
    }
    if (name == "entries") {
        return Value(makeNative("entries", 0, [map](const std::vector<Value>&) -> Value {
            auto result = gcNew<PraiaArray>();
            for (auto& [k, v] : map->entries) {
                auto pair = gcNew<PraiaArray>();
                pair->elements.push_back(k);
                pair->elements.push_back(v);
                result->elements.push_back(Value(pair));
            }
            return Value(result);
        }));
    }
    if (name == "clear") {
        return Value(makeNative("clear", 0, [map](const std::vector<Value>&) -> Value {
            map->entries.clear();
            return Value();
        }));
    }
    throw RuntimeError("Map has no method '" + name + "'", line);
}

// Set methods. Membership tests use the same ValueHash/ValueKeyEqual
// predicates as PraiaMap keys, so numeric semantics match (3 == 3.0,
// NaN compares equal to NaN here). Element insertion validates
// isHashable so users can't sneak unhashable values (arrays, maps,
// other sets, instances) in through .add().
Value getSetMethod(std::shared_ptr<PraiaSet> set,
                   const std::string& name, int line) {
    if (name == "add") {
        return Value(makeNative("add", 1, [set](const std::vector<Value>& args) -> Value {
            if (!isHashable(args[0]))
                throw RuntimeError("set.add(): element must be a primitive (nil, bool, number, string)", 0);
            auto [_, inserted] = set->elements.insert(args[0]);
            return Value(inserted);  // true if newly added, false if already present
        }));
    }
    if (name == "remove") {
        return Value(makeNative("remove", 1, [set](const std::vector<Value>& args) -> Value {
            return Value(set->elements.erase(args[0]) > 0);
        }));
    }
    if (name == "has") {
        return Value(makeNative("has", 1, [set](const std::vector<Value>& args) -> Value {
            return Value(set->elements.find(args[0]) != set->elements.end());
        }));
    }
    if (name == "size") {
        return Value(makeNative("size", 0, [set](const std::vector<Value>&) -> Value {
            return Value(static_cast<int64_t>(set->elements.size()));
        }));
    }
    if (name == "clear") {
        return Value(makeNative("clear", 0, [set](const std::vector<Value>&) -> Value {
            set->elements.clear();
            return Value();
        }));
    }
    if (name == "toArray") {
        return Value(makeNative("toArray", 0, [set](const std::vector<Value>&) -> Value {
            auto arr = gcNew<PraiaArray>();
            arr->elements.reserve(set->elements.size());
            for (auto& e : set->elements) arr->elements.push_back(e);
            return Value(arr);
        }));
    }
    if (name == "clone") {
        return Value(makeNative("clone", 0, [set](const std::vector<Value>&) -> Value {
            auto copy = gcNew<PraiaSet>();
            copy->elements = set->elements;
            return Value(copy);
        }));
    }
    // Set-algebra. All produce a NEW set; the receiver isn't mutated.
    if (name == "union") {
        return Value(makeNative("union", 1, [set](const std::vector<Value>& args) -> Value {
            if (!args[0].isSet())
                throw RuntimeError("set.union(): argument must be a set", 0);
            auto result = gcNew<PraiaSet>();
            result->elements = set->elements;
            for (auto& e : args[0].asSet()->elements) result->elements.insert(e);
            return Value(result);
        }));
    }
    if (name == "intersection") {
        return Value(makeNative("intersection", 1, [set](const std::vector<Value>& args) -> Value {
            if (!args[0].isSet())
                throw RuntimeError("set.intersection(): argument must be a set", 0);
            auto result = gcNew<PraiaSet>();
            auto& other = args[0].asSet()->elements;
            // Iterate the smaller for fewer lookups.
            auto& smaller = (set->elements.size() <= other.size())
                              ? set->elements : other;
            auto& larger  = (set->elements.size() <= other.size())
                              ? other : set->elements;
            for (auto& e : smaller)
                if (larger.find(e) != larger.end()) result->elements.insert(e);
            return Value(result);
        }));
    }
    if (name == "difference") {
        return Value(makeNative("difference", 1, [set](const std::vector<Value>& args) -> Value {
            if (!args[0].isSet())
                throw RuntimeError("set.difference(): argument must be a set", 0);
            auto result = gcNew<PraiaSet>();
            auto& other = args[0].asSet()->elements;
            for (auto& e : set->elements)
                if (other.find(e) == other.end()) result->elements.insert(e);
            return Value(result);
        }));
    }
    // In-place set-algebra. Mutate the receiver; return nil.
    if (name == "unionInPlace") {
        return Value(makeNative("unionInPlace", 1, [set](const std::vector<Value>& args) -> Value {
            if (!args[0].isSet())
                throw RuntimeError("set.unionInPlace(): argument must be a set", 0);
            for (auto& e : args[0].asSet()->elements) set->elements.insert(e);
            return Value();
        }));
    }
    if (name == "intersectionInPlace") {
        return Value(makeNative("intersectionInPlace", 1, [set](const std::vector<Value>& args) -> Value {
            if (!args[0].isSet())
                throw RuntimeError("set.intersectionInPlace(): argument must be a set", 0);
            auto& other = args[0].asSet()->elements;
            for (auto it = set->elements.begin(); it != set->elements.end(); ) {
                if (other.find(*it) == other.end()) it = set->elements.erase(it);
                else ++it;
            }
            return Value();
        }));
    }
    if (name == "differenceInPlace") {
        return Value(makeNative("differenceInPlace", 1, [set](const std::vector<Value>& args) -> Value {
            if (!args[0].isSet())
                throw RuntimeError("set.differenceInPlace(): argument must be a set", 0);
            for (auto& e : args[0].asSet()->elements) set->elements.erase(e);
            return Value();
        }));
    }
    if (name == "isSubset") {
        return Value(makeNative("isSubset", 1, [set](const std::vector<Value>& args) -> Value {
            if (!args[0].isSet())
                throw RuntimeError("set.isSubset(): argument must be a set", 0);
            auto& other = args[0].asSet()->elements;
            if (set->elements.size() > other.size()) return Value(false);
            for (auto& e : set->elements)
                if (other.find(e) == other.end()) return Value(false);
            return Value(true);
        }));
    }
    throw RuntimeError("Set has no method '" + name + "'", line);
}
