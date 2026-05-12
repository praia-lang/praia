#include "../builtins.h"
#include "../gc_heap.h"
#include <cstring>
#include <string>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

// One-shot gzip and raw-deflate via zlib. Two pairs of inverses:
//
//   zlib.gzip(b, level?)    →  RFC 1952 (with 10-byte header + crc32 + isize)
//   zlib.gunzip(b)          →  inverse of gzip
//
//   zlib.deflate(b, level?) →  RFC 1951 raw deflate (no header, no checksum)
//   zlib.inflate(b)         →  inverse of deflate
//
// Use gzip for HTTP `Content-Encoding: gzip`, `.gz` files, log-rotation
// output. Use raw deflate when interoperating with a protocol that
// strips its own framing (HTTP `Content-Encoding: deflate` in practice
// is the zlib format, not raw deflate — yes, the name is wrong).
//
// Streaming variants would be useful for huge inputs but require a
// stateful object with update() / finish() methods. Deferred — most
// real-world payloads (HTTP bodies, JSON logs, gzipped config) fit
// the one-shot API just fine.

namespace {

#ifdef HAVE_ZLIB

// windowBits selects the wire format:
//   -15           raw deflate (RFC 1951)
//    15           zlib format (RFC 1950, 2-byte header + adler32)
//    15 + 16 = 31 gzip format (RFC 1952)
//
// inflateInit2 accepts the same -15 / 15 / 31 set plus 32 for
// "auto-detect zlib or gzip", which we don't expose at the language
// level — let callers be explicit.
constexpr int CHUNK = 16384;

int validateLevel(int level) {
    if (level < 0 || level > 9)
        throw RuntimeError("zlib: compression level must be 0..9 (got " +
                           std::to_string(level) + ")", 0);
    return level;
}

std::string doDeflate(const std::string& input, int windowBits, int level) {
    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw RuntimeError("zlib: deflateInit2 failed", 0);
    // const_cast is fine — zlib's next_in is read-only despite the
    // non-const type; the header declares it for legacy reasons.
    zs.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    std::string out;
    out.reserve(input.size() / 2 + 64);
    char buf[CHUNK];
    int ret;
    do {
        zs.next_out  = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = CHUNK;
        ret = deflate(&zs, Z_FINISH);
        if (ret == Z_STREAM_ERROR) {
            deflateEnd(&zs);
            throw RuntimeError("zlib: deflate stream error", 0);
        }
        out.append(buf, CHUNK - zs.avail_out);
    } while (ret != Z_STREAM_END);
    deflateEnd(&zs);
    return out;
}

std::string doInflate(const std::string& input, int windowBits) {
    z_stream zs{};
    if (inflateInit2(&zs, windowBits) != Z_OK)
        throw RuntimeError("zlib: inflateInit2 failed", 0);
    zs.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    std::string out;
    char buf[CHUNK];
    int ret;
    do {
        zs.next_out  = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = CHUNK;
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            std::string msg = zs.msg ? zs.msg : "(no detail from zlib)";
            inflateEnd(&zs);
            throw RuntimeError("zlib: inflate failed: " + msg, 0);
        }
        out.append(buf, CHUNK - zs.avail_out);
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

int extractLevel(const std::vector<Value>& args, size_t idx) {
    if (args.size() <= idx) return Z_DEFAULT_COMPRESSION;  // -1 → zlib picks 6
    if (!args[idx].isNumber())
        throw RuntimeError("zlib: optional level argument must be a number 0..9", 0);
    return validateLevel(static_cast<int>(args[idx].toInt64ForBitwise()));
}

#endif  // HAVE_ZLIB

}  // namespace

void registerZlibBuiltins(std::shared_ptr<PraiaMap> zlibMap) {
#ifdef HAVE_ZLIB
    // zlib.gzip(bytes, level?) — RFC 1952 gzip-framed output. Level
    // is 0..9 (0 = no compression, 1 = fastest, 9 = best); the zlib
    // default of 6 is chosen when omitted, which is a good balance
    // of ratio and CPU for typical text payloads.
    zlibMap->entries[Value("gzip")] = Value(makeNative("zlib.gzip", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("zlib.gzip(bytes, level?) requires bytes (a string)", 0);
            int level = extractLevel(args, 1);
            return Value(doDeflate(args[0].asString(), 15 + 16, level));
        }));

    zlibMap->entries[Value("gunzip")] = Value(makeNative("zlib.gunzip", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("zlib.gunzip(bytes) requires bytes (a string)", 0);
            return Value(doInflate(args[0].asString(), 15 + 16));
        }));

    // zlib.deflate(bytes, level?) — RAW deflate (RFC 1951). No header,
    // no checksum, no framing. Use this when the surrounding protocol
    // already frames the compressed payload; use gzip otherwise.
    zlibMap->entries[Value("deflate")] = Value(makeNative("zlib.deflate", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("zlib.deflate(bytes, level?) requires bytes (a string)", 0);
            int level = extractLevel(args, 1);
            return Value(doDeflate(args[0].asString(), -15, level));
        }));

    zlibMap->entries[Value("inflate")] = Value(makeNative("zlib.inflate", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("zlib.inflate(bytes) requires bytes (a string)", 0);
            return Value(doInflate(args[0].asString(), -15));
        }));
#else
    // No zlib at link time → every entry throws so callers see a clear
    // signal at the call site rather than mysterious "undefined name"
    // errors at parse time.
    auto needZlib = [](const std::string& fnName) {
        return Value(makeNative(fnName, -1,
            [fnName](const std::vector<Value>&) -> Value {
                throw RuntimeError(fnName + " requires zlib (rebuild with HAVE_ZLIB)", 0);
            }));
    };
    zlibMap->entries[Value("gzip")]    = needZlib("zlib.gzip");
    zlibMap->entries[Value("gunzip")]  = needZlib("zlib.gunzip");
    zlibMap->entries[Value("deflate")] = needZlib("zlib.deflate");
    zlibMap->entries[Value("inflate")] = needZlib("zlib.inflate");
#endif
}
