#include "../builtins.h"
#include "../gc_heap.h"
#include "scope_guards.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>
#include <openssl/kdf.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/bn.h>
#include <arpa/inet.h>
#endif

// ── Helpers ──

static std::string toHexString(const uint8_t* data, size_t len) {
    std::string hex;
    hex.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", data[i]);
        hex += buf;
    }
    return hex;
}

// ── SHA-1 (hand-rolled, RFC 3174) ──

static std::string sha1_hash(const std::string& input) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    auto rotl = [](uint32_t x, int n) -> uint32_t { return (x << n) | (x >> (32 - n)); };

    std::string msg = input;
    uint64_t origLen = msg.size() * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 64 != 56) msg += '\0';
    for (int i = 7; i >= 0; i--) msg += static_cast<char>((origLen >> (i * 8)) & 0xFF);

    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (static_cast<uint8_t>(msg[offset+i*4]) << 24) |
                   (static_cast<uint8_t>(msg[offset+i*4+1]) << 16) |
                   (static_cast<uint8_t>(msg[offset+i*4+2]) << 8) |
                    static_cast<uint8_t>(msg[offset+i*4+3]);
        for (int i = 16; i < 80; i++)
            w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;          k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;          k = 0xCA62C1D6; }
            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    uint8_t digest[20];
    for (int i = 0; i < 4; i++) {
        digest[i]    = (h0 >> (24 - i*8)) & 0xFF;
        digest[4+i]  = (h1 >> (24 - i*8)) & 0xFF;
        digest[8+i]  = (h2 >> (24 - i*8)) & 0xFF;
        digest[12+i] = (h3 >> (24 - i*8)) & 0xFF;
        digest[16+i] = (h4 >> (24 - i*8)) & 0xFF;
    }
    return toHexString(digest, 20);
}

// ── SHA-512 (hand-rolled, FIPS 180-4) ──

static std::string sha512_hash(const std::string& input) {
    static const uint64_t K[80] = {
        0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
        0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
        0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
        0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
    };
    auto rotr64 = [](uint64_t x, int n) -> uint64_t { return (x >> n) | (x << (64 - n)); };

    uint64_t h[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
    };

    // Padding (128-byte blocks, 16-byte length field — upper 8 bytes zero for practical inputs)
    std::string msg = input;
    uint64_t origLen = msg.size() * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 128 != 112) msg += '\0';
    for (int i = 0; i < 8; i++) msg += '\0'; // upper 8 bytes of 128-bit length
    for (int i = 7; i >= 0; i--) msg += static_cast<char>((origLen >> (i * 8)) & 0xFF);

    for (size_t offset = 0; offset < msg.size(); offset += 128) {
        uint64_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = 0;
            for (int j = 0; j < 8; j++)
                w[i] = (w[i] << 8) | static_cast<uint8_t>(msg[offset + i*8 + j]);
        }
        for (int i = 16; i < 80; i++) {
            uint64_t s0 = rotr64(w[i-15],1) ^ rotr64(w[i-15],8) ^ (w[i-15]>>7);
            uint64_t s1 = rotr64(w[i-2],19) ^ rotr64(w[i-2],61) ^ (w[i-2]>>6);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }

        uint64_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 80; i++) {
            uint64_t S1 = rotr64(e,14) ^ rotr64(e,18) ^ rotr64(e,41);
            uint64_t ch = (e & f) ^ (~e & g);
            uint64_t t1 = hh + S1 + ch + K[i] + w[i];
            uint64_t S0 = rotr64(a,28) ^ rotr64(a,34) ^ rotr64(a,39);
            uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint64_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    uint8_t digest[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            digest[i*8+j] = (h[i] >> (56 - j*8)) & 0xFF;
    return toHexString(digest, 64);
}

// ── SHA-256 (shared helper, returns raw 32-byte digest) ──

static std::string sha256_raw(const std::string& input) {
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    auto rotr = [](uint32_t x, int n) -> uint32_t { return (x >> n) | (x << (32 - n)); };

    std::string msg = input;
    uint64_t origLen = msg.size() * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 64 != 56) msg += '\0';
    for (int i = 7; i >= 0; i--) msg += static_cast<char>((origLen >> (i * 8)) & 0xFF);

    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (static_cast<uint8_t>(msg[offset+i*4]) << 24) |
                   (static_cast<uint8_t>(msg[offset+i*4+1]) << 16) |
                   (static_cast<uint8_t>(msg[offset+i*4+2]) << 8) |
                    static_cast<uint8_t>(msg[offset+i*4+3]);
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15]>>3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    std::string raw(32, '\0');
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++)
            raw[i*4+j] = static_cast<char>((h[i] >> (24 - j*8)) & 0xFF);
    return raw;
}

static std::string sha256_hex(const std::string& input) {
    std::string raw = sha256_raw(input);
    return toHexString(reinterpret_cast<const uint8_t*>(raw.data()), 32);
}

// ── HMAC (RFC 2104, works with any hash function) ──

static std::string hmac_compute(const std::string& key, const std::string& msg,
                                 const std::string& algorithm) {
#ifdef HAVE_OPENSSL
    auto hmacOpenSSL = [&]() -> std::string {
        const EVP_MD* md = nullptr;
        if (algorithm == "sha256") md = EVP_sha256();
        else if (algorithm == "sha1") md = EVP_sha1();
        else if (algorithm == "sha512") md = EVP_sha512();
        else if (algorithm == "md5") md = EVP_md5();
        if (!md) return "";

        unsigned char result[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(md, key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
             result, &len);
        return toHexString(result, len);
    };
    return hmacOpenSSL();
#else
    // Hand-rolled HMAC with SHA-256 only (no OpenSSL)
    if (algorithm != "sha256")
        throw RuntimeError("crypto.hmac() requires OpenSSL for " + algorithm + " (only sha256 available without it)", 0);

    int blockSize = 64;
    int digestSize = 32;

    std::string k = key;
    if (static_cast<int>(k.size()) > blockSize)
        k = sha256_raw(k);
    while (static_cast<int>(k.size()) < blockSize)
        k += '\0';

    std::string ipad(blockSize, '\0'), opad(blockSize, '\0');
    for (int i = 0; i < blockSize; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    std::string innerHash = sha256_raw(ipad + msg);
    std::string outerHash = sha256_raw(opad + innerHash);
    return toHexString(reinterpret_cast<const uint8_t*>(outerHash.data()), digestSize);
#endif
}

// ── Random bytes ──

static std::string generateRandomBytes(int count) {
    std::string result(count, '\0');
#ifdef HAVE_OPENSSL
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&result[0]), count) != 1)
        throw RuntimeError("crypto.randomBytes() failed", 0);
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom || !urandom.read(&result[0], count))
        throw RuntimeError("crypto.randomBytes() failed: cannot read /dev/urandom", 0);
#endif
    return result;
}

// ── PHC string format (Argon2 / scrypt) ──────────────────────
//
// PHC encoding lets us return a single self-describing string
// rather than a structured map, so a stored hash carries its own
// algorithm + parameters. Format:
//
//   $<algo>$<param1=val1,param2=val2,...>$<base64(salt)>$<base64(hash)>
//
// Argon2 has an extra version segment between algo and params:
//
//   $argon2id$v=19$m=65536,t=3,p=4$<base64(salt)>$<base64(hash)>
//
// Base64 here is the standard alphabet (A-Za-z0-9+/) with no
// padding — the same form the argon2 reference and Python's passlib
// emit, so hashes round-trip with other tools' outputs.

#ifdef HAVE_OPENSSL

static std::string phcB64Encode(const std::string& bytes) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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

static std::string phcB64Decode(const std::string& in) {
    auto dec = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    // Canonical base64 lengths mod 4 are {0, 2, 3} — a single trailing
    // char encodes only 6 bits and can't form a byte. Without this
    // check, a 1-char segment like "A" decodes to "" silently, which
    // lets a malformed PHC ("$scrypt$...$saltb64$") with a junk-but-
    // non-empty hash segment derive zero bytes and ctEqual("", "")
    // any password.
    if (in.size() % 4 == 1)
        throw RuntimeError("PHC: non-canonical base64 length (" +
                           std::to_string(in.size()) + " chars)", 0);
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        // Praia emits unpadded PHC base64 (per the argon2-reference /
        // passlib convention). The earlier `if (c == '=') break;` was
        // a verification bypass: an attacker could splice an "=" into
        // the middle of the hash segment to truncate decoding, so
        // e.g. a 32-byte stored hash collapsed to its 16-byte prefix.
        // The verifier then derived 16 bytes (outLen = hash.size())
        // and matched the prefix — gifting the attacker a 2^128
        // brute-force advantage over honest verification. Reject any
        // '=' along with every other non-base64 character.
        int d = dec(c);
        if (d < 0)
            throw RuntimeError("PHC: invalid base64 character in salt or hash", 0);
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out += static_cast<char>((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return out;
}

// Split "k=v,k=v,..." into ordered name/value pairs. Order matters
// for some downstream consumers, so we keep a vector rather than a
// map. Throws on malformed (missing '=', empty key, etc.).
static std::vector<std::pair<std::string, std::string>>
phcParseKvSegment(const std::string& s) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t comma = s.find(',', i);
        size_t end = (comma == std::string::npos) ? s.size() : comma;
        size_t eq = s.find('=', i);
        if (eq == std::string::npos || eq >= end || eq == i)
            throw RuntimeError("PHC: malformed param segment near \"" +
                               s.substr(i, end - i) + "\"", 0);
        out.emplace_back(s.substr(i, eq - i), s.substr(eq + 1, end - eq - 1));
        i = (comma == std::string::npos) ? s.size() : comma + 1;
    }
    return out;
}

