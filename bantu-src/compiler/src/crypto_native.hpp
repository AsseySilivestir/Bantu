#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  crypto_native.hpp — native (C++) accelerators for the Bantu crypto suite.
//
//  WHY THIS EXISTS
//  ---------------
//  The hash/crypto/uuid modules are written in pure Bantu on top of native 32-bit
//  primitives. That is auditable and correct, but a tree-walking interpreter runs
//  the compression loops at ~3 KB/s — fine for tokens/passwords, useless for files.
//  This header provides byte-identical NATIVE implementations of the same digests,
//  so the .b modules can delegate to them when present (via `has_native(...)`) and
//  fall back to the pure-Bantu reference otherwise. A differential test asserts the
//  two paths agree on the official vectors, so the fast path can never silently
//  diverge from the audited spec.
//
//  DESIGN / SAFETY
//  ---------------
//   • Self-contained: no external crypto library, no allocations in the hot loop,
//     endian-safe (explicit byte assembly — never a reinterpret_cast over words).
//   • Additive: nothing here changes existing language behavior; these are extra
//     builtins the modules opt into. Old code and the pure-Bantu paths are intact.
//   • These raw digests operate on PUBLIC data (message → digest), so they are not
//     required to be constant-time; secret-dependent comparisons still go through
//     the constant-time `ct_equal` primitive.
//   • SHA-512/384 live here because their 64-bit words cannot be represented
//     exactly by Bantu's float64 — there is deliberately no pure-Bantu fallback.
//
//  Implementations follow the FIPS 180-4 (SHA), RFC 1321 (MD5), and RFC 2104
//  (HMAC) specifications and are validated bit-exact against the published vectors.
// ════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace bantu_native {

// ── small helpers ────────────────────────────────────────────────────────────
static inline uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t rotl32(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }
static inline uint64_t rotr64(uint64_t x, unsigned n) { return (x >> n) | (x << (64 - n)); }

static const char* kHexLower = "0123456789abcdef";

// Raw digest bytes → lowercase hex string.
inline std::string toHex(const uint8_t* p, size_t n) {
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s.push_back(kHexLower[p[i] >> 4]); s.push_back(kHexLower[p[i] & 0xF]); }
    return s;
}

// ════════════════════════════════════════════════════════════════════════════
//  MD5 — RFC 1321. Little-endian. Cryptographically BROKEN (collisions); provided
//  for checksums / legacy interop / UUID v3 only. Callers label it as non-secure.
// ════════════════════════════════════════════════════════════════════════════
inline void md5_raw(const uint8_t* msg, size_t len, uint8_t out[16]) {
    // Per-round left-rotate amounts and the precomputed sine-derived constants K.
    static const uint32_t S[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21 };
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };

    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

    // Padded length: message + 0x80 + zeros + 64-bit little-endian bit length.
    size_t newLen = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> buf(newLen, 0);
    std::memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[newLen - 8 + i] = (uint8_t)(bits >> (8 * i));

    for (size_t off = 0; off < newLen; off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++)
            M[i] = (uint32_t)buf[off+4*i] | ((uint32_t)buf[off+4*i+1]<<8)
                 | ((uint32_t)buf[off+4*i+2]<<16) | ((uint32_t)buf[off+4*i+3]<<24);

        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | (~B & D);        g = i; }
            else if (i < 32) { F = (D & B) | (~D & C);        g = (5*i + 1) & 15; }
            else if (i < 48) { F = B ^ C ^ D;                 g = (3*i + 5) & 15; }
            else             { F = C ^ (B | ~D);              g = (7*i) & 15; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + rotl32(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }

    uint32_t words[4] = { a0, b0, c0, d0 };
    for (int i = 0; i < 4; i++)               // little-endian output
        for (int j = 0; j < 4; j++) out[4*i + j] = (uint8_t)(words[i] >> (8*j));
}

