#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  aes_gcm.hpp — AES-128 in Galois/Counter Mode (NIST SP 800-38D).
//
//  WHY THIS EXISTS
//  ---------------
//  RFC 8291 fixes Web Push payload encryption to aes128gcm. libsodium provides
//  only (X)ChaCha20-Poly1305, so there is nothing to delegate to. See
//  docs/pwa-research.md §5.1 for why this suite is written from scratch.
//
//  DESIGN
//  ------
//   • ENCRYPT-ONLY AES. GCM uses the forward cipher in both directions —
//     confidentiality is CTR mode, authentication is GHASH — so `open` works
//     without the inverse cipher. That removes InvMixColumns and the inverse
//     S-box entirely.
//   • GHASH is the table-free bitwise algorithm. The 4-bit ("Shoup") table
//     variant is the construction targeted by published GHASH cache-timing
//     attacks, because the table is indexed by data mixed with the hash subkey.
//     The bitwise loop has NO data-dependent memory access. Worst case here is a
//     4 KB body — 256 blocks × 128 iterations, tens of microseconds. Speed is
//     irrelevant at push volumes; simplicity and timing safety are not.
//   • 96-BIT IV ONLY. Web Push always uses a 12-byte nonce, so the GHASH-derived
//     J0 path for other IV lengths is deliberately unimplemented and rejected —
//     one fewer thing to get wrong.
//   • The S-box is a 256-byte data-indexed table (4 cache lines). That is a
//     cache-timing surface, and it is accepted: the threat model is a remote
//     attacker, not a co-resident one. A bitsliced AES is not warranted here.
//
//  Validated against FIPS 197 Appendix B and McGrew & Viega's GCM test cases.
// ════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace bantu_aesgcm {

// ── AES-128 ─────────────────────────────────────────────────────────────────

static const uint8_t kSbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static inline uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

struct Aes128 {
    uint8_t rk[176];   // 11 round keys × 16 bytes
};

static inline void aes128_expand(Aes128& a, const uint8_t key[16]) {
    std::memcpy(a.rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { a.rk[i-4], a.rk[i-3], a.rk[i-2], a.rk[i-1] };
        if (i % 16 == 0) {
            uint8_t tmp = t[0];                                   // RotWord
            t[0] = kSbox[t[1]]; t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]]; t[3] = kSbox[tmp];
            t[0] ^= rcon;
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++) a.rk[i + j] = a.rk[i - 16 + j] ^ t[j];
    }
}

