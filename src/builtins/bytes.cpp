#include "../builtins.h"
#include <cstring>
#include "../gc_heap.h"

void registerBytesBuiltins(std::shared_ptr<PraiaMap> bytesMap) {
    // ── Struct format helpers ──
    // Format: optional endian prefix (> big, < little, default big-endian)
    // followed by type chars with optional repeat counts:
    //   b/B = i8/u8, h/H = i16/u16, i/I = i32/u32, q/Q = i64/u64,
    //   f = f32, d = f64, x = pad byte. Example: ">3BHI" = 3 bytes + u16 + u32.

    struct StructField { char type; int size; };

    // Per-field byte cap. `>9999999999h` would otherwise overflow the
    // count accumulator silently and push a near-infinite number of
    // StructFields. We bound `count * sz` to 16 MiB per field — comfortably
    // larger than any realistic struct-format unpack (audio buffers, image
    // rows, protocol parsing) while bounding worst-case allocation. The
    // accumulator is also bounded during digit parsing so a giant numeric
    // prefix throws before it can wrap.
    constexpr int64_t kMaxFieldBytes = 16 * 1024 * 1024;

    auto parseStructFmt = [&](const std::string& fmt, bool& bigEndian) -> std::vector<StructField> {
        std::vector<StructField> fields;
        size_t i = 0;
        bigEndian = true; // default big-endian
        if (!fmt.empty() && (fmt[0] == '>' || fmt[0] == '!' || fmt[0] == '<' || fmt[0] == '=')) {
            bigEndian = (fmt[0] == '>' || fmt[0] == '!');
            i = 1;
        }
        while (i < fmt.size()) {
            int64_t count = 0;
            while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
                count = count * 10 + (fmt[i] - '0');
                if (count > kMaxFieldBytes)
                    throw RuntimeError("Struct format count exceeds 16 MiB limit", 0);
                i++;
            }
            if (count == 0) count = 1;
            if (i >= fmt.size()) throw RuntimeError("Incomplete struct format", 0);
            char c = fmt[i++];
            int sz = 0;
            switch (c) {
                case 'b': case 'B': sz = 1; break;
                case 'h': case 'H': sz = 2; break;
                case 'i': case 'I': sz = 4; break;
                case 'q': case 'Q': sz = 8; break;
                case 'f': sz = 4; break;
                case 'd': sz = 8; break;
                case 'x': sz = 1; break;
                default: throw RuntimeError(std::string("Unknown struct format char: '") + c + "'", 0);
            }
            if (count * sz > kMaxFieldBytes)
                throw RuntimeError("Struct format field would consume more than 16 MiB", 0);
            for (int64_t j = 0; j < count; j++) fields.push_back({c, sz});
        }
        return fields;
    };

    // bytes.pack(format, values)
    bytesMap->entries[Value("pack")] = Value(makeNative("bytes.pack", 2,
        [parseStructFmt](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isArray())
                throw RuntimeError("bytes.pack(format, values) requires a format string and array", 0);
            auto& fmt = args[0].asString();
            auto& vals = args[1].asArray()->elements;
            std::string result;

            bool big;
            auto fields = parseStructFmt(fmt, big);
            size_t vi = 0;
            for (auto& f : fields) {
                if (f.type == 'x') { result += '\0'; continue; }
                if (vi >= vals.size())
                    throw RuntimeError("bytes.pack: not enough values for format", 0);
                if (!vals[vi].isNumber())
                    throw RuntimeError("bytes.pack: values must be numbers", 0);

                if (f.type == 'f') {
                    float fv = static_cast<float>(vals[vi].asNumber());
                    uint32_t bits; std::memcpy(&bits, &fv, 4);
                    for (int j = 0; j < 4; j++)
                        result += static_cast<char>((bits >> (big ? (24 - j*8) : j*8)) & 0xFF);
                    vi++; continue;
                }
                if (f.type == 'd') {
                    double dv = vals[vi].asNumber();
                    uint64_t bits; std::memcpy(&bits, &dv, 8);
                    for (int j = 0; j < 8; j++)
                        result += static_cast<char>((bits >> (big ? (56 - j*8) : j*8)) & 0xFF);
                    vi++; continue;
                }

                int64_t n = vals[vi].isInt() ? vals[vi].asInt()
                                             : static_cast<int64_t>(vals[vi].asNumber());
                for (int j = 0; j < f.size; j++)
                    result += static_cast<char>((n >> (big ? ((f.size-1-j)*8) : (j*8))) & 0xFF);
                vi++;
            }
            return Value(std::move(result));
        }));

    // bytes.unpack(format, data) — returns array of numbers
    bytesMap->entries[Value("unpack")] = Value(makeNative("bytes.unpack", 2,
        [parseStructFmt](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("bytes.unpack(format, data) requires strings", 0);
            auto& fmt = args[0].asString();
            auto& data = args[1].asString();
            auto result = gcNew<PraiaArray>();

            bool big;
            auto fields = parseStructFmt(fmt, big);
            size_t pos = 0;
            auto b = [&](size_t i) -> uint8_t { return static_cast<uint8_t>(data[pos + i]); };

            for (auto& f : fields) {
                if (pos + f.size > data.size())
                    throw RuntimeError("bytes.unpack: data too short for format", 0);
                if (f.type == 'x') { pos += 1; continue; }

                if (f.type == 'f') {
                    uint32_t bits = 0;
                    for (int j = 0; j < 4; j++)
                        bits |= static_cast<uint32_t>(b(big ? (3-j) : j)) << (j*8);
                    float fv; std::memcpy(&fv, &bits, 4);
                    result->elements.push_back(Value(static_cast<double>(fv)));
                    pos += 4; continue;
                }
                if (f.type == 'd') {
                    uint64_t bits = 0;
                    for (int j = 0; j < 8; j++)
                        bits |= static_cast<uint64_t>(b(big ? (7-j) : j)) << (j*8);
                    double dv; std::memcpy(&dv, &bits, 8);
                    result->elements.push_back(Value(dv));
                    pos += 8; continue;
                }

                // Integer types
                uint64_t raw = 0;
                for (int j = 0; j < f.size; j++)
                    raw |= static_cast<uint64_t>(b(big ? (f.size-1-j) : j)) << (j*8);

                int64_t val;
                bool isSigned = (f.type >= 'a' && f.type <= 'z'); // b,h,i,q are signed
                if (isSigned) {
                    int bits = f.size * 8;
                    if (bits < 64 && (raw & (1ULL << (bits - 1))))
                        val = static_cast<int64_t>(raw | (~0ULL << bits));
                    else
                        val = static_cast<int64_t>(raw);
                } else {
                    val = static_cast<int64_t>(raw);
                }
                result->elements.push_back(Value(val));
                pos += f.size;
            }
            return Value(result);
        }));

    // bytes.calcsize(format) — return total byte size of a struct format
    bytesMap->entries[Value("calcsize")] = Value(makeNative("bytes.calcsize", 1,
        [parseStructFmt](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("bytes.calcsize() requires a format string", 0);
            bool big;
            auto fields = parseStructFmt(args[0].asString(), big);
            int64_t total = 0;
            for (auto& f : fields) total += f.size;
            return Value(total);
        }));

    // bytes.from([72, 101, 108]) → "Hel" — array of byte values to string
    bytesMap->entries[Value("from")] = Value(makeNative("bytes.from", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("bytes.from() requires an array of numbers", 0);
            std::string result;
            for (auto& v : args[0].asArray()->elements) {
                if (!v.isNumber())
                    throw RuntimeError("bytes.from() array must contain numbers", 0);
                result += static_cast<char>(static_cast<int>(v.asNumber()) & 0xFF);
            }
            return Value(std::move(result));
        }));

    // bytes.toArray("Hel") → [72, 101, 108] — string to array of byte values
    bytesMap->entries[Value("toArray")] = Value(makeNative("bytes.toArray", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("bytes.toArray() requires a string", 0);
            auto result = gcNew<PraiaArray>();
            for (unsigned char c : args[0].asString())
                result->elements.push_back(Value(static_cast<int64_t>(c)));
            return Value(result);
        }));

    // bytes.hex("AB") → "4142" — string to hex representation
    bytesMap->entries[Value("hex")] = Value(makeNative("bytes.hex", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("bytes.hex() requires a string", 0);
            static const char* digits = "0123456789abcdef";
            std::string result;
            for (unsigned char c : args[0].asString()) {
                result += digits[c >> 4];
                result += digits[c & 0xF];
            }
            return Value(std::move(result));
        }));

    // bytes.fromHex("4142") → "AB" — hex string to raw bytes
    bytesMap->entries[Value("fromHex")] = Value(makeNative("bytes.fromHex", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("bytes.fromHex() requires a string", 0);
            auto& hex = args[0].asString();
            if (hex.size() % 2 != 0)
                throw RuntimeError("bytes.fromHex() requires even-length hex string", 0);
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                throw RuntimeError(std::string("bytes.fromHex() invalid hex character: '") + c + "'", 0);
            };
            std::string result;
            for (size_t i = 0; i < hex.size(); i += 2)
                result += static_cast<char>((hexVal(hex[i]) << 4) | hexVal(hex[i + 1]));
            return Value(std::move(result));
        }));

    // bytes.len(str) — byte length (same as len() but semantically clear for binary data)
    bytesMap->entries[Value("len")] = Value(makeNative("bytes.len", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("bytes.len() requires a string", 0);
            return Value(static_cast<int64_t>(args[0].asString().size()));
        }));

    // bytes.slice(s, start, end?) — byte-indexed substring (string is treated as raw bytes)
    // Negative indices count from the end. Useful when a string holds binary data
    // (e.g. multipart bodies) and grapheme-based slice() would corrupt it.
    bytesMap->entries[Value("slice")] = Value(makeNative("bytes.slice", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("bytes.slice(s, start, end?) requires a string", 0);
            if (args.size() < 2 || !args[1].isNumber())
                throw RuntimeError("bytes.slice() requires a start index", 0);
            auto& s = args[0].asString();
            int slen = static_cast<int>(s.size());
            int start = static_cast<int>(args[1].asNumber());
            if (start < 0) start += slen;
            if (start < 0) start = 0;
            if (start >= slen) return Value(std::string(""));
            int end = slen;
            if (args.size() > 2 && args[2].isNumber()) {
                end = static_cast<int>(args[2].asNumber());
                if (end < 0) end += slen;
                if (end <= start) return Value(std::string(""));
                if (end > slen) end = slen;
            }
            return Value(s.substr(start, end - start));
        }));

    // bytes.split(s, sep) — byte-indexed split. The string method works on bytes
    // already, but mirroring it here keeps "treat as bytes" intent explicit.
    bytesMap->entries[Value("split")] = Value(makeNative("bytes.split", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("bytes.split(s, sep) requires strings", 0);
            auto& s = args[0].asString();
            auto& sep = args[1].asString();
            auto arr = gcNew<PraiaArray>();
            if (sep.empty()) {
                for (char c : s) arr->elements.push_back(Value(std::string(1, c)));
                return Value(arr);
            }
            size_t pos = 0, found;
            while ((found = s.find(sep, pos)) != std::string::npos) {
                arr->elements.push_back(Value(s.substr(pos, found - pos)));
                pos = found + sep.size();
            }
            arr->elements.push_back(Value(s.substr(pos)));
            return Value(arr);
        }));

    // bytes.startsWith / bytes.endsWith — byte-prefix/suffix tests.
    bytesMap->entries[Value("startsWith")] = Value(makeNative("bytes.startsWith", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("bytes.startsWith(s, prefix) requires strings", 0);
            auto& s = args[0].asString();
            auto& p = args[1].asString();
            return Value(s.size() >= p.size() && s.compare(0, p.size(), p) == 0);
        }));
    bytesMap->entries[Value("endsWith")] = Value(makeNative("bytes.endsWith", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("bytes.endsWith(s, suffix) requires strings", 0);
            auto& s = args[0].asString();
            auto& p = args[1].asString();
            return Value(s.size() >= p.size() &&
                         s.compare(s.size() - p.size(), p.size(), p) == 0);
        }));

    // bytes.indexOf(s, sub, startByte?) — byte-indexed find. Returns byte offset or -1.
    bytesMap->entries[Value("indexOf")] = Value(makeNative("bytes.indexOf", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].isString() || !args[1].isString())
                throw RuntimeError("bytes.indexOf(s, sub, start?) requires strings", 0);
            auto& s = args[0].asString();
            auto& sub = args[1].asString();
            size_t startByte = 0;
            if (args.size() > 2 && args[2].isNumber()) {
                int sb = static_cast<int>(args[2].asNumber());
                if (sb < 0) sb = 0;
                startByte = static_cast<size_t>(sb);
            }
            auto pos = s.find(sub, startByte);
            if (pos == std::string::npos) return Value(static_cast<int64_t>(-1));
            return Value(static_cast<int64_t>(pos));
        }));

    // bytes.xor(data, mask) — XOR data with a repeating mask. Both are raw byte strings.
    bytesMap->entries[Value("xor")] = Value(makeNative("bytes.xor", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("bytes.xor() requires two strings", 0);
            auto& data = args[0].asString();
            auto& mask = args[1].asString();
            if (mask.empty()) return Value(data);
            std::string result(data.size(), '\0');
            size_t maskLen = mask.size();
            for (size_t i = 0; i < data.size(); i++)
                result[i] = data[i] ^ mask[i % maskLen];
            return Value(std::move(result));
        }));

}