// Parse a PHC numeric param strictly: ASCII digits only (no sign, no
// whitespace, no base prefix, no trailing junk), and bounded to
// [minV, maxV]. The strictness matters for security — a lenient
// parser like plain `std::stoull` silently accepts "m=64xyz" as 64
// or "m=4294967360" as a value that wraps when narrowed to uint32_t,
// which lets an attacker forge a PHC string that verifies under a
// weaker cost factor than the one stored in the hash.
static uint64_t phcParseUint(const std::string& name,
                             const std::vector<std::pair<std::string, std::string>>& kvs,
                             uint64_t minV, uint64_t maxV) {
    // Locate *the* match. Duplicate keys are malformed input — silently
    // accepting the first occurrence lets an attacker bury a benign
    // value (e.g. m=8) next to a wrap-target (e.g. m=4294967360); we'd
    // verify under the first while the displayed PHC string carries
    // the second. Reject the whole string.
    const std::string* matched = nullptr;
    for (auto& [k, v] : kvs) {
        if (k != name) continue;
        if (matched != nullptr)
            throw RuntimeError("PHC: duplicate parameter '" + name + "'", 0);
        matched = &v;
    }
    if (!matched)
        throw RuntimeError("PHC: missing required parameter '" + name + "'", 0);
    const std::string& v = *matched;

    if (v.empty())
        throw RuntimeError("PHC: parameter '" + name + "' is empty", 0);
    for (char c : v) {
        if (c < '0' || c > '9')
            throw RuntimeError("PHC: parameter '" + name +
                               "' is not a base-10 integer: \"" + v + "\"", 0);
    }
    uint64_t n;
    try {
        n = std::stoull(v);
    } catch (...) {
        // std::out_of_range — value >= 2^64.
        throw RuntimeError("PHC: parameter '" + name +
                           "' is too large: \"" + v + "\"", 0);
    }
    if (n < minV || n > maxV)
        throw RuntimeError("PHC: parameter '" + name + "'=" + v +
                           " out of range [" + std::to_string(minV) +
                           ", " + std::to_string(maxV) + "]", 0);
    return n;
}

// Strict integer-in-[1, INT32_MAX] validator for the PBKDF2 iterations
// argument. The lenient `static_cast<int64_t>(asNumber())` pattern that
// used to live at the call sites had three latent bugs:
//   - 1.9 silently truncated to 1 (caller asked for >= 2 work, got
//     the absolute minimum).
//   - math.INF cast to int64 is undefined behavior — happened to throw
//     "< 1" on x86/arm because UB returns INT64_MIN, but a different
//     compiler/arch could just as easily produce a huge positive value
//     and run a near-infinite PBKDF2.
//   - Values above INT32_MAX got `std::min`'d down to INT32_MAX, so
//     `iterations: 1e10` silently ran ~2 billion HMAC rounds instead
//     of being rejected as nonsense. Effectively a self-DoS.
// This helper rejects all three with a clear message and a single
// integral-in-range result.
static int validatePbkdf2Iterations(const Value& v, const char* fnName) {
    if (v.isInt()) {
        int64_t n = v.asInt();
        if (n < 1 || n > INT32_MAX)
            throw RuntimeError(std::string(fnName) +
                               ": iterations must be in [1, " +
                               std::to_string(INT32_MAX) + "] (got " +
                               std::to_string(n) + ")", 0);
        return static_cast<int>(n);
    }
    // Double path. Reject NaN/Inf and any fractional value before the
    // range check — silently truncating 1.9 to 1 hides a caller bug.
    double d = v.asNumber();
    if (!std::isfinite(d))
        throw RuntimeError(std::string(fnName) +
                           ": iterations must be a finite number", 0);
    if (d != std::floor(d))
        throw RuntimeError(std::string(fnName) +
                           ": iterations must be an integer (got " +
                           std::to_string(d) + ")", 0);
    if (d < 1.0 || d > static_cast<double>(INT32_MAX))
        throw RuntimeError(std::string(fnName) +
                           ": iterations must be in [1, " +
                           std::to_string(INT32_MAX) + "] (got " +
                           std::to_string(d) + ")", 0);
    return static_cast<int>(d);
}

// Parse a PHC string into its components. The result keeps params as
// a kv-vector so the caller can pull out cost factors by name. Salt
// and hash come back decoded as bytes. Argon2's "v=19" segment is
// folded into the kv list under key "v" for uniformity.
struct PhcParts {
    std::string algo;
    std::vector<std::pair<std::string, std::string>> params;
    std::string salt;
    std::string hash;
};

static PhcParts phcParse(const std::string& s) {
    // Split on '$'. Argon2 has 5 non-empty segments, scrypt has 4.
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('$', i);
        if (j == std::string::npos) {
            parts.push_back(s.substr(i));
            break;
        }
        parts.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    // The leading '$' produces an empty first element.
    if (parts.empty() || !parts[0].empty())
        throw RuntimeError("PHC: must start with '$'", 0);
    parts.erase(parts.begin());

    PhcParts out;
    out.algo = parts[0];

    // Argon2 family (argon2i/argon2d/argon2id) per RFC 9106 §3.1 has
    // a mandatory version segment "$v=NN$" between algo and params —
    // exactly 5 segments. Everything else (scrypt and friends) has
    // no version segment — exactly 4 segments. An "argon2id" PHC
    // without v=NN is malformed; previously accepted at parse time
    // and silently fed to the derive with an implicit version, which
    // means two semantically different PHC strings could verify the
    // same password.
    bool isArgon2 = out.algo.rfind("argon2", 0) == 0;
    size_t expected = isArgon2 ? 5 : 4;
    if (parts.size() != expected)
        throw RuntimeError("PHC: '" + out.algo + "' expects " +
                           std::to_string(expected) + " segments, got " +
                           std::to_string(parts.size()), 0);

    size_t paramIdx = 1;
    if (isArgon2) {
        auto kv = phcParseKvSegment(parts[1]);
        if (kv.size() != 1 || kv[0].first != "v")
            throw RuntimeError("PHC: expected version segment 'v=NN' between algo and params", 0);
        // RFC 9106 defines v=0x13 (19) as the current Argon2 version.
        // Earlier v=0x10 (16) exists but isn't supported by modern
        // OpenSSL's ARGON2ID derive — reject rather than silently
        // hand off a value the KDF would either ignore or reinterpret.
        if (kv[0].second != "19")
            throw RuntimeError("PHC: unsupported argon2 version 'v=" +
                               kv[0].second + "' (only v=19 is supported)", 0);
        out.params.emplace_back("v", kv[0].second);
        paramIdx = 2;
    }
    auto kvParams = phcParseKvSegment(parts[paramIdx]);
    for (auto& kv : kvParams) out.params.push_back(std::move(kv));
    out.salt = phcB64Decode(parts[paramIdx + 1]);
    out.hash = phcB64Decode(parts[paramIdx + 2]);
    return out;
}

// Drive EVP_KDF with the assembled OSSL_PARAM array, returning
// `outLen` derived bytes. Centralizing this keeps the scrypt and
// argon2id paths from drifting on resource cleanup.
static std::string evpKdfDerive(const char* kdfName,
                                OSSL_PARAM* params, size_t outLen,
                                const char* fnNameForErr) {
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, kdfName, nullptr);
    if (!kdf) {
        // The Argon2 case is the practical reason this path exists:
        // OpenSSL < 3.2 doesn't ship ARGON2ID. Make the error point
        // at the version requirement so the user knows what to fix.
        std::string msg = fnNameForErr;
        msg += ": ";
        msg += kdfName;
        msg += " not available in this OpenSSL build";
        if (std::string(kdfName) == "ARGON2ID")
            msg += " (requires OpenSSL 3.2 or later)";
        throw RuntimeError(msg, 0);
    }
    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!ctx)
        throw RuntimeError(std::string(fnNameForErr) + ": EVP_KDF_CTX_new failed", 0);
    std::string out(outLen, '\0');
    int rc = EVP_KDF_derive(ctx, reinterpret_cast<unsigned char*>(&out[0]),
                            outLen, params);
    EVP_KDF_CTX_free(ctx);
    if (rc != 1)
        throw RuntimeError(std::string(fnNameForErr) +
                           ": EVP_KDF_derive failed (check parameter ranges)", 0);
    return out;
}

// Constant-time bytes equality. Identical pattern to the secrets
// namespace — duplicated rather than refactored to keep this file
// self-contained. Used to verify a fresh derivation against a stored
// hash without leaking partial-match information via timing.
static bool ctEqual(const std::string& a, const std::string& b) {
    size_t la = a.size(), lb = b.size();
    size_t n = la > lb ? la : lb;
    unsigned int diff = (la == lb) ? 0u : 1u;
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = i < la ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = i < lb ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned int>(ca ^ cb);
    }
    return diff == 0;
}

// Derive `outLen` bytes via scrypt with the given cost parameters.
// N must be a power of 2 ≥ 2; r and p ≥ 1. OpenSSL validates further
// (e.g. r*p < 2^30 per RFC 7914) and fails with a derive error.
static std::string scryptDerive(const std::string& pw, const std::string& salt,
                                uint64_t N, uint64_t r, uint64_t p, size_t outLen) {
    OSSL_PARAM params[7];
    int i = 0;
    params[i++] = OSSL_PARAM_construct_octet_string(
        OSSL_KDF_PARAM_PASSWORD,
        const_cast<void*>(static_cast<const void*>(pw.data())), pw.size());
    params[i++] = OSSL_PARAM_construct_octet_string(
        OSSL_KDF_PARAM_SALT,
        const_cast<void*>(static_cast<const void*>(salt.data())), salt.size());
    params[i++] = OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_N, &N);
    params[i++] = OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_R, &r);
    params[i++] = OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_P, &p);
    // OpenSSL caps scrypt's memory at ~32 MiB by default; raise it
    // so users can pick larger N without hitting an opaque internal
    // limit. The cap is a safety net against accidental DoS; 4 GiB
    // is plenty for any reasonable use.
    uint64_t maxMem = 4ULL << 30;  // 4 GiB
    params[i++] = OSSL_PARAM_construct_uint64("maxmem_bytes", &maxMem);
    params[i] = OSSL_PARAM_construct_end();
    return evpKdfDerive("SCRYPT", params, outLen, "crypto.scrypt");
}

