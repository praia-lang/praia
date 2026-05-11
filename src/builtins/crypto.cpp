#include "../builtins.h"
#include "../gc_heap.h"
#include "scope_guards.h"
#include <algorithm>
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

    // ── AES-256-CBC (requires OpenSSL) ──

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

            // Generate or use provided salt
            std::string salt;
            if (args.size() > 1 && args[1].isString()) {
                salt = args[1].asString();
            } else {
                salt = generateRandomBytes(16);
            }

            int iterations = 100000;
            if (args.size() > 2 && args[2].isNumber()) {
                iterations = static_cast<int>(args[2].asNumber());
                if (iterations < 1) iterations = 1;
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
            if (args.size() > 3 && args[3].isNumber())
                iterations = static_cast<int>(args[3].asNumber());

            // Decode salt from hex
            std::string salt;
            for (size_t i = 0; i + 1 < saltHex.size(); i += 2) {
                int hi = 0, lo = 0;
                auto hexVal = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                hi = hexVal(saltHex[i]);
                lo = hexVal(saltHex[i+1]);
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
