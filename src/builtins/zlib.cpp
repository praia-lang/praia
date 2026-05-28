#include "../builtins.h"
#include "../gc_heap.h"
#include <cmath>
#include <cstring>
#include <limits>
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

// Default output cap for inflate/gunzip — 64 MiB. A few hundred bytes
// of gzip can expand to gigabytes (zip-bomb), so the inverse direction
// is the dangerous one. Callers can pass a higher (or lower) maxOutput
// argument to override; passing 0 / nil keeps the default.
constexpr size_t INFLATE_DEFAULT_MAX = 64ull * 1024 * 1024;

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

std::string doInflate(const std::string& input, int windowBits, size_t maxOutput) {
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
        size_t produced = CHUNK - zs.avail_out;
        if (out.size() + produced > maxOutput) {
            inflateEnd(&zs);
            throw RuntimeError("zlib: inflate output exceeded maxOutput (" +
                               std::to_string(maxOutput) + " bytes) — possible "
                               "zip bomb; pass a higher maxOutput if intentional",
                               0);
        }
        out.append(buf, produced);
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

size_t extractMaxOutput(const std::vector<Value>& args, size_t idx) {
    if (args.size() <= idx) return INFLATE_DEFAULT_MAX;
    if (args[idx].isNil())  return INFLATE_DEFAULT_MAX;
    if (!args[idx].isNumber())
        throw RuntimeError("zlib: maxOutput must be a number of bytes (or nil for default)", 0);
    double d = args[idx].asNumber();
    // NaN-first: all NaN comparisons are false, so a NaN would slip
    // past `d <= 0` and `d >= SIZE_MAX`, reaching the static_cast<size_t>
    // below, which is undefined behavior in C++.
    if (!std::isfinite(d))
        throw RuntimeError(
            "zlib: maxOutput must be a finite positive number (or nil for default)", 0);
    if (d <= 0)
        throw RuntimeError("zlib: maxOutput must be positive (got " +
                           std::to_string(static_cast<long long>(d)) + ")", 0);
    // Clamp obviously-out-of-range values to size_t max — anything past
    // that would have to be truncated anyway.
    if (d >= static_cast<double>(std::numeric_limits<size_t>::max()))
        return std::numeric_limits<size_t>::max();
    return static_cast<size_t>(d);
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

    // zlib.gunzip(bytes, maxOutput?) — inverse of gzip. maxOutput caps the
    // decompressed size (defaults to 64 MiB) to stop zip-bombs. Pass a
    // higher value when handling known-large archives; pass nil to use
    // the default explicitly.
    zlibMap->entries[Value("gunzip")] = Value(makeNative("zlib.gunzip", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("zlib.gunzip(bytes, maxOutput?) requires bytes (a string)", 0);
            size_t maxOut = extractMaxOutput(args, 1);
            return Value(doInflate(args[0].asString(), 15 + 16, maxOut));
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

    // zlib.inflate(bytes, maxOutput?) — inverse of deflate. See gunzip
    // for maxOutput semantics; the cap is the same default 64 MiB.
    zlibMap->entries[Value("inflate")] = Value(makeNative("zlib.inflate", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("zlib.inflate(bytes, maxOutput?) requires bytes (a string)", 0);
            size_t maxOut = extractMaxOutput(args, 1);
            return Value(doInflate(args[0].asString(), -15, maxOut));
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