// Derive `outLen` bytes via Argon2id. t = iterations (≥1), m = memory
// in KiB (must be ≥ 8 * lanes), p = lanes (≥1). OpenSSL throws a
// derive failure on out-of-range combinations.
static std::string argon2idDerive(const std::string& pw, const std::string& salt,
                                  uint32_t t, uint32_t m, uint32_t p, size_t outLen) {
    OSSL_PARAM params[6];
    int i = 0;
    params[i++] = OSSL_PARAM_construct_octet_string(
        OSSL_KDF_PARAM_PASSWORD,
        const_cast<void*>(static_cast<const void*>(pw.data())), pw.size());
    params[i++] = OSSL_PARAM_construct_octet_string(
        OSSL_KDF_PARAM_SALT,
        const_cast<void*>(static_cast<const void*>(salt.data())), salt.size());
    params[i++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &t);
    params[i++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &m);
    params[i++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &p);
    params[i] = OSSL_PARAM_construct_end();
    return evpKdfDerive("ARGON2ID", params, outLen, "crypto.argon2id");
}

// Optional-map helper — pulls an integer parameter out of an optional
// {...} map argument, with default + range check. Reused by both
// scrypt and argon2id entry points.
static uint64_t mapUintParam(const Value& maybeMap, const char* key,
                             uint64_t deflt, uint64_t minVal, uint64_t maxVal,
                             const char* fnName) {
    if (!maybeMap.isMap()) return deflt;
    auto& m = maybeMap.asMap()->entries;
    auto it = m.find(Value(std::string(key)));
    if (it == m.end()) return deflt;
    if (!it->second.isNumber())
        throw RuntimeError(std::string(fnName) + ": param '" + key +
                           "' must be a number", 0);
    int64_t v = it->second.toInt64ForBitwise();
    if (v < 0)
        throw RuntimeError(std::string(fnName) + ": param '" + key +
                           "' must be non-negative", 0);
    uint64_t u = static_cast<uint64_t>(v);
    if (u < minVal || u > maxVal)
        throw RuntimeError(std::string(fnName) + ": param '" + key +
                           "' must be in [" + std::to_string(minVal) + ", " +
                           std::to_string(maxVal) + "] (got " +
                           std::to_string(u) + ")", 0);
    return u;
}

static std::string mapStringParam(const Value& maybeMap, const char* key,
                                  const char* fnName) {
    if (!maybeMap.isMap()) return "";
    auto& m = maybeMap.asMap()->entries;
    auto it = m.find(Value(std::string(key)));
    if (it == m.end()) return "";
    if (!it->second.isString())
        throw RuntimeError(std::string(fnName) + ": param '" + key +
                           "' must be a string", 0);
    return it->second.asString();
}

// ── X509 certificate parsing ──────────────────────────────────
//
// Returns a Praia map with all the fields a non-crypto-expert
// caller usually wants: subject + issuer DNs (both as a structured
// map and OpenSSL's `/CN=foo/O=bar` one-liner), validity range as
// ISO 8601 strings, SubjectAltNames typed by kind (DNS/IP/email/URI),
// SHA-256 fingerprint, signature algorithm short name, CA flag,
// and the public key (PEM + algo type + bit count).
//
// Intentionally NOT exposed: full extension dump, AuthorityInfoAccess,
// CRL distribution points, CT SCTs. Adding any of these is mechanical
// once we have a request; the v1 surface targets the questions
// users actually ask of a cert (who, when, fingerprint, hosts).

static Value x509NameToMap(X509_NAME* name) {
    auto out = gcNew<PraiaMap>();
    int n = X509_NAME_entry_count(name);
    for (int i = 0; i < n; ++i) {
        X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, i);
        ASN1_OBJECT* obj = X509_NAME_ENTRY_get_object(entry);
        int nid = OBJ_obj2nid(obj);
        std::string keyName;
        // Short name (CN/O/OU/...) when OpenSSL knows it; OID text
        // otherwise so unusual fields don't disappear.
        const char* sn = (nid != NID_undef) ? OBJ_nid2sn(nid) : nullptr;
        if (sn) {
            keyName = sn;
        } else {
            char buf[128];
            OBJ_obj2txt(buf, sizeof(buf), obj, 1);
            keyName = buf;
        }
        ASN1_STRING* str = X509_NAME_ENTRY_get_data(entry);
        unsigned char* utf8 = nullptr;
        int len = ASN1_STRING_to_UTF8(&utf8, str);
        std::string val;
        if (len > 0 && utf8) val.assign(reinterpret_cast<char*>(utf8), len);
        if (utf8) OPENSSL_free(utf8);
        // Multi-valued RDNs (rare but legal): collapse to a
        // comma-separated string so each key still maps to a single
        // value. Most callers iterate keys() and treat the result as
        // a flat string lookup.
        Value k(keyName);
        auto it = out->entries.find(k);
        if (it != out->entries.end() && it->second.isString())
            it->second = Value(it->second.asString() + "," + val);
        else
            out->entries[k] = Value(val);
    }
    return Value(out);
}

static std::string x509NameToOneline(X509_NAME* name) {
    // X509_NAME_oneline with NULL buf returns a newly-allocated
    // string sized exactly to the content — no risk of truncation
    // for unusually-long subject DNs.
    char* line = X509_NAME_oneline(name, nullptr, 0);
    if (!line) return "";
    std::string out(line);
    OPENSSL_free(line);
    return out;
}

