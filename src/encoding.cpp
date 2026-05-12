#include "encoding.h"
#include "unicode.h"
#include <cctype>
#include <functional>
#include <sstream>

namespace praia::encoding {

namespace {

// Lowercase + strip '-' and '_'. "UTF-8", "utf8", "Utf_8" all collapse
// to the same canonical form, which keeps the encoding-name match a
// single string compare.
std::string canonicalName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '-' || c == '_') continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Format a codepoint as U+XXXX (or U+XXXXXX for supplementary planes)
// for error messages.
std::string formatCp(int32_t cp) {
    std::ostringstream s;
    s << "U+";
    s << std::hex << std::uppercase;
    if (cp <= 0xFFFF) s.width(4);
    else              s.width(6);
    s.fill('0');
    s << cp;
    return s.str();
}

std::string formatByte(unsigned char b) {
    std::ostringstream s;
    s << "0x" << std::hex << std::uppercase;
    s.width(2);
    s.fill('0');
    s << static_cast<int>(b);
    return s.str();
}

// Walk a UTF-8 byte sequence, calling `fn` for each decoded codepoint.
// Throws EncodingError on the first invalid byte — we don't try to
// repair or substitute; encoding APIs that "succeed" on garbage
// produce worse downstream bugs than ones that fail loud.
void walkUtf8(const std::string& s, std::function<void(int32_t)> fn) {
    const unsigned char* d = reinterpret_cast<const unsigned char*>(s.data());
    size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        unsigned char b = d[i];
        int32_t cp;
        size_t need;
        if      ((b & 0x80) == 0x00) { cp = b;         need = 0; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F;  need = 1; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F;  need = 2; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07;  need = 3; }
        else {
            throw EncodingError("invalid UTF-8 lead byte " + formatByte(b) +
                                " at offset " + std::to_string(i));
        }
        if (i + need >= n)
            throw EncodingError("truncated UTF-8 sequence at offset " +
                                std::to_string(i));
        for (size_t k = 1; k <= need; ++k) {
            unsigned char c = d[i + k];
            if ((c & 0xC0) != 0x80)
                throw EncodingError("invalid UTF-8 continuation byte " +
                                    formatByte(c) + " at offset " +
                                    std::to_string(i + k));
            cp = (cp << 6) | (c & 0x3F);
        }
        // Reject overlong encodings and surrogate halves — both are
        // disallowed in well-formed UTF-8 (RFC 3629).
        if ((need == 1 && cp < 0x80) ||
            (need == 2 && cp < 0x800) ||
            (need == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) ||
            cp > 0x10FFFF) {
            throw EncodingError("invalid UTF-8 sequence at offset " +
                                std::to_string(i) +
                                " (overlong or surrogate)");
        }
        fn(cp);
        i += need + 1;
    }
}

}  // namespace

std::string encode(const std::string& utf8In, const std::string& encoding) {
    const std::string canon = canonicalName(encoding);

    if (canon == "utf8") {
        // Validate the input UTF-8 by walking it. We don't just return
        // the bytes as-is because callers expect encode() to FAIL on
        // garbage input rather than silently pass it through.
        std::string out;
        out.reserve(utf8In.size());
        walkUtf8(utf8In, [&](int32_t cp) { out += utf8_encode(cp); });
        return out;
    }

    if (canon == "ascii") {
        std::string out;
        walkUtf8(utf8In, [&](int32_t cp) {
            if (cp >= 0x80)
                throw EncodingError("codepoint " + formatCp(cp) +
                                    " is not encodable in ASCII");
            out += static_cast<char>(cp);
        });
        return out;
    }

    if (canon == "latin1" || canon == "iso88591") {
        std::string out;
        walkUtf8(utf8In, [&](int32_t cp) {
            if (cp > 0xFF)
                throw EncodingError("codepoint " + formatCp(cp) +
                                    " is not encodable in latin-1");
            out += static_cast<char>(cp);
        });
        return out;
    }

    if (canon == "utf16le" || canon == "utf16be") {
        const bool le = (canon == "utf16le");
        std::string out;
        auto emit = [&](uint16_t unit) {
            if (le) {
                out += static_cast<char>(unit & 0xFF);
                out += static_cast<char>((unit >> 8) & 0xFF);
            } else {
                out += static_cast<char>((unit >> 8) & 0xFF);
                out += static_cast<char>(unit & 0xFF);
            }
        };
        walkUtf8(utf8In, [&](int32_t cp) {
            if (cp < 0x10000) {
                emit(static_cast<uint16_t>(cp));
            } else {
                // BMP overflow → surrogate pair (RFC 2781 §2.1).
                int32_t adj = cp - 0x10000;
                emit(static_cast<uint16_t>(0xD800 + (adj >> 10)));
                emit(static_cast<uint16_t>(0xDC00 + (adj & 0x3FF)));
            }
        });
        return out;
    }

    throw EncodingError("unknown encoding \"" + encoding +
                        "\" (supported: utf-8, utf-16le, utf-16be, latin-1, ascii)");
}