// Forward cipher. `in` and `out` may alias.
static inline void aes128_encrypt_block(const Aes128& a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a.rk[i];

    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = kSbox[s[i]];          // SubBytes

        uint8_t t[16];                                            // ShiftRows
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[r + 4 * c] = s[r + 4 * ((c + r) & 3)];
        std::memcpy(s, t, 16);

        if (round != 10) {                                        // MixColumns
            for (int c = 0; c < 4; c++) {
                uint8_t* p = s + 4 * c;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                uint8_t x = a0 ^ a1 ^ a2 ^ a3;
                p[0] = (uint8_t)(a0 ^ x ^ xtime((uint8_t)(a0 ^ a1)));
                p[1] = (uint8_t)(a1 ^ x ^ xtime((uint8_t)(a1 ^ a2)));
                p[2] = (uint8_t)(a2 ^ x ^ xtime((uint8_t)(a2 ^ a3)));
                p[3] = (uint8_t)(a3 ^ x ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= a.rk[16 * round + i]; // AddRoundKey
    }
    std::memcpy(out, s, 16);
}

// ── GHASH over GF(2^128) ────────────────────────────────────────────────────
//
// GCM uses the "reversed" bit convention: bit 0 of a field element is the MOST
// significant bit of byte 0. So multiplication is a RIGHT shift with the
// reduction polynomial 0xE1 << 120. Getting this backwards yields a
// self-consistent but wrong tag that only a published vector detects.

struct Block128 { uint64_t hi, lo; };

static inline Block128 load_be(const uint8_t b[16]) {
    Block128 r{0, 0};
    for (int i = 0; i < 8; i++)  r.hi = (r.hi << 8) | b[i];
    for (int i = 8; i < 16; i++) r.lo = (r.lo << 8) | b[i];
    return r;
}
static inline void store_be(const Block128& v, uint8_t b[16]) {
    for (int i = 0; i < 8; i++) b[i]     = (uint8_t)(v.hi >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) b[8 + i] = (uint8_t)(v.lo >> (56 - 8 * i));
}

// z = x • y in GF(2^128). Bitwise, branch-free, no tables.
static inline Block128 gf_mul(Block128 x, Block128 y) {
    Block128 z{0, 0};
    Block128 v = y;
    for (int i = 0; i < 128; i++) {
        // bit i of x, counting from the most significant bit
        uint64_t bit = (i < 64) ? ((x.hi >> (63 - i)) & 1)
                                : ((x.lo >> (127 - i)) & 1);
        uint64_t m = (uint64_t)0 - bit;
        z.hi ^= v.hi & m;
        z.lo ^= v.lo & m;

        uint64_t lsb  = v.lo & 1;
        v.lo = (v.lo >> 1) | (v.hi << 63);
        v.hi = v.hi >> 1;
        v.hi ^= (uint64_t)0xE100000000000000ULL & ((uint64_t)0 - lsb);
    }
    return z;
}

struct Ghash {
    Block128 H;
    Block128 acc;
};

static inline void ghash_init(Ghash& g, const uint8_t h[16]) {
    g.H = load_be(h);
    g.acc.hi = g.acc.lo = 0;
}
// Absorb `len` bytes, zero-padding the final partial block.
static inline void ghash_update(Ghash& g, const uint8_t* data, size_t len) {
    uint8_t block[16];
    size_t off = 0;
    while (off < len) {
        size_t n = len - off < 16 ? len - off : 16;
        std::memset(block, 0, 16);
        std::memcpy(block, data + off, n);
        Block128 b = load_be(block);
        g.acc.hi ^= b.hi;
        g.acc.lo ^= b.lo;
        g.acc = gf_mul(g.acc, g.H);
        off += n;
    }
}
// Absorb the length block: len(A) and len(C) in BITS, each 64-bit big-endian.
static inline void ghash_lengths(Ghash& g, uint64_t aad_len, uint64_t ct_len) {
    uint8_t block[16];
    uint64_t abits = aad_len * 8, cbits = ct_len * 8;
    for (int i = 0; i < 8; i++) block[i]     = (uint8_t)(abits >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) block[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    Block128 b = load_be(block);
    g.acc.hi ^= b.hi;
    g.acc.lo ^= b.lo;
    g.acc = gf_mul(g.acc, g.H);
}

// ── GCM ─────────────────────────────────────────────────────────────────────

// counter block = J0 with the trailing 32-bit big-endian counter set to `n`.
static inline void counter_block(const uint8_t j0[16], uint32_t n, uint8_t out[16]) {
    std::memcpy(out, j0, 12);
    out[12] = (uint8_t)(n >> 24);
    out[13] = (uint8_t)(n >> 16);
    out[14] = (uint8_t)(n >> 8);
    out[15] = (uint8_t)n;
}

static inline void ct_xor(uint8_t* dst, const uint8_t* a, const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = a[i] ^ b[i];
}

// Constant-time 16-byte compare.
static inline bool ct_eq16(const uint8_t* a, const uint8_t* b) {
    uint8_t d = 0;
    for (int i = 0; i < 16; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

// AES-128-GCM seal. `iv` MUST be 12 bytes. Output is ciphertext || 16-byte tag.
// Returns false if the IV length is not 12.
static inline bool gcm_seal(const uint8_t key[16], const uint8_t* iv, size_t iv_len,
                            const uint8_t* aad, size_t aad_len,
                            const uint8_t* pt, size_t pt_len,
                            std::vector<uint8_t>& out) {
    if (iv_len != 12) return false;                 // 96-bit IV only, by design

    Aes128 a;
    aes128_expand(a, key);

    uint8_t h[16] = {0};
    aes128_encrypt_block(a, h, h);                  // H = AES_K(0^128)

    uint8_t j0[16];
    std::memcpy(j0, iv, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1; // J0 = IV || 0^31 || 1

    out.assign(pt_len + 16, 0);

    // The first data block uses inc32(J0), i.e. counter value 2 — J0 itself
    // carries counter 1 and is reserved for the tag mask. Starting the data at
    // J0 is a self-consistent bug that only a published vector detects.
    uint8_t ks[16];
    for (size_t off = 0, ctr = 2; off < pt_len; off += 16, ctr++) {
        uint8_t cb[16];
        counter_block(j0, (uint32_t)ctr, cb);
        aes128_encrypt_block(a, cb, ks);
        size_t n = pt_len - off < 16 ? pt_len - off : 16;
        ct_xor(out.data() + off, pt + off, ks, n);
    }

    Ghash g;
    ghash_init(g, h);
    ghash_update(g, aad, aad_len);
    ghash_update(g, out.data(), pt_len);
    ghash_lengths(g, aad_len, pt_len);

    uint8_t s[16], ej0[16];
    store_be(g.acc, s);
    aes128_encrypt_block(a, j0, ej0);
    ct_xor(out.data() + pt_len, s, ej0, 16);        // tag = GHASH ^ AES_K(J0)

    std::memset(ks, 0, sizeof ks);
    std::memset(&a, 0, sizeof a);
    return true;
}

// AES-128-GCM open. `in` is ciphertext || tag. Returns false on ANY failure —
// a false return means REJECT, never "empty plaintext".
static inline bool gcm_open(const uint8_t key[16], const uint8_t* iv, size_t iv_len,
                            const uint8_t* aad, size_t aad_len,
                            const uint8_t* in, size_t in_len,
                            std::vector<uint8_t>& out) {
    if (iv_len != 12) return false;
    if (in_len < 16) return false;
    size_t ct_len = in_len - 16;

    Aes128 a;
    aes128_expand(a, key);

    uint8_t h[16] = {0};
    aes128_encrypt_block(a, h, h);

    uint8_t j0[16];
    std::memcpy(j0, iv, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

    Ghash g;
    ghash_init(g, h);
    ghash_update(g, aad, aad_len);
    ghash_update(g, in, ct_len);
    ghash_lengths(g, aad_len, ct_len);

    uint8_t s[16], ej0[16], tag[16];
    store_be(g.acc, s);
    aes128_encrypt_block(a, j0, ej0);
    ct_xor(tag, s, ej0, 16);

    if (!ct_eq16(tag, in + ct_len)) {               // verify BEFORE decrypting
        std::memset(&a, 0, sizeof a);
        return false;
    }

    out.assign(ct_len, 0);
    uint8_t ks[16];
    for (size_t off = 0, ctr = 2; off < ct_len; off += 16, ctr++) {   // see gcm_seal
        uint8_t cb[16];
        counter_block(j0, (uint32_t)ctr, cb);
        aes128_encrypt_block(a, cb, ks);
        size_t n = ct_len - off < 16 ? ct_len - off : 16;
        ct_xor(out.data() + off, in + off, ks, n);
    }

    std::memset(ks, 0, sizeof ks);
    std::memset(&a, 0, sizeof a);
    return true;
}

} // namespace bantu_aesgcm