static std::string asn1TimeToIso(const ASN1_TIME* t) {
    if (!t) return "";
    struct tm tm;
    if (ASN1_TIME_to_tm(t, &tm) != 1) return "";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

static Value parseSans(X509* cert) {
    auto arr = gcNew<PraiaArray>();
    GENERAL_NAMES* gens = static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (!gens) return Value(arr);

    int n = sk_GENERAL_NAME_num(gens);
    for (int i = 0; i < n; ++i) {
        GENERAL_NAME* gen = sk_GENERAL_NAME_value(gens, i);
        std::string type, value;
        switch (gen->type) {
            case GEN_DNS: {
                type = "DNS";
                int len = ASN1_STRING_length(gen->d.dNSName);
                const unsigned char* data = ASN1_STRING_get0_data(gen->d.dNSName);
                if (data && len > 0) value.assign(reinterpret_cast<const char*>(data), len);
                break;
            }
            case GEN_EMAIL: {
                type = "email";
                int len = ASN1_STRING_length(gen->d.rfc822Name);
                const unsigned char* data = ASN1_STRING_get0_data(gen->d.rfc822Name);
                if (data && len > 0) value.assign(reinterpret_cast<const char*>(data), len);
                break;
            }
            case GEN_URI: {
                type = "URI";
                int len = ASN1_STRING_length(gen->d.uniformResourceIdentifier);
                const unsigned char* data = ASN1_STRING_get0_data(gen->d.uniformResourceIdentifier);
                if (data && len > 0) value.assign(reinterpret_cast<const char*>(data), len);
                break;
            }
            case GEN_IPADD: {
                type = "IP";
                int len = ASN1_STRING_length(gen->d.iPAddress);
                const unsigned char* data = ASN1_STRING_get0_data(gen->d.iPAddress);
                if (len == 4 && data) {
                    char buf[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, data, buf, sizeof(buf));
                    value = buf;
                } else if (len == 16 && data) {
                    char buf[INET6_ADDRSTRLEN];
                    inet_ntop(AF_INET6, data, buf, sizeof(buf));
                    value = buf;
                }
                break;
            }
            default:
                // directoryName, x400Address, ediPartyName, registeredID,
                // otherName — uncommon in real-world certs, skip.
                continue;
        }
        auto entry = gcNew<PraiaMap>();
        entry->entries[Value("type")]  = Value(type);
        entry->entries[Value("value")] = Value(value);
        arr->elements.push_back(Value(entry));
    }
    GENERAL_NAMES_free(gens);
    return Value(arr);
}

// Turn an X509* into the Praia map shape exposed to user code.
// Shared by parseCertificate (single) and parseCertificateChain.
// The X509* is borrowed — caller still owns it.
static Value x509ToValue(X509* cert) {
    auto out = gcNew<PraiaMap>();

    // version — internally 0-indexed (X.509 v3 is stored as 2).
    out->entries[Value("version")] = Value(
        static_cast<int64_t>(X509_get_version(cert) + 1));

    // serial — emitted as a hex string because real certs use
    // 128-bit serials that don't fit in int64.
    ASN1_INTEGER* serial = X509_get_serialNumber(cert);
    BIGNUM* bn = ASN1_INTEGER_to_BN(serial, nullptr);
    char* hex = BN_bn2hex(bn);
    if (hex) {
        // BN_bn2hex uppercases; lowercase for consistency with the
        // rest of the bytes.hex output in this stdlib.
        std::string s(hex);
        for (auto& c : s) if (c >= 'A' && c <= 'F') c = c - 'A' + 'a';
        out->entries[Value("serial")] = Value(s);
        OPENSSL_free(hex);
    } else {
        out->entries[Value("serial")] = Value(std::string(""));
    }
    BN_free(bn);

    out->entries[Value("subject")]       = x509NameToMap(X509_get_subject_name(cert));
    out->entries[Value("subjectString")] = Value(x509NameToOneline(X509_get_subject_name(cert)));
    out->entries[Value("issuer")]        = x509NameToMap(X509_get_issuer_name(cert));
    out->entries[Value("issuerString")]  = Value(x509NameToOneline(X509_get_issuer_name(cert)));

    out->entries[Value("notBefore")] = Value(asn1TimeToIso(X509_get0_notBefore(cert)));
    out->entries[Value("notAfter")]  = Value(asn1TimeToIso(X509_get0_notAfter(cert)));

    int sigNid = X509_get_signature_nid(cert);
    const char* sigName = OBJ_nid2ln(sigNid);
    out->entries[Value("sigAlg")] = Value(std::string(sigName ? sigName : "unknown"));

    out->entries[Value("sans")] = parseSans(cert);

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    if (X509_digest(cert, EVP_sha256(), digest, &digestLen) == 1) {
        out->entries[Value("fingerprintSha256")] = Value(toHexString(digest, digestLen));
    } else {
        out->entries[Value("fingerprintSha256")] = Value(std::string(""));
    }

    // X509_check_ca returns 0 = not CA, >0 = CA (various subtypes).
    out->entries[Value("isCA")] = Value(X509_check_ca(cert) > 0);

    // Public key — both as PEM (for re-use) and as a small info map
    // (so callers don't need to re-parse the PEM to learn the alg).
    EVP_PKEY* pk = X509_get0_pubkey(cert);
    if (pk) {
        BIO* bio = BIO_new(BIO_s_mem());
        if (bio) {
            PEM_write_bio_PUBKEY(bio, pk);
            BUF_MEM* mb = nullptr;
            BIO_get_mem_ptr(bio, &mb);
            if (mb && mb->data && mb->length > 0)
                out->entries[Value("publicKey")] =
                    Value(std::string(mb->data, mb->length));
            BIO_free(bio);
        }
        auto pkInfo = gcNew<PraiaMap>();
        int pkType = EVP_PKEY_id(pk);
        const char* typeName = OBJ_nid2sn(pkType);
        pkInfo->entries[Value("type")] = Value(std::string(typeName ? typeName : "unknown"));
        pkInfo->entries[Value("bits")] = Value(static_cast<int64_t>(EVP_PKEY_bits(pk)));
        out->entries[Value("publicKeyInfo")] = Value(pkInfo);
    }

    return Value(out);
}

// Sniff: PEM if we see "-----BEGIN" anywhere in the first 32 bytes,
// DER otherwise. A real ASN.1 SEQUENCE starts with 0x30, so a smarter
// sniffer could test that explicitly, but the PEM marker check is
// more discriminating since DER bytes can sometimes start with the
// same 0x30 in a PEM line.
static bool looksLikePem(const std::string& s) {
    size_t scanEnd = std::min(s.size(), static_cast<size_t>(64));
    return s.compare(0, scanEnd, "-----BEGIN", 0,
                     std::min(scanEnd, static_cast<size_t>(10))) == 0 ||
           s.find("-----BEGIN CERTIFICATE-----") != std::string::npos;
}

static Value parseCertificateImpl(const std::string& data) {
    if (data.empty())
        throw RuntimeError("crypto.parseCertificate: empty input", 0);

    X509* cert = nullptr;
    if (looksLikePem(data)) {
        BIO* bio = BIO_new_mem_buf(data.data(), static_cast<int>(data.size()));
        if (!bio)
            throw RuntimeError("crypto.parseCertificate: BIO_new_mem_buf failed", 0);
        cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!cert)
            throw RuntimeError("crypto.parseCertificate: failed to parse PEM "
                               "(check the BEGIN/END markers and base64 body)", 0);
    } else {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data());
        cert = d2i_X509(nullptr, &p, static_cast<long>(data.size()));
        if (!cert)
            throw RuntimeError("crypto.parseCertificate: failed to parse DER", 0);
    }
    Value out = x509ToValue(cert);
    X509_free(cert);
    return out;
}

static Value parseCertificateChainImpl(const std::string& pem) {
    auto arr = gcNew<PraiaArray>();
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio)
        throw RuntimeError("crypto.parseCertificateChain: BIO_new_mem_buf failed", 0);
    while (true) {
        X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (!cert) {
            // Distinguish "no more certs" (success) from "garbage at
            // the end" (failure). PEM_R_NO_START_LINE is the clean-
            // EOF signal.
            unsigned long err = ERR_peek_last_error();
            ERR_clear_error();
            if (arr->elements.empty()) {
                BIO_free(bio);
                throw RuntimeError("crypto.parseCertificateChain: no certificates found", 0);
            }
            (void)err;
            break;
        }
        arr->elements.push_back(x509ToValue(cert));
        X509_free(cert);
    }
    BIO_free(bio);
    return Value(arr);
}

#endif  // HAVE_OPENSSL

// ── Registration ──

void registerCryptoBuiltins(std::shared_ptr<PraiaMap> cryptoMap) {

    // MD5 (hand-rolled, RFC 1321)
    cryptoMap->entries[Value("md5")] = Value(makeNative("crypto.md5", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("crypto.md5() requires a string", 0);
            auto& input = args[0].asString();

            uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
            static const uint32_t K[64] = {
                0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
                0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
                0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
                0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
                0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
                0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
                0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
                0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
            };
            static const uint32_t s[64] = {
                7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
                5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
                6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
            };

            std::string msg = input;
            uint64_t origLen = msg.size() * 8;
            msg += static_cast<char>(0x80);
            while (msg.size() % 64 != 56) msg += '\0';
            for (int i = 0; i < 8; i++) msg += static_cast<char>((origLen >> (i * 8)) & 0xFF);

            for (size_t offset = 0; offset < msg.size(); offset += 64) {
                uint32_t M[16];
                for (int i = 0; i < 16; i++)
                    M[i] = static_cast<uint8_t>(msg[offset+i*4]) |
                           (static_cast<uint8_t>(msg[offset+i*4+1]) << 8) |
                           (static_cast<uint8_t>(msg[offset+i*4+2]) << 16) |
                           (static_cast<uint8_t>(msg[offset+i*4+3]) << 24);

                uint32_t A = a0, B = b0, C = c0, D = d0;
                for (int i = 0; i < 64; i++) {
                    uint32_t F, g;
                    if (i < 16)      { F = (B & C) | (~B & D); g = i; }
                    else if (i < 32) { F = (D & B) | (~D & C); g = (5*i+1) % 16; }
                    else if (i < 48) { F = B ^ C ^ D;          g = (3*i+5) % 16; }
                    else             { F = C ^ (B | ~D);        g = (7*i) % 16; }
                    F += A + K[i] + M[g];
                    A = D; D = C; C = B;
                    B += (F << s[i]) | (F >> (32 - s[i]));
                }
                a0 += A; b0 += B; c0 += C; d0 += D;
            }

            char hex[33];
            auto toHex = [&](uint32_t v, int off) {
                for (int i = 0; i < 4; i++)
                    snprintf(hex + off + i*2, 3, "%02x", (v >> (i*8)) & 0xFF);
            };
            toHex(a0, 0); toHex(b0, 8); toHex(c0, 16); toHex(d0, 24);
            hex[32] = '\0';
            return Value(std::string(hex));
        }));

    // SHA-1 (hand-rolled, RFC 3174)
    cryptoMap->entries[Value("sha1")] = Value(makeNative("crypto.sha1", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("crypto.sha1() requires a string", 0);
            return Value(sha1_hash(args[0].asString()));
        }));

    // SHA-256 (hand-rolled, FIPS 180-4)
    cryptoMap->entries[Value("sha256")] = Value(makeNative("crypto.sha256", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("crypto.sha256() requires a string", 0);
            return Value(sha256_hex(args[0].asString()));
        }));

    // SHA-512 (hand-rolled, FIPS 180-4)
    cryptoMap->entries[Value("sha512")] = Value(makeNative("crypto.sha512", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("crypto.sha512() requires a string", 0);
            return Value(sha512_hash(args[0].asString()));
        }));

    // HMAC (key, message, algorithm)
    cryptoMap->entries[Value("hmac")] = Value(makeNative("crypto.hmac", 3,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString() || !args[2].isString())
                throw RuntimeError("crypto.hmac() requires (key, message, algorithm) as strings", 0);
            auto& algo = args[2].asString();
            if (algo != "sha256" && algo != "sha1" && algo != "sha512" && algo != "md5")
                throw RuntimeError("crypto.hmac() algorithm must be 'sha256', 'sha1', 'sha512', or 'md5'", 0);
            return Value(hmac_compute(args[0].asString(), args[1].asString(), algo));
        }));

    // crypto.hkdf(key, salt, info, length, hash?) — RFC 5869 HKDF.
    //
    // Extract-then-expand key derivation: turn one high-entropy
    // secret + a salt + a context label into one or more derived
    // keys of arbitrary length. The `info` arg binds the output to a
    // context, so a leak of e.g. the "session-encryption-key" output
    // doesn't compromise other keys derived from the same master
    // with different info strings. Salt can be empty; info can be
    // empty; key (the IKM) cannot.
    //
    // Output is at most 255 * digest_output_size bytes (8160 for
    // SHA-256, 16320 for SHA-512). Larger requests throw.
    //
    // OpenSSL 3+ only; uses the EVP_KDF interface.
    cryptoMap->entries[Value("hkdf")] = Value(makeNative("crypto.hkdf", -1,
#ifdef HAVE_OPENSSL
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 4)
                throw RuntimeError("crypto.hkdf(key, salt, info, length, hash?) requires 4-5 arguments", 0);
            if (!args[0].isString())
                throw RuntimeError("crypto.hkdf: key must be a string (bytes)", 0);
            if (!args[1].isString())
                throw RuntimeError("crypto.hkdf: salt must be a string (use \"\" for no salt)", 0);
            if (!args[2].isString())
                throw RuntimeError("crypto.hkdf: info must be a string (use \"\" for no info)", 0);
            if (!args[3].isNumber())
                throw RuntimeError("crypto.hkdf: length must be a number", 0);

            const auto& key  = args[0].asString();
            const auto& salt = args[1].asString();
            const auto& info = args[2].asString();
            int64_t length   = args[3].toInt64ForBitwise();
            if (length <= 0)
                throw RuntimeError("crypto.hkdf: length must be positive", 0);

            std::string hashName = "sha256";
            if (args.size() >= 5) {
                if (!args[4].isString())
                    throw RuntimeError("crypto.hkdf: hash must be a string name", 0);
                hashName = args[4].asString();
            }
            // Choose digest + per-hash output cap.
            int hashLen = 0;
            if      (hashName == "sha256") hashLen = 32;
            else if (hashName == "sha512") hashLen = 64;
            else if (hashName == "sha384") hashLen = 48;
            else if (hashName == "sha1")   hashLen = 20;
            else throw RuntimeError("crypto.hkdf: hash must be 'sha256', 'sha384', 'sha512', or 'sha1'", 0);
            if (length > 255 * hashLen)
                throw RuntimeError("crypto.hkdf: requested length " + std::to_string(length) +
                                   " exceeds RFC 5869 max " + std::to_string(255 * hashLen) +
                                   " for " + hashName, 0);

            EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
            if (!kdf)
                throw RuntimeError("crypto.hkdf: EVP_KDF_fetch(HKDF) failed (OpenSSL 3+ required)", 0);
            EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
            EVP_KDF_free(kdf);
            if (!ctx)
                throw RuntimeError("crypto.hkdf: EVP_KDF_CTX_new failed", 0);

            // Note: cast away const for OSSL_PARAM_construct_*_string —
            // the API takes non-const pointers for legacy reasons but
            // doesn't write through them.
            OSSL_PARAM params[5];
            int i = 0;
            params[i++] = OSSL_PARAM_construct_utf8_string(
                OSSL_KDF_PARAM_DIGEST, const_cast<char*>(hashName.c_str()), 0);
            params[i++] = OSSL_PARAM_construct_octet_string(
                OSSL_KDF_PARAM_KEY,
                const_cast<void*>(static_cast<const void*>(key.data())), key.size());
            params[i++] = OSSL_PARAM_construct_octet_string(
                OSSL_KDF_PARAM_SALT,
                const_cast<void*>(static_cast<const void*>(salt.data())), salt.size());
            params[i++] = OSSL_PARAM_construct_octet_string(
                OSSL_KDF_PARAM_INFO,
                const_cast<void*>(static_cast<const void*>(info.data())), info.size());
            params[i] = OSSL_PARAM_construct_end();

            std::string out(static_cast<size_t>(length), '\0');
            int rc = EVP_KDF_derive(ctx, reinterpret_cast<unsigned char*>(&out[0]),
                                    out.size(), params);
            EVP_KDF_CTX_free(ctx);
            if (rc != 1)
                throw RuntimeError("crypto.hkdf: EVP_KDF_derive failed", 0);
            return Value(std::move(out));
        }