// ════════════════════════════════════════════════════════════════════════════
//  SHA-1 — FIPS 180-4. Big-endian. BROKEN for collision resistance; checksums /
//  legacy interop / UUID v5 only.
// ════════════════════════════════════════════════════════════════════════════
inline void sha1_raw(const uint8_t* msg, size_t len, uint8_t out[20]) {
    uint32_t h0=0x67452301,h1=0xEFCDAB89,h2=0x98BADCFE,h3=0x10325476,h4=0xC3D2E1F0;

    size_t newLen = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> buf(newLen, 0);
    std::memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[newLen - 1 - i] = (uint8_t)(bits >> (8*i)); // big-endian

    for (size_t off = 0; off < newLen; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)buf[off+4*i]<<24) | ((uint32_t)buf[off+4*i+1]<<16)
                 | ((uint32_t)buf[off+4*i+2]<<8) | (uint32_t)buf[off+4*i+3];
        for (int i = 16; i < 80; i++) w[i] = rotl32(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);

        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            uint32_t t = rotl32(a,5) + f + e + k + w[i];
            e = d; d = c; c = rotl32(b,30); b = a; a = t;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }

    uint32_t hs[5] = { h0,h1,h2,h3,h4 };
    for (int i = 0; i < 5; i++)               // big-endian output
        for (int j = 0; j < 4; j++) out[4*i + j] = (uint8_t)(hs[i] >> (24 - 8*j));
}