std::string decode(const std::string& bytesIn, const std::string& encoding) {
    const std::string canon = canonicalName(encoding);

    if (canon == "utf8") {
        // Validate by walking; re-emit so we get a normalized output
        // (no overlongs, no surrogate halves) even from input that
        // round-trips at the byte level.
        std::string out;
        out.reserve(bytesIn.size());
        walkUtf8(bytesIn, [&](int32_t cp) { out += utf8_encode(cp); });
        return out;
    }

    if (canon == "ascii") {
        for (size_t i = 0; i < bytesIn.size(); ++i) {
            unsigned char b = static_cast<unsigned char>(bytesIn[i]);
            if (b >= 0x80)
                throw EncodingError("byte " + formatByte(b) + " at offset " +
                                    std::to_string(i) +
                                    " is not valid ASCII");
        }
        return bytesIn;
    }

    if (canon == "latin1" || canon == "iso88591") {
        // Each byte maps to the codepoint with the same numeric value
        // — this is true by design of ISO-8859-1 and the first 256
        // codepoints of Unicode.
        std::string out;
        out.reserve(bytesIn.size() * 2);
        for (unsigned char b : bytesIn) out += utf8_encode(b);
        return out;
    }

    if (canon == "utf16le" || canon == "utf16be") {
        const bool le = (canon == "utf16le");
        if (bytesIn.size() % 2 != 0)
            throw EncodingError("utf-16 byte length must be even (got " +
                                std::to_string(bytesIn.size()) + ")");
        std::string out;
        size_t i = 0;
        auto readUnit = [&]() -> uint16_t {
            unsigned char a = static_cast<unsigned char>(bytesIn[i]);
            unsigned char b = static_cast<unsigned char>(bytesIn[i + 1]);
            i += 2;
            return le ? static_cast<uint16_t>(a | (b << 8))
                      : static_cast<uint16_t>((a << 8) | b);
        };
        while (i < bytesIn.size()) {
            size_t unitOffset = i;
            uint16_t u = readUnit();
            int32_t cp;
            if (u >= 0xD800 && u <= 0xDBFF) {
                // High surrogate; must be followed by a low surrogate.
                if (i >= bytesIn.size())
                    throw EncodingError("truncated utf-16 surrogate pair at offset " +
                                        std::to_string(unitOffset));
                uint16_t low = readUnit();
                if (low < 0xDC00 || low > 0xDFFF)
                    throw EncodingError("invalid low surrogate in utf-16 at offset " +
                                        std::to_string(unitOffset + 2));
                cp = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
                // Lone low surrogate — RFC 2781 forbids this.
                throw EncodingError("unexpected low surrogate in utf-16 at offset " +
                                    std::to_string(unitOffset));
            } else {
                cp = u;
            }
            out += utf8_encode(cp);
        }
        return out;
    }

    throw EncodingError("unknown encoding \"" + encoding +
                        "\" (supported: utf-8, utf-16le, utf-16be, latin-1, ascii)");
}

}  // namespace praia::encoding