#else
        [](const std::vector<Value>&) -> Value {
            throw RuntimeError("crypto.hkdf requires OpenSSL (rebuild with HAVE_OPENSSL)", 0);
        }
#endif
    ));

    // Random bytes — returns raw binary string
    cryptoMap->entries[Value("randomBytes")] = Value(makeNative("crypto.randomBytes", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("crypto.randomBytes() requires a number", 0);
            int count = static_cast<int>(args[0].asNumber());
            if (count < 0 || count > 65536)
                throw RuntimeError("crypto.randomBytes() count must be 0-65536", 0);
            return Value(generateRandomBytes(count));
        }));

    // ── AES-256-GCM AEAD (requires OpenSSL) ──
    //
    // `seal` / `open` are the recommended symmetric-encryption API.
    // They use AES-256-GCM, an authenticated cipher: a successful
    // `open` proves the ciphertext was produced by someone holding the
    // key AND that no bit of it has been altered. Compare with
    // `encrypt` / `decrypt` below (CBC), which provide confidentiality
    // only — an attacker who can modify ciphertext bits causes
    // controlled plaintext modifications that decryption can't detect.
    //
    // Sealed output layout: nonce(12) ‖ ciphertext ‖ tag(16). The
    // caller never sees the nonce as a separate value; it's generated
    // freshly per call from the CSPRNG and bundled in.
    //
    // Optional `aad` (additional authenticated data) is bound to the
    // tag but is NOT encrypted — use it for context that must match at
    // open time (user ID, timestamp, protocol version) but doesn't
    // need to be secret. Mismatched AAD fails authentication.

#ifdef HAVE_OPENSSL
    cryptoMap->entries[Value("seal")] = Value(makeNative("crypto.seal", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].isString() || !args[1].isString())
                throw RuntimeError("crypto.seal(plaintext, key, [aad]) — first two args must be strings", 0);
            auto& plaintext = args[0].asString();
            auto& key = args[1].asString();
            if (key.size() != 32)
                throw RuntimeError("crypto.seal(): key must be 32 bytes (256-bit)", 0);
            std::string aad;
            if (args.size() > 2) {
                if (!args[2].isString())
                    throw RuntimeError("crypto.seal(): aad must be a string if provided", 0);
                aad = args[2].asString();
            }

            std::string nonce = generateRandomBytes(12);

            praia::EvpCipherCtxGuard ctx(EVP_CIPHER_CTX_new());
            if (!ctx) throw RuntimeError("crypto.seal(): failed to create context", 0);

            if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
                throw RuntimeError("crypto.seal(): init failed", 0);
            if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1)
                throw RuntimeError("crypto.seal(): set IV length failed", 0);
            if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr,
                    reinterpret_cast<const unsigned char*>(key.data()),
                    reinterpret_cast<const unsigned char*>(nonce.data())) != 1)
                throw RuntimeError("crypto.seal(): key/nonce setup failed", 0);

            int len = 0;
            if (!aad.empty()) {
                if (EVP_EncryptUpdate(ctx.get(), nullptr, &len,
                        reinterpret_cast<const unsigned char*>(aad.data()),
                        static_cast<int>(aad.size())) != 1)
                    throw RuntimeError("crypto.seal(): AAD update failed", 0);
            }

            // GCM doesn't pad, so ciphertext is exactly plaintext.size().
            std::string ct(plaintext.size(), '\0');
            if (EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(&ct[0]), &len,
                    reinterpret_cast<const unsigned char*>(plaintext.data()),
                    static_cast<int>(plaintext.size())) != 1)
                throw RuntimeError("crypto.seal(): encrypt update failed", 0);
            int totalLen = len;
            int finalLen = 0;
            if (EVP_EncryptFinal_ex(ctx.get(),
                    reinterpret_cast<unsigned char*>(&ct[totalLen]), &finalLen) != 1)
                throw RuntimeError("crypto.seal(): final failed", 0);
            totalLen += finalLen;
            ct.resize(totalLen);

            std::string tag(16, '\0');
            if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16,
                    reinterpret_cast<unsigned char*>(&tag[0])) != 1)
                throw RuntimeError("crypto.seal(): tag retrieval failed", 0);

            return Value(nonce + ct + tag);
        }));

    cryptoMap->entries[Value("open")] = Value(makeNative("crypto.open", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].isString() || !args[1].isString())
                throw RuntimeError("crypto.open(sealed, key, [aad]) — first two args must be strings", 0);
            auto& sealed = args[0].asString();
            auto& key = args[1].asString();
            if (key.size() != 32)
                throw RuntimeError("crypto.open(): key must be 32 bytes (256-bit)", 0);
            if (sealed.size() < 28)
                throw RuntimeError("crypto.open(): sealed blob too short (need >=28 bytes: 12 nonce + 16 tag)", 0);
            std::string aad;
            if (args.size() > 2) {
                if (!args[2].isString())
                    throw RuntimeError("crypto.open(): aad must be a string if provided", 0);
                aad = args[2].asString();
            }

            const unsigned char* base = reinterpret_cast<const unsigned char*>(sealed.data());
            const unsigned char* noncePtr = base;
            const unsigned char* ctPtr    = base + 12;
            size_t ctLen                  = sealed.size() - 12 - 16;
            const unsigned char* tagPtr   = base + sealed.size() - 16;

            praia::EvpCipherCtxGuard ctx(EVP_CIPHER_CTX_new());
            if (!ctx) throw RuntimeError("crypto.open(): failed to create context", 0);

            if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
                throw RuntimeError("crypto.open(): init failed", 0);
            if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1)
                throw RuntimeError("crypto.open(): set IV length failed", 0);
            if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                    reinterpret_cast<const unsigned char*>(key.data()), noncePtr) != 1)
                throw RuntimeError("crypto.open(): key/nonce setup failed", 0);

            int len = 0;
            if (!aad.empty()) {
                if (EVP_DecryptUpdate(ctx.get(), nullptr, &len,
                        reinterpret_cast<const unsigned char*>(aad.data()),
                        static_cast<int>(aad.size())) != 1)
                    throw RuntimeError("crypto.open(): AAD update failed", 0);
            }

            std::string pt(ctLen, '\0');
            if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(&pt[0]), &len,
                    ctPtr, static_cast<int>(ctLen)) != 1)
                throw RuntimeError("crypto.open(): decrypt update failed", 0);
            int totalLen = len;

            // Set expected tag BEFORE final — final's return is the auth verdict.
            if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16,
                    const_cast<unsigned char*>(tagPtr)) != 1)
                throw RuntimeError("crypto.open(): tag setup failed", 0);

            int finalLen = 0;
            if (EVP_DecryptFinal_ex(ctx.get(),
                    reinterpret_cast<unsigned char*>(&pt[totalLen]), &finalLen) <= 0) {
                throw RuntimeError("crypto.open(): authentication failed "
                                   "(wrong key, tampered ciphertext, or AAD mismatch)", 0);
            }
            totalLen += finalLen;
            pt.resize(totalLen);
            return Value(std::move(pt));
        }));
