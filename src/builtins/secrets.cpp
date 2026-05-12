#include "../builtins.h"
#include "../gc_heap.h"
#include "../unicode.h"
#include <cstdint>
#include <fstream>
#include <string>

#ifdef HAVE_OPENSSL
#include <openssl/rand.h>
#endif

// `secrets` is the canonical namespace for generating tokens and
// comparing them safely. It exists for the same reason Python's
// `secrets` module does: when users reach for `random.int(...)` to
// build a session ID, they get a predictable Mersenne Twister
// output and ship a vulnerability. Channeling them at "use secrets
// for that" with a friendly API is the cheapest fix.
//
// Every function here pulls from the OS CSPRNG (RAND_bytes →
// /dev/urandom on Linux/macOS, BCryptGenRandom on Windows). No
// Mersenne Twister, no time-based seeds.

namespace {

std::string csprngBytes(int count) {
    std::string out(static_cast<size_t>(count), '\0');
#ifdef HAVE_OPENSSL
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&out[0]), count) != 1)
        throw RuntimeError("secrets: RAND_bytes CSPRNG failure", 0);
#else
    // Fallback for builds without OpenSSL — read /dev/urandom directly.
    // Avoids a dependency on the crypto.cpp helper.
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom || !urandom.read(&out[0], count))
        throw RuntimeError("secrets: cannot read /dev/urandom", 0);
#endif
    return out;
}

std::string toHex(const std::string& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 0xF];
    }
    return out;
}

// RFC 4648 §5 URL-safe base64: '-_' replace '+/', no '=' padding.
std::string toUrlSafeB64(const std::string& bytes) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : bytes) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out += table[(val >> valb) & 0x3F];
            valb -= 6;
        }
    }
    if (valb > -6) out += table[((val << 8) >> (valb + 8)) & 0x3F];
    return out;
}

// Constant-time string compare. Walks the full longer-of-the-two
// length so that timing doesn't leak the length of the secret
// either. Length mismatch → false without short-circuiting.
bool constantTimeEqual(const std::string& a, const std::string& b) {
    size_t la = a.size(), lb = b.size();
    size_t n = la > lb ? la : lb;
    // Folding the length comparison into the OR keeps the entire
    // function branch-free on the data path.
    unsigned int diff = (la == lb) ? 0u : 1u;
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = i < la ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = i < lb ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned int>(ca ^ cb);
    }
    return diff == 0;
}

// Uniform random integer in [0, n). Rejection sampling so small n
// don't suffer modulo bias. Mask the random uint64 down to the
// smallest power-of-two range covering n, then retry until we land
// below n. Worst case retries: average 2, max unbounded but
// astronomically rare.
uint64_t uniformBelow(uint64_t n) {
    if (n == 0) throw RuntimeError("secrets.choice: sequence is empty", 0);
    uint64_t mask = 0;
    for (uint64_t v = n - 1; v != 0; v >>= 1) mask = (mask << 1) | 1u;
    while (true) {
        uint64_t r;
#ifdef HAVE_OPENSSL
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&r), sizeof(r)) != 1)
            throw RuntimeError("secrets: RAND_bytes CSPRNG failure", 0);
#else
        std::ifstream urandom("/dev/urandom", std::ios::binary);
        if (!urandom || !urandom.read(reinterpret_cast<char*>(&r), sizeof(r)))
            throw RuntimeError("secrets: cannot read /dev/urandom", 0);
#endif
        r &= mask;
        if (r < n) return r;
    }
}

int extractByteCount(const Value& v, const char* fnName) {
    if (!v.isNumber())
        throw RuntimeError(std::string(fnName) + ": byte count must be a number", 0);
    int64_t n = v.toInt64ForBitwise();
    if (n < 0 || n > 65536)
        throw RuntimeError(std::string(fnName) + ": byte count must be 0..65536 (got " +
                           std::to_string(n) + ")", 0);
    return static_cast<int>(n);
}

}  // namespace

void registerSecretsBuiltins(std::shared_ptr<PraiaMap> map) {
    // secrets.token(n) — n raw bytes from the OS CSPRNG. The result
    // is a Praia bytes-string; pass it to bytes.* or stash it into
    // a sealed cookie. Most apps want one of the encoded variants
    // (Hex / UrlSafe) instead of raw bytes.
    map->entries[Value("token")] = Value(makeNative("secrets.token", 1,
        [](const std::vector<Value>& args) -> Value {
            return Value(csprngBytes(extractByteCount(args[0], "secrets.token")));
        }));

    // secrets.tokenHex(n) — n random bytes as a 2n-character lower-
    // case hex string. The natural form for database PK columns and
    // log identifiers.
    map->entries[Value("tokenHex")] = Value(makeNative("secrets.tokenHex", 1,
        [](const std::vector<Value>& args) -> Value {
            return Value(toHex(csprngBytes(extractByteCount(args[0], "secrets.tokenHex"))));
        }));

    // secrets.tokenUrlSafe(n) — n random bytes as URL-safe base64,
    // no padding (RFC 4648 §5). Use for password-reset URLs,
    // unsubscribe links, and other tokens that will appear in URLs.
    map->entries[Value("tokenUrlSafe")] = Value(makeNative("secrets.tokenUrlSafe", 1,
        [](const std::vector<Value>& args) -> Value {
            return Value(toUrlSafeB64(csprngBytes(
                extractByteCount(args[0], "secrets.tokenUrlSafe"))));
        }));

    // secrets.compare(a, b) — constant-time string equality. Use
    // this when comparing secret material (HMAC tags, session IDs,
    // API keys) to avoid leaking the secret one byte at a time
    // through timing-side-channel attacks. Length mismatches return
    // false without revealing which input was longer.
    map->entries[Value("compare")] = Value(makeNative("secrets.compare", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("secrets.compare(a, b) requires two strings", 0);
            return Value(constantTimeEqual(args[0].asString(), args[1].asString()));
        }));

    // secrets.choice(seq) — uniformly random element from a
    // non-empty array or string. Uses CSPRNG + rejection sampling
    // so small sequences don't suffer modulo bias. For strings the
    // result is a single grapheme cluster, consistent with how
    // Praia indexes strings elsewhere.
    map->entries[Value("choice")] = Value(makeNative("secrets.choice", 1,
        [](const std::vector<Value>& args) -> Value {
            if (args[0].isArray()) {
                auto& v = args[0].asArray()->elements;
                if (v.empty())
                    throw RuntimeError("secrets.choice: array is empty", 0);
                return v[uniformBelow(v.size())];
            }
            if (args[0].isString()) {
                const auto& s = args[0].asString();
                if (s.empty())
                    throw RuntimeError("secrets.choice: string is empty", 0);
#ifdef HAVE_UTF8PROC
                auto gs = utf8_graphemes(s);
                return Value(gs[uniformBelow(gs.size())]);
#else
                // Without utf8proc, fall back to a byte. Multi-byte
                // UTF-8 sequences will yield garbage; that's the
                // degraded-mode cost.
                return Value(std::string(1, s[uniformBelow(s.size())]));
#endif
            }
            throw RuntimeError("secrets.choice: argument must be an array or string", 0);
        }));
}