// ════════════════════════════════════════════════════════════════════════════
//  SHA-256 / SHA-224 — FIPS 180-4. Big-endian, 32-bit words. SECURE.
//  `bits224 == true` uses the SHA-224 IV and emits the 28-byte truncation.
// ════════════════════════════════════════════════════════════════════════════
inline void sha256_core(const uint8_t* msg, size_t len, uint32_t H[8]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

    size_t newLen = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> buf(newLen, 0);
    std::memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[newLen - 1 - i] = (uint8_t)(bits >> (8*i));

    for (size_t off = 0; off < newLen; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)buf[off+4*i]<<24) | ((uint32_t)buf[off+4*i+1]<<16)
                 | ((uint32_t)buf[off+4*i+2]<<8) | (uint32_t)buf[off+4*i+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr32(w[i-15],7) ^ rotr32(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr32(w[i-2],17) ^ rotr32(w[i-2],19)  ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=H[0],b=H[1],c=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr32(e,6) ^ rotr32(e,11) ^ rotr32(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        H[0]+=a; H[1]+=b; H[2]+=c; H[3]+=d; H[4]+=e; H[5]+=f; H[6]+=g; H[7]+=h;
    }
}
inline void sha256_raw(const uint8_t* msg, size_t len, uint8_t out[32]) {
    uint32_t H[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    sha256_core(msg, len, H);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[4*i+j] = (uint8_t)(H[i] >> (24 - 8*j));
}
inline void sha224_raw(const uint8_t* msg, size_t len, uint8_t out[28]) {
    uint32_t H[8] = {0xc1059ed8,0x367cd507,0x3070dd17,0xf70e5939,0xffc00b31,0x68581511,0x64f98fa7,0xbefa4fa4};
    sha256_core(msg, len, H);
    for (int i = 0; i < 7; i++) for (int j = 0; j < 4; j++) out[4*i+j] = (uint8_t)(H[i] >> (24 - 8*j));
}

// ════════════════════════════════════════════════════════════════════════════
//  SHA-512 / SHA-384 — FIPS 180-4. Big-endian, 64-bit words. SECURE.
//  These require true 64-bit integers, which Bantu's float64 cannot represent —
//  hence native-only (there is deliberately no pure-Bantu fallback).
// ════════════════════════════════════════════════════════════════════════════
inline void sha512_core(const uint8_t* msg, size_t len, uint64_t H[8]) {
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
        0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL };

    // Pad to a multiple of 128, appending a 128-bit big-endian bit length
    // (the high 64 bits are always zero for any practical input size).
    size_t newLen = ((len + 16) / 128 + 1) * 128;
    std::vector<uint8_t> buf(newLen, 0);
    std::memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[newLen - 1 - i] = (uint8_t)(bits >> (8*i));

    for (size_t off = 0; off < newLen; off += 128) {
        uint64_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = 0;
            for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | buf[off + 8*i + j];
        }
        for (int i = 16; i < 80; i++) {
            uint64_t s0 = rotr64(w[i-15],1) ^ rotr64(w[i-15],8) ^ (w[i-15] >> 7);
            uint64_t s1 = rotr64(w[i-2],19) ^ rotr64(w[i-2],61) ^ (w[i-2] >> 6);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint64_t a=H[0],b=H[1],c=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7];
        for (int i = 0; i < 80; i++) {
            uint64_t S1 = rotr64(e,14) ^ rotr64(e,18) ^ rotr64(e,41);
            uint64_t ch = (e & f) ^ (~e & g);
            uint64_t t1 = h + S1 + ch + K[i] + w[i];
            uint64_t S0 = rotr64(a,28) ^ rotr64(a,34) ^ rotr64(a,39);
            uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint64_t t2 = S0 + maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        H[0]+=a; H[1]+=b; H[2]+=c; H[3]+=d; H[4]+=e; H[5]+=f; H[6]+=g; H[7]+=h;
    }
}
inline void sha512_raw(const uint8_t* msg, size_t len, uint8_t out[64]) {
    uint64_t H[8] = {0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
                     0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    sha512_core(msg, len, H);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) out[8*i+j] = (uint8_t)(H[i] >> (56 - 8*j));
}
inline void sha384_raw(const uint8_t* msg, size_t len, uint8_t out[48]) {
    uint64_t H[8] = {0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
                     0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL};
    sha512_core(msg, len, H);
    for (int i = 0; i < 6; i++) for (int j = 0; j < 8; j++) out[8*i+j] = (uint8_t)(H[i] >> (56 - 8*j));
}

// ════════════════════════════════════════════════════════════════════════════
//  HMAC — RFC 2104, generic over a block-based hash. Used for HMAC-SHA256.
// ════════════════════════════════════════════════════════════════════════════
//  digestFn: writes `digestLen` bytes; blockLen is the hash's block size.
template <typename DigestFn>
inline void hmac_raw(DigestFn digestFn, size_t digestLen, size_t blockLen,
                     const uint8_t* key, size_t keyLen,
                     const uint8_t* msg, size_t msgLen, uint8_t* out) {
    std::vector<uint8_t> k(blockLen, 0);
    if (keyLen > blockLen) {                 // keys longer than a block are hashed first
        std::vector<uint8_t> kh(digestLen);
        digestFn(key, keyLen, kh.data());
        std::memcpy(k.data(), kh.data(), digestLen);
    } else {
        std::memcpy(k.data(), key, keyLen);
    }
    std::vector<uint8_t> ipad(blockLen), opad(blockLen);
    for (size_t i = 0; i < blockLen; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }

    std::vector<uint8_t> inner(blockLen + msgLen);
    std::memcpy(inner.data(), ipad.data(), blockLen);
    if (msgLen) std::memcpy(inner.data() + blockLen, msg, msgLen);
    std::vector<uint8_t> innerHash(digestLen);
    digestFn(inner.data(), inner.size(), innerHash.data());

    std::vector<uint8_t> outer(blockLen + digestLen);
    std::memcpy(outer.data(), opad.data(), blockLen);
    std::memcpy(outer.data() + blockLen, innerHash.data(), digestLen);
    digestFn(outer.data(), outer.size(), out);
}

inline void hmac_sha256_raw(const uint8_t* key, size_t keyLen,
                            const uint8_t* msg, size_t msgLen, uint8_t out[32]) {
    hmac_raw([](const uint8_t* m, size_t n, uint8_t* o){ sha256_raw(m, n, o); },
             32, 64, key, keyLen, msg, msgLen, out);
}

} // namespace bantu_native