#endif

    // ── AES-256-CBC (requires OpenSSL) — UNAUTHENTICATED, low-level ──
    //
    // Confidentiality only. Anyone who can modify the ciphertext can
    // make controlled changes to the decrypted plaintext that
    // `decrypt()` won't notice (padding-oracle and bit-flipping
    // attacks). Use `crypto.seal` / `crypto.open` (AEAD) for any new
    // code. CBC stays available for interop with legacy systems that
    // demand it.

#ifdef HAVE_OPENSSL
    // crypto.encrypt(plaintext, key, iv) — AES-256-CBC, returns raw ciphertext
    cryptoMap->entries[Value("encrypt")] = Value(makeNative("crypto.encrypt", 3,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString() || !args[2].isString())
                throw RuntimeError("crypto.encrypt() requires (plaintext, key, iv) as strings", 0);
            auto& plaintext = args[0].asString();
            auto& key = args[1].asString();
            auto& iv = args[2].asString();
            if (key.size() != 32)
                throw RuntimeError("crypto.encrypt() key must be 32 bytes (256-bit)", 0);
            if (iv.size() != 16)
                throw RuntimeError("crypto.encrypt() iv must be 16 bytes", 0);

            praia::EvpCipherCtxGuard ctx(EVP_CIPHER_CTX_new());
            if (!ctx) throw RuntimeError("crypto.encrypt() failed to create context", 0);

            if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr,
                    reinterpret_cast<const unsigned char*>(key.data()),
                    reinterpret_cast<const unsigned char*>(iv.data())) != 1)
                throw RuntimeError("crypto.encrypt() init failed", 0);

            std::string ciphertext(plaintext.size() + 16, '\0');
            int len = 0, totalLen = 0;
            if (EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(&ciphertext[0]),
                    &len, reinterpret_cast<const unsigned char*>(plaintext.data()),
                    static_cast<int>(plaintext.size())) != 1)
                throw RuntimeError("crypto.encrypt() update failed", 0);
            totalLen = len;
            if (EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(&ciphertext[totalLen]),
                    &len) != 1)
                throw RuntimeError("crypto.encrypt() final failed", 0);
            totalLen += len;
            ciphertext.resize(totalLen);
            return Value(std::move(ciphertext));
        }));

    // crypto.decrypt(ciphertext, key, iv) — AES-256-CBC, returns plaintext
    cryptoMap->entries[Value("decrypt")] = Value(makeNative("crypto.decrypt", 3,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString() || !args[2].isString())
                throw RuntimeError("crypto.decrypt() requires (ciphertext, key, iv) as strings", 0);
            auto& ciphertext = args[0].asString();
            auto& key = args[1].asString();
            auto& iv = args[2].asString();
            if (key.size() != 32)
                throw RuntimeError("crypto.decrypt() key must be 32 bytes (256-bit)", 0);
            if (iv.size() != 16)
                throw RuntimeError("crypto.decrypt() iv must be 16 bytes", 0);

            praia::EvpCipherCtxGuard ctx(EVP_CIPHER_CTX_new());
            if (!ctx) throw RuntimeError("crypto.decrypt() failed to create context", 0);

            if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr,
                    reinterpret_cast<const unsigned char*>(key.data()),
                    reinterpret_cast<const unsigned char*>(iv.data())) != 1)
                throw RuntimeError("crypto.decrypt() init failed", 0);

            std::string plaintext(ciphertext.size(), '\0');
            int len = 0, totalLen = 0;
            if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(&plaintext[0]),
                    &len, reinterpret_cast<const unsigned char*>(ciphertext.data()),
                    static_cast<int>(ciphertext.size())) != 1)
                throw RuntimeError("crypto.decrypt() failed (wrong key/iv or corrupted data)", 0);
            totalLen = len;
            if (EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(&plaintext[totalLen]),
                    &len) != 1)
                throw RuntimeError("crypto.decrypt() failed (wrong key/iv or corrupted data)", 0);
            totalLen += len;
            plaintext.resize(totalLen);
            return Value(std::move(plaintext));
        }));
#endif

    // ── Password hashing (bcrypt-style using PBKDF2, requires OpenSSL) ──

