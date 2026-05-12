#pragma once
// Charset conversions between UTF-8 strings (Praia's native form) and
// bytes in a target encoding. Supports a small set of encodings
// chosen to cover legacy-Western interop without taking on libiconv
// as a dependency:
//
//   utf-8          identity (with validation)
//   utf-16le       LE byte order, surrogate pairs for non-BMP
//   utf-16be       BE byte order
//   latin-1        ISO-8859-1; codepoints U+0000..U+00FF map 1:1 to bytes
//   ascii          codepoints U+0000..U+007F only
//
// Encoding names are normalized case-insensitively and ignore '-' and
// '_', so "utf-8", "UTF8", "Utf_8" all resolve to the same encoder.
// Aliases: "iso-8859-1" == "latin-1".
//
// Asian legacy encodings (Shift-JIS, GBK, EUC-KR, Big5) are out of
// scope here — those need either a table-based decoder or libiconv.

#include <string>
#include <stdexcept>

namespace praia::encoding {

class EncodingError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Encode a UTF-8 string into bytes in the target encoding.
// Throws EncodingError for unencodable codepoints (e.g. U+0100 in
// latin-1), unknown encoding names, or invalid input UTF-8.
std::string encode(const std::string& utf8In, const std::string& encoding);

// Decode bytes in the source encoding into a UTF-8 string.
// Throws EncodingError for invalid byte sequences (truncated UTF-16
// pairs, lone surrogates, non-ASCII bytes in ascii mode, etc.) or
// unknown encoding names.
std::string decode(const std::string& bytesIn, const std::string& encoding);

}  // namespace praia::encoding