#ifdef HAVE_OPENSSL
    // crypto.hashPassword(password, salt?, iterations?) — PBKDF2-SHA256
    cryptoMap->entries[Value("hashPassword")] = Value(makeNative("crypto.hashPassword", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("crypto.hashPassword() requires a password string", 0);
            auto& password = args[0].asString();

            // Salt: omitted, or explicitly nil (the positional-skip
            // idiom — `hashPassword(pw, nil, 1000)`) → 16 random bytes.
            // Any other provided type is a caller bug: silently
            // falling back to random for e.g. a number or a map
            // produces a hash the caller can never reproduce, and
            // hides typos in fixture wiring.
            std::string salt;
            if (args.size() > 1 && !args[1].isNil()) {
                if (!args[1].isString())
                    throw RuntimeError("crypto.hashPassword(): salt must be a string or nil", 0);
                salt = args[1].asString();
            } else {
                salt = generateRandomBytes(16);
            }

            int iterations = 100000;
            if (args.size() > 2 && !args[2].isNil()) {
                // Same rationale as salt: a non-number iterations arg
                // (or one < 1, fractional, NaN/Inf, or > INT32_MAX)
                // is a caller bug, not something to silently paper
                // over with truncation or clamping.
                if (!args[2].isNumber())
                    throw RuntimeError("crypto.hashPassword(): iterations must be a number", 0);
                iterations = validatePbkdf2Iterations(args[2], "crypto.hashPassword()");
            }

            unsigned char derived[32];
            if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                    reinterpret_cast<const unsigned char*>(salt.data()),
                    static_cast<int>(salt.size()), iterations, EVP_sha256(), 32, derived) != 1) {
                throw RuntimeError("crypto.hashPassword() failed", 0);
            }

            auto result = gcNew<PraiaMap>();
            result->entries[Value("hash")] = Value(toHexString(derived, 32));
            result->entries[Value("salt")] = Value(toHexString(reinterpret_cast<const uint8_t*>(salt.data()), salt.size()));
            result->entries[Value("iterations")] = Value(static_cast<int64_t>(iterations));
            return Value(result);
        }));

    // crypto.verifyPassword(password, hash, salt, iterations?) — verify PBKDF2
    cryptoMap->entries[Value("verifyPassword")] = Value(makeNative("crypto.verifyPassword", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 3 || !args[0].isString() || !args[1].isString() || !args[2].isString())
                throw RuntimeError("crypto.verifyPassword() requires (password, hash, salt)", 0);
            auto& password = args[0].asString();
            auto& expectedHex = args[1].asString();
            auto& saltHex = args[2].asString();

            int iterations = 100000;
            if (args.size() > 3 && !args[3].isNil()) {
                if (!args[3].isNumber())
                    throw RuntimeError("crypto.verifyPassword(): iterations must be a number", 0);
                iterations = validatePbkdf2Iterations(args[3], "crypto.verifyPassword()");
            }

            // Decode salt from hex. Odd length is rejected: the old
            // `i + 1 < size` loop silently dropped the final nibble,
            // so e.g. "616263f" decoded to the same bytes as "616263"
            // and matched a hash made with that shorter salt — a
            // verification bypass for any caller that fed in a
            // truncated or attacker-controlled salt string.
            if (saltHex.size() % 2 != 0)
                throw RuntimeError("crypto.verifyPassword() salt hex must have even length", 0);
            std::string salt;
            salt.reserve(saltHex.size() / 2);
            for (size_t i = 0; i + 1 < saltHex.size(); i += 2) {
                auto hexVal = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int hi = hexVal(saltHex[i]);
                int lo = hexVal(saltHex[i+1]);
                if (hi < 0 || lo < 0) throw RuntimeError("crypto.verifyPassword() invalid salt hex", 0);
                salt += static_cast<char>((hi << 4) | lo);
            }

            unsigned char derived[32];
            if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                    reinterpret_cast<const unsigned char*>(salt.data()),
                    static_cast<int>(salt.size()), iterations, EVP_sha256(), 32, derived) != 1) {
                throw RuntimeError("crypto.verifyPassword() failed", 0);
            }

            std::string actualHex = toHexString(derived, 32);
            // Constant-time comparison. Don't early-return on size mismatch —
            // textbook timing-leak: an early return shortcuts the work and
            // leaks length info via wall time. Instead, fold the length
            // diff into the running XOR accumulator so the loop runs over
            // min(a, b) bytes regardless and the final result is true iff
            // both lengths and bytes match. In practice actualHex is always
            // 64 chars and expectedHex is a stored hash that should also be
            // 64, so this branch is rarely hit — but the fix is free.
            int diff = static_cast<int>(actualHex.size() ^ expectedHex.size());
            size_t n = std::min(actualHex.size(), expectedHex.size());
            for (size_t i = 0; i < n; i++)
                diff |= actualHex[i] ^ expectedHex[i];
            return Value(diff == 0);
        }));

    // ── Modern password KDFs (scrypt + argon2id) ─────────────
    //
    // Both functions return self-describing PHC strings rather than
    // a structured map. The hash carries its own algorithm + cost
    // parameters, so verifiers don't need to know how the password
    // was originally hashed beyond "it's a PHC string". This format
    // round-trips with Python's passlib, libsodium, and the argon2
    // reference CLI, so hashes can be migrated in either direction.
    //
    // For new code, prefer argon2id (memory-hard, the OWASP first
    // choice as of 2024). scrypt remains useful when interop with
    // existing scrypt-hashed databases matters; PBKDF2 (the older
    // hashPassword/verifyPassword) is left in place for back-compat
    // but isn't recommended for new applications.

    // crypto.scrypt(password, params?) — RFC 7914 scrypt.
    //   params: optional map; supported keys:
    //     ln      log2(N), default 15 (N = 32768, ~32 MiB)
    //     r       block size, default 8
    //     p       parallelism, default 1
    //     salt    pre-generated salt bytes; default 16 random
    //     length  output bytes, default 32
    cryptoMap->entries[Value("scrypt")] = Value(makeNative("crypto.scrypt", -1,
#ifdef HAVE_OPENSSL
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("crypto.scrypt(password, params?) requires a password string", 0);
            const std::string& password = args[0].asString();
            Value params = args.size() > 1 ? args[1] : Value();
            uint64_t ln = mapUintParam(params, "ln", 15, 1, 30, "crypto.scrypt");
            uint64_t r  = mapUintParam(params, "r",  8,  1, 1u << 20, "crypto.scrypt");
            uint64_t p  = mapUintParam(params, "p",  1,  1, 1u << 16, "crypto.scrypt");
            uint64_t length = mapUintParam(params, "length", 32, 16, 1024, "crypto.scrypt");
            std::string salt = mapStringParam(params, "salt", "crypto.scrypt");
            if (salt.empty()) salt = generateRandomBytes(16);
            if (salt.size() < 8)
                throw RuntimeError("crypto.scrypt: salt must be at least 8 bytes", 0);
            uint64_t N = 1ULL << ln;
            std::string h = scryptDerive(password, salt, N, r, p, length);
            // PHC: $scrypt$ln=<ln>,r=<r>,p=<p>$<b64 salt>$<b64 hash>
            std::string out = "$scrypt$ln=" + std::to_string(ln) +
                              ",r=" + std::to_string(r) +
                              ",p=" + std::to_string(p) +
                              "$" + phcB64Encode(salt) +
                              "$" + phcB64Encode(h);
            return Value(out);
        }
#else
        [](const std::vector<Value>&) -> Value {
            throw RuntimeError("crypto.scrypt requires OpenSSL (rebuild with HAVE_OPENSSL)", 0);
        }
#endif
    ));

    cryptoMap->entries[Value("verifyScrypt")] = Value(makeNative("crypto.verifyScrypt", 2,
#ifdef HAVE_OPENSSL
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("crypto.verifyScrypt(password, phc) requires two strings", 0);
            const std::string& password = args[0].asString();
            PhcParts p = phcParse(args[1].asString());
            if (p.algo != "scrypt")
                throw RuntimeError("crypto.verifyScrypt: PHC algorithm is '" + p.algo +
                                   "', expected 'scrypt'", 0);
            // Bounds mirror crypto.scrypt's generator: ln ∈ [1, 30],
            // r and p in the uint32_t range that scryptDerive accepts
            // (it then passes them to OpenSSL via uint64 params).
            uint64_t ln = phcParseUint("ln", p.params, 1, 30);
            uint64_t r  = phcParseUint("r",  p.params, 1, UINT32_MAX);
            uint64_t pp = phcParseUint("p",  p.params, 1, UINT32_MAX);
            // Reject empty / too-small / too-large salt and hash
            // segments. Without these checks an attacker can craft
            // a PHC with a hash segment that decodes to zero bytes
            // (e.g. an empty segment, or a stray 1-char segment),
            // get the derive to produce zero bytes, and have
            // ctEqual("", "") return true for any password. Bounds
            // mirror the generator: salt ≥ 8 bytes, hash ∈ [16, 1024].
            if (p.salt.size() < 8)
                throw RuntimeError("crypto.verifyScrypt: salt must be at least 8 bytes "
                                   "(got " + std::to_string(p.salt.size()) + ")", 0);
            if (p.hash.size() < 16 || p.hash.size() > 1024)
                throw RuntimeError("crypto.verifyScrypt: hash length must be in [16, 1024] "
                                   "(got " + std::to_string(p.hash.size()) + ")", 0);
            std::string derived = scryptDerive(password, p.salt, 1ULL << ln, r, pp, p.hash.size());
            return Value(ctEqual(derived, p.hash));
        }
#else
        [](const std::vector<Value>&) -> Value {
            throw RuntimeError("crypto.verifyScrypt requires OpenSSL (rebuild with HAVE_OPENSSL)", 0);
        }
#endif
    ));

    // crypto.argon2id(password, params?) — RFC 9106 Argon2id.
    //   params: optional map; supported keys:
    //     t       iterations / time cost, default 3
    //     m       memory in KiB, default 65536 (64 MiB)
    //     p       parallelism (lanes), default 4
    //     salt    pre-generated salt bytes; default 16 random
    //     length  output bytes, default 32
    //
    // Defaults follow RFC 9106 §4 "second recommended option" — fits
    // a modest server budget. For higher-security contexts bump t/m
    // until verification takes ~250 ms on the verifier hardware.
    //
    // Throws on OpenSSL < 3.2 with a clear error message.
    cryptoMap->entries[Value("argon2id")] = Value(makeNative("crypto.argon2id", -1,
#ifdef HAVE_OPENSSL
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("crypto.argon2id(password, params?) requires a password string", 0);
            const std::string& password = args[0].asString();
            Value params = args.size() > 1 ? args[1] : Value();
            uint64_t t = mapUintParam(params, "t", 3, 1, 1u << 20, "crypto.argon2id");
            uint64_t m = mapUintParam(params, "m", 65536, 8, 1u << 22, "crypto.argon2id");
            uint64_t p = mapUintParam(params, "p", 4, 1, 1u << 16, "crypto.argon2id");
            uint64_t length = mapUintParam(params, "length", 32, 4, 1024, "crypto.argon2id");
            if (m < 8 * p)
                throw RuntimeError("crypto.argon2id: m (memory KiB) must be >= 8 * p (lanes)", 0);
            std::string salt = mapStringParam(params, "salt", "crypto.argon2id");
            if (salt.empty()) salt = generateRandomBytes(16);
            if (salt.size() < 8)
                throw RuntimeError("crypto.argon2id: salt must be at least 8 bytes", 0);
            std::string h = argon2idDerive(password, salt,
                                           static_cast<uint32_t>(t),
                                           static_cast<uint32_t>(m),
                                           static_cast<uint32_t>(p),
                                           static_cast<size_t>(length));
            // PHC with the Argon2 version segment (v=19, current).
            std::string out = "$argon2id$v=19$m=" + std::to_string(m) +
                              ",t=" + std::to_string(t) +
                              ",p=" + std::to_string(p) +
                              "$" + phcB64Encode(salt) +
                              "$" + phcB64Encode(h);
            return Value(out);
        }
#else
        [](const std::vector<Value>&) -> Value {
            throw RuntimeError("crypto.argon2id requires OpenSSL (rebuild with HAVE_OPENSSL)", 0);
        }
#endif
    ));

    cryptoMap->entries[Value("verifyArgon2id")] = Value(makeNative("crypto.verifyArgon2id", 2,
#ifdef HAVE_OPENSSL
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("crypto.verifyArgon2id(password, phc) requires two strings", 0);
            const std::string& password = args[0].asString();
            PhcParts p = phcParse(args[1].asString());
            if (p.algo != "argon2id")
                throw RuntimeError("crypto.verifyArgon2id: PHC algorithm is '" + p.algo +
                                   "', expected 'argon2id'", 0);
            // phcParse has already enforced that the v=NN segment is
            // present and equals "19" (the only Argon2 version this
            // build supports). No further version handling needed
            // here — t/m/p are the only remaining knobs.
            // argon2idDerive narrows to uint32_t for OpenSSL's
            // OSSL_PARAM_construct_uint32. Cap at UINT32_MAX *before*
            // the cast — without this bound, "m=4294967360" silently
            // wraps to m=64, which means an attacker can verify the
            // password under a much weaker cost factor than the
            // generator chose. UINT32_MAX as the ceiling is exactly
            // the size of the OpenSSL field.
            uint64_t t  = phcParseUint("t", p.params, 1, UINT32_MAX);
            uint64_t m  = phcParseUint("m", p.params, 1, UINT32_MAX);
            uint64_t pp = phcParseUint("p", p.params, 1, UINT32_MAX);
            // Reject degenerate salt/hash segments — see verifyScrypt
            // for the rationale (empty hash → derive(0) → ctEqual("",
            // "") → any password verifies). Bounds match the
            // argon2id generator: salt ≥ 8, hash ∈ [4, 1024].
            if (p.salt.size() < 8)
                throw RuntimeError("crypto.verifyArgon2id: salt must be at least 8 bytes "
                                   "(got " + std::to_string(p.salt.size()) + ")", 0);
            if (p.hash.size() < 4 || p.hash.size() > 1024)
                throw RuntimeError("crypto.verifyArgon2id: hash length must be in [4, 1024] "
                                   "(got " + std::to_string(p.hash.size()) + ")", 0);
            std::string derived = argon2idDerive(password, p.salt,
                                                 static_cast<uint32_t>(t),
                                                 static_cast<uint32_t>(m),
                                                 static_cast<uint32_t>(pp),
                                                 p.hash.size());
            return Value(ctEqual(derived, p.hash));
        }
#else
        [](const std::vector<Value>&) -> Value {
            throw RuntimeError("crypto.verifyArgon2id requires OpenSSL (rebuild with HAVE_OPENSSL)", 0);
        }
#endif
    ));

    // ── X509 certificate parsing ─────────────────────────────

    // crypto.parseCertificate(pemOrDer)
    //   Accepts either a PEM string (containing -----BEGIN CERTIFICATE-----)
    //   or raw DER bytes. Returns a map describing the certificate's
    //   key fields — subject/issuer DNs, validity window, SANs,
    //   public key, SHA-256 fingerprint, CA flag, sig algorithm.
    //
    //   When the input contains multiple PEM-encoded certs, only the
    //   first is parsed. Use parseCertificateChain for multi-cert
    //   bundles (server cert + intermediates).
    cryptoMap->entries[Value("parseCertificate")] = Value(makeNative("crypto.parseCertificate", 1,
#ifdef HAVE_OPENSSL
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("crypto.parseCertificate(data) requires a string (PEM or DER bytes)", 0);
            return parseCertificateImpl(args[0].asString());
        }
#else
        [](const std::vector<Value>&) -> Value {
            throw RuntimeError("crypto.parseCertificate requires OpenSSL (rebuild with HAVE_OPENSSL)", 0);
        }
#endif
    ));

    // crypto.parseCertificateChain(pem)
    //   Parses all PEM-encoded certs in the input and returns them
    //   as an array (in file order). Throws if the input contains
    //   no PEM certificates at all.
    cryptoMap->entries[Value("parseCertificateChain")] = Value(makeNative("crypto.parseCertificateChain", 1,
#ifdef HAVE_OPENSSL
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("crypto.parseCertificateChain(pem) requires a string", 0);
            return parseCertificateChainImpl(args[0].asString());
        }
#else
        [](const std::vector<Value>&) -> Value {
            throw RuntimeError("crypto.parseCertificateChain requires OpenSSL (rebuild with HAVE_OPENSSL)", 0);
        }
#endif
    ));

    // ── Asymmetric crypto ──

    // Helper: get EVP_MD from algorithm name
    auto getDigest = [](const std::string& algo) -> const EVP_MD* {
        if (algo == "sha256") return EVP_sha256();
        if (algo == "sha384") return EVP_sha384();
        if (algo == "sha512") return EVP_sha512();
        if (algo == "sha1") return EVP_sha1();
        return nullptr;
    };

    // Helper: base64 encode/decode for signatures
    auto b64Encode = [](const std::vector<uint8_t>& data) -> std::string {
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* mem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, mem);
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(b64, data.data(), static_cast<int>(data.size()));
        BIO_flush(b64);
        BUF_MEM* bptr;
        BIO_get_mem_ptr(b64, &bptr);
        std::string result(bptr->data, bptr->length);
        BIO_free_all(b64);
        return result;
    };

    auto b64Decode = [](const std::string& encoded) -> std::vector<uint8_t> {
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* mem = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
        mem = BIO_push(b64, mem);
        BIO_set_flags(mem, BIO_FLAGS_BASE64_NO_NL);
        std::vector<uint8_t> result(encoded.size());
        int len = BIO_read(mem, result.data(), static_cast<int>(result.size()));
        if (len > 0) result.resize(len);
        else result.clear();
        BIO_free_all(mem);
        return result;
    };

    // crypto.sign(data, privateKeyPEM, algorithm?)
    cryptoMap->entries[Value("sign")] = Value(makeNative("crypto.sign", -1,
        [getDigest, b64Encode](const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].isString() || !args[1].isString())
                throw RuntimeError("crypto.sign(data, privateKeyPEM, algorithm?) requires data and key strings", 0);
            auto& data = args[0].asString();
            auto& keyPem = args[1].asString();
            std::string algo = (args.size() > 2 && args[2].isString()) ? args[2].asString() : "sha256";

            const EVP_MD* md = getDigest(algo);
            if (!md) throw RuntimeError("crypto.sign(): unknown algorithm '" + algo + "'", 0);

            praia::BioGuard bio(BIO_new_mem_buf(keyPem.data(), static_cast<int>(keyPem.size())));
            praia::EvpPkeyGuard pkey(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
            if (!pkey) throw RuntimeError("crypto.sign(): invalid private key", 0);

            praia::EvpMdCtxGuard ctx(EVP_MD_CTX_new());
            if (!ctx) throw RuntimeError("crypto.sign(): failed to create digest context", 0);

            size_t sigLen = 0;
            bool ok = EVP_DigestSignInit(ctx.get(), nullptr, md, nullptr, pkey.get()) == 1 &&
                      EVP_DigestSignUpdate(ctx.get(), data.data(), data.size()) == 1 &&
                      EVP_DigestSignFinal(ctx.get(), nullptr, &sigLen) == 1;
            std::vector<uint8_t> sig(sigLen);
            ok = ok && EVP_DigestSignFinal(ctx.get(), sig.data(), &sigLen) == 1;
            sig.resize(sigLen);

            if (!ok) throw RuntimeError("crypto.sign(): signing failed", 0);
            return Value(b64Encode(sig));
        }));

    // crypto.verify(data, signature, publicKeyPEM, algorithm?)
    cryptoMap->entries[Value("verify")] = Value(makeNative("crypto.verify", -1,
        [getDigest, b64Decode](const std::vector<Value>& args) -> Value {
            if (args.size() < 3 || !args[0].isString() || !args[1].isString() || !args[2].isString())
                throw RuntimeError("crypto.verify(data, signature, publicKeyPEM, algorithm?) requires three strings", 0);
            auto& data = args[0].asString();
            auto& sigB64 = args[1].asString();
            auto& keyPem = args[2].asString();
            std::string algo = (args.size() > 3 && args[3].isString()) ? args[3].asString() : "sha256";

            const EVP_MD* md = getDigest(algo);
            if (!md) throw RuntimeError("crypto.verify(): unknown algorithm '" + algo + "'", 0);

            auto sig = b64Decode(sigB64);
            if (sig.empty()) return Value(false);

            praia::BioGuard bio(BIO_new_mem_buf(keyPem.data(), static_cast<int>(keyPem.size())));
            praia::EvpPkeyGuard pkey(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
            if (!pkey) throw RuntimeError("crypto.verify(): invalid public key", 0);

            praia::EvpMdCtxGuard ctx(EVP_MD_CTX_new());
            if (!ctx) throw RuntimeError("crypto.verify(): failed to create digest context", 0);

            bool ok = EVP_DigestVerifyInit(ctx.get(), nullptr, md, nullptr, pkey.get()) == 1 &&
                      EVP_DigestVerifyUpdate(ctx.get(), data.data(), data.size()) == 1 &&
                      EVP_DigestVerifyFinal(ctx.get(), sig.data(), sig.size()) == 1;
            return Value(ok);
        }));

    // crypto.generateKeyPair(type?, bits?)
    cryptoMap->entries[Value("generateKeyPair")] = Value(makeNative("crypto.generateKeyPair", -1,
        [](const std::vector<Value>& args) -> Value {
            std::string keyType = (!args.empty() && args[0].isString()) ? args[0].asString() : "rsa";
            int bits = (args.size() > 1 && args[1].isNumber()) ? static_cast<int>(args[1].asNumber()) : 2048;

            praia::EvpPkeyGuard pkey;
            praia::EvpPkeyCtxGuard ctx;

            if (keyType == "rsa") {
                ctx = praia::EvpPkeyCtxGuard(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
                EVP_PKEY* raw = nullptr;
                if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0 ||
                    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), bits) <= 0 ||
                    EVP_PKEY_keygen(ctx.get(), &raw) <= 0)
                    throw RuntimeError("crypto.generateKeyPair(): RSA key generation failed", 0);
                pkey = praia::EvpPkeyGuard(raw);
            } else if (keyType == "ec") {
                ctx = praia::EvpPkeyCtxGuard(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
                EVP_PKEY* raw = nullptr;
                if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0 ||
                    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) <= 0 ||
                    EVP_PKEY_keygen(ctx.get(), &raw) <= 0)
                    throw RuntimeError("crypto.generateKeyPair(): EC key generation failed", 0);
                pkey = praia::EvpPkeyGuard(raw);
            } else {
                throw RuntimeError("crypto.generateKeyPair(): unknown type '" + keyType + "' (expected 'rsa' or 'ec')", 0);
            }

            // Export private key as PEM
            std::string privPem;
            {
                praia::BioGuard privBio(BIO_new(BIO_s_mem()));
                PEM_write_bio_PrivateKey(privBio.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr);
                BUF_MEM* privBuf;
                BIO_get_mem_ptr(privBio.get(), &privBuf);
                privPem.assign(privBuf->data, privBuf->length);
            }

            // Export public key as PEM
            std::string pubPem;
            {
                praia::BioGuard pubBio(BIO_new(BIO_s_mem()));
                PEM_write_bio_PUBKEY(pubBio.get(), pkey.get());
                BUF_MEM* pubBuf;
                BIO_get_mem_ptr(pubBio.get(), &pubBuf);
                pubPem.assign(pubBuf->data, pubBuf->length);
            }

            auto result = gcNew<PraiaMap>();
            result->entries[Value("privateKey")] = Value(std::move(privPem));
            result->entries[Value("publicKey")] = Value(std::move(pubPem));
            return Value(result);
        }));
#endif
}
