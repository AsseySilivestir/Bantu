#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  p256.hpp — NIST P-256 (secp256r1): ECDH, ECDSA-SHA256, key generation.
//
//  WHY THIS EXISTS
//  ---------------
//  Web Push (RFC 8291) mandates P-256. libsodium — the project's normal source
//  of encryption primitives (DECISIONS D11) — offers only Curve25519/Ed25519, so
//  there is nothing to delegate to, and linking OpenSSL would make libcrypto a
//  hard runtime dependency of every Bantu binary. Hence a self-contained
//  implementation, compiled unconditionally. See docs/pwa-research.md §5.1 and
//  crypto-suite/DECISIONS.md D14 for the full rationale.
//
//  Bantu's numbers are float64, so a pure-Bantu fallback is impossible here
//  (same reason SHA-512 is native-only). Nothing in this file is reachable from
//  Bantu except through the `webpush_*` builtins.
//
//  DESIGN
//  ------
//   • Field elements are 4 × uint64_t limbs, little-endian limb order, held in
//     MONTGOMERY form. Reduction is CIOS Montgomery — uniform, with a single
//     conditional subtraction — rather than the FIPS 186-4 D.2 Solinas
//     reduction, whose multi-step correction is a classic source of
//     "wrong for a narrow input range" bugs.
//   • Every modulus-derived constant (n0', R mod m, R² mod m) is COMPUTED at
//     init from the modulus itself, never transcribed. Only p, n, b, Gx and Gy
//     are literals, as big-endian hex, so they can be eyeballed against
//     FIPS 186-4 D.1.2.3.
//   • Point arithmetic uses the Renes–Costello–Batina COMPLETE formulas for
//     a = −3 (EUROCRYPT 2015). They are exception-free: no special cases for
//     P == Q, P == −Q, or the point at infinity. Incomplete (Jacobian) formulas
//     need those cases handled explicitly, and they are hit on a sparse,
//     scalar-dependent set of inputs — the kind of bug that passes a test suite
//     and then corrupts one signature in ten thousand.
//   • Scalar multiplication is plain double-and-add over all 256 bits with a
//     constant-time cmov select. Because the formulas are complete there is no
//     table, hence NO secret-dependent memory index and no secret-dependent
//     branch anywhere.
//   • Inversion is Fermat (a^(m−2)) over a PUBLIC constant exponent. Binary
//     extended Euclid is rejected: ECDSA inverts the secret k, and xgcd's
//     iteration count depends on its input.
//   • ECDSA nonces are deterministic (RFC 6979). That removes the catastrophic
//     nonce-reuse failure mode for a long-lived VAPID key, and — the deciding
//     argument — it makes signing byte-comparable against published vectors.
//
//  THREAT MODEL
//  ------------
//  A remote attacker against a long-lived server-side VAPID signing key. The
//  mitigations chosen (no secret branches, no secret memory indices, complete
//  formulas, deterministic nonces, validated point import) target that. A
//  co-resident attacker with cache observation is explicitly OUT of scope, so
//  scalar blinding and projective randomisation are deliberately absent. C++
//  offers no constant-time guarantee; this is best-effort and says so.
// ════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "crypto_native.hpp"   // sha256_raw / hmac_sha256_raw (RFC 6979 DRBG)

namespace bantu_p256 {

// ════════════════════════════════════════════════════════════════════════════
//  Word primitives
//
//  The 64×64→128 multiply is the ONLY thing that varies by platform. Everything
//  above it is identical, so the portable path doubles as a differential test of
//  the __int128 path (selftest cross-checks them).
// ════════════════════════════════════════════════════════════════════════════

#if defined(__SIZEOF_INT128__) && !defined(BANTU_P256_NO_INT128)
  #define BANTU_P256_HAVE_INT128 1
  typedef unsigned __int128 p256_u128;
  // NOTE: never divide or take a modulus of a u128 — that pulls in __udivti3.
  // Only *, + and shifts are used here.
  static inline void mul64(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo) {
      p256_u128 p = (p256_u128)a * (p256_u128)b;
      *lo = (uint64_t)p;
      *hi = (uint64_t)(p >> 64);
  }
#else
  #define BANTU_P256_HAVE_INT128 0
  // Schoolbook 32×32→64. Used on any target without __int128 (e.g. a 32-bit
  // MinGW build) and, in the selftest, as an independent check of the fast path.
  static inline void mul64(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo) {
      uint64_t a0 = a & 0xFFFFFFFFull, a1 = a >> 32;
      uint64_t b0 = b & 0xFFFFFFFFull, b1 = b >> 32;
      uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
      uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFull) + (p10 & 0xFFFFFFFFull);
      *lo = (p00 & 0xFFFFFFFFull) | (mid << 32);
      *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
  }
#endif

// Portable schoolbook multiply, always available — the selftest runs the whole
// field layer through this and asserts it agrees with mul64.
static inline void mul64_portable(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo) {
    uint64_t a0 = a & 0xFFFFFFFFull, a1 = a >> 32;
    uint64_t b0 = b & 0xFFFFFFFFull, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFull) + (p10 & 0xFFFFFFFFull);
    *lo = (p00 & 0xFFFFFFFFull) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

// out = a + b + cin ; returns the carry out (0 or 1). Branch-free.
static inline uint64_t addc64(uint64_t a, uint64_t b, uint64_t cin, uint64_t* out) {
    uint64_t s1 = a + b;
    uint64_t c1 = (uint64_t)(s1 < a);
    uint64_t s2 = s1 + cin;
    uint64_t c2 = (uint64_t)(s2 < s1);
    *out = s2;
    return c1 | c2;               // c1 and c2 are never both 1
}

// out = a - b - bin ; returns the borrow out (0 or 1). Branch-free.
static inline uint64_t subb64(uint64_t a, uint64_t b, uint64_t bin, uint64_t* out) {
    uint64_t d1 = a - b;
    uint64_t b1 = (uint64_t)(a < b);
    uint64_t d2 = d1 - bin;
    uint64_t b2 = (uint64_t)(d1 < bin);
    *out = d2;
    return b1 | b2;
}

// 0 → 0, anything else → all-ones. Used to build branch-free selects.
static inline uint64_t mask_nonzero(uint64_t x) {
    return (uint64_t)0 - ((x | (uint64_t)0 - x) >> 63);
}
static inline uint64_t mask_of_bit(uint64_t bit) {   // bit ∈ {0,1}
    return (uint64_t)0 - (bit & 1);
}

// Overwrite a buffer the compiler is not allowed to elide.
static inline void secure_zero(void* p, size_t n) {
    volatile unsigned char* q = (volatile unsigned char*)p;
    while (n--) *q++ = 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  Multi-precision helpers over 4 limbs
// ════════════════════════════════════════════════════════════════════════════

static inline uint64_t add4(uint64_t r[4], const uint64_t a[4], const uint64_t b[4]) {
    uint64_t c = 0;
    for (int i = 0; i < 4; i++) c = addc64(a[i], b[i], c, &r[i]);
    return c;
}
static inline uint64_t sub4(uint64_t r[4], const uint64_t a[4], const uint64_t b[4]) {
    uint64_t br = 0;
    for (int i = 0; i < 4; i++) br = subb64(a[i], b[i], br, &r[i]);
    return br;
}
static inline void cmov4(uint64_t r[4], const uint64_t a[4], uint64_t bit) {
    uint64_t m = mask_of_bit(bit);
    for (int i = 0; i < 4; i++) r[i] = (r[i] & ~m) | (a[i] & m);
}
static inline bool is_zero4(const uint64_t a[4]) {
    return (a[0] | a[1] | a[2] | a[3]) == 0;
}
static inline bool eq4(const uint64_t a[4], const uint64_t b[4]) {
    uint64_t d = 0;
    for (int i = 0; i < 4; i++) d |= (a[i] ^ b[i]);
    return d == 0;
}
// a < b ?  (constant-time: computes the borrow of a - b)
static inline bool lt4(const uint64_t a[4], const uint64_t b[4]) {
    uint64_t t[4];
    return sub4(t, a, b) != 0;
}

// 32-byte big-endian → limbs (limb 0 is least significant).
static inline void be_to_limbs(const uint8_t in[32], uint64_t out[4]) {
    for (int i = 0; i < 4; i++) {
        uint64_t w = 0;
        const uint8_t* p = in + (3 - i) * 8;      // limb i sits at the far end
        for (int j = 0; j < 8; j++) w = (w << 8) | p[j];
        out[i] = w;
    }
}
// limbs → 32-byte big-endian. Explicit byte assembly, never a reinterpret_cast.
static inline void limbs_to_be(const uint64_t in[4], uint8_t out[32]) {
    for (int i = 0; i < 4; i++) {
        uint64_t w = in[3 - i];
        for (int j = 0; j < 8; j++) out[i * 8 + j] = (uint8_t)(w >> (56 - 8 * j));
    }
}
static inline int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// 64-char big-endian hex → limbs. Only used for the five curve literals.
static inline void hex_to_limbs(const char* hex, uint64_t out[4]) {
    uint8_t b[32];
    for (int i = 0; i < 32; i++)
        b[i] = (uint8_t)((hexval(hex[2 * i]) << 4) | hexval(hex[2 * i + 1]));
    be_to_limbs(b, out);
}

// ════════════════════════════════════════════════════════════════════════════
//  Montgomery arithmetic over a 256-bit odd modulus
// ════════════════════════════════════════════════════════════════════════════

struct Mod {
    uint64_t m[4];        // the modulus (2^255 < m < 2^256)
    uint64_t n0inv;       // -m^-1 mod 2^64
    uint64_t one[4];      // R mod m         (Montgomery form of 1)
    uint64_t rr[4];       // R^2 mod m       (used by to_mont)
    uint64_t exp_m2[4];   // m - 2           (Fermat inversion exponent)
};

// r = a + b mod M
static inline void mod_add(uint64_t r[4], const uint64_t a[4], const uint64_t b[4], const Mod& M) {
    uint64_t carry = add4(r, a, b);
    uint64_t t[4];
    uint64_t borrow = sub4(t, r, M.m);
    // Subtract m when the sum overflowed 2^256, or when it is already >= m.
    cmov4(r, t, carry | (borrow ^ 1));
}
// r = a - b mod M
static inline void mod_sub(uint64_t r[4], const uint64_t a[4], const uint64_t b[4], const Mod& M) {
    uint64_t borrow = sub4(r, a, b);
    uint64_t t[4];
    add4(t, r, M.m);
    cmov4(r, t, borrow);
}
// r = 2a mod M
static inline void mod_dbl(uint64_t r[4], const uint64_t a[4], const Mod& M) {
    mod_add(r, a, a, M);
}
// r = -a mod M
static inline void mod_neg(uint64_t r[4], const uint64_t a[4], const Mod& M) {
    uint64_t z[4] = {0, 0, 0, 0};
    uint64_t nz = mask_nonzero(a[0] | a[1] | a[2] | a[3]);
    uint64_t t[4];
    sub4(t, M.m, a);
    for (int i = 0; i < 4; i++) r[i] = (t[i] & nz) | (z[i] & ~nz);   // -0 == 0
}

// Montgomery multiplication, CIOS. r = a*b*R^-1 mod M, with a, b < M.
//
// The 128-bit accumulator never overflows: the worst case per step is
// (2^64-1)^2 + (2^64-1) + (2^64-1) = 2^128 - 1 exactly.
template <void (*MUL)(uint64_t, uint64_t, uint64_t*, uint64_t*)>
static inline void mont_mul_impl(uint64_t r[4], const uint64_t a[4], const uint64_t b[4], const Mod& M) {
    uint64_t t[6] = {0, 0, 0, 0, 0, 0};

    for (int i = 0; i < 4; i++) {
        // ── t += a * b[i] ──
        uint64_t carry = 0;
        for (int j = 0; j < 4; j++) {
            uint64_t hi, lo, c;
            MUL(a[j], b[i], &hi, &lo);
            c = addc64(lo, t[j], 0, &lo);   hi += c;
            c = addc64(lo, carry, 0, &lo);  hi += c;
            t[j]  = lo;
            carry = hi;
        }
        t[5] = addc64(t[4], carry, 0, &t[4]);

        // ── m = t[0] * n0'  →  t += m*M, which zeroes the low limb ──
        uint64_t mm = t[0] * M.n0inv;
        uint64_t carry2;
        {
            uint64_t hi, lo, c;
            MUL(mm, M.m[0], &hi, &lo);
            c = addc64(lo, t[0], 0, &lo);   hi += c;    // lo is now 0 by construction
            carry2 = hi;
        }
        for (int j = 1; j < 4; j++) {
            uint64_t hi, lo, c;
            MUL(mm, M.m[j], &hi, &lo);
            c = addc64(lo, t[j], 0, &lo);    hi += c;
            c = addc64(lo, carry2, 0, &lo);  hi += c;
            t[j - 1] = lo;
            carry2   = hi;
        }
        uint64_t c = addc64(t[4], carry2, 0, &t[3]);
        t[4] = t[5] + c;
    }

    // t[0..3] < 2m and t[4] ∈ {0,1}: one conditional subtraction finishes it.
    uint64_t d[4];
    uint64_t borrow = sub4(d, t, M.m);
    uint64_t need   = t[4] | (borrow ^ 1);
    uint64_t bit    = (need | ((uint64_t)0 - need)) >> 63;
    for (int i = 0; i < 4; i++) r[i] = t[i];
    cmov4(r, d, bit);

    secure_zero(t, sizeof t);
}

static inline void mont_mul(uint64_t r[4], const uint64_t a[4], const uint64_t b[4], const Mod& M) {
    mont_mul_impl<mul64>(r, a, b, M);
}
static inline void mont_mul_portable(uint64_t r[4], const uint64_t a[4], const uint64_t b[4], const Mod& M) {
    mont_mul_impl<mul64_portable>(r, a, b, M);
}
static inline void mont_sqr(uint64_t r[4], const uint64_t a[4], const Mod& M) {
    mont_mul(r, a, a, M);
}
static inline void to_mont(uint64_t r[4], const uint64_t a[4], const Mod& M) {
    mont_mul(r, a, M.rr, M);
}
static inline void from_mont(uint64_t r[4], const uint64_t a[4], const Mod& M) {
    uint64_t one[4] = {1, 0, 0, 0};
    mont_mul(r, a, one, M);
}

// r = a^e mod M (Montgomery domain), left-to-right square-and-multiply over a
// PUBLIC exponent. Used only for Fermat inversion, so `e` is a fixed constant
// and the operation sequence is input-independent.
static inline void mont_pow(uint64_t r[4], const uint64_t a[4], const uint64_t e[4], const Mod& M) {
    uint64_t acc[4];
    std::memcpy(acc, M.one, sizeof acc);
    for (int i = 255; i >= 0; i--) {
        mont_sqr(acc, acc, M);
        uint64_t bit = (e[i >> 6] >> (i & 63)) & 1;
        uint64_t t[4];
        mont_mul(t, acc, a, M);
        cmov4(acc, t, bit);
    }
    std::memcpy(r, acc, sizeof acc);
}
// r = a^-1 mod M, via Fermat. a == 0 yields 0.
static inline void mont_inv(uint64_t r[4], const uint64_t a[4], const Mod& M) {
    mont_pow(r, a, M.exp_m2, M);
}

// Build every derived constant from the modulus alone.
static inline void mod_init(Mod& M, const char* modulus_hex) {
    hex_to_limbs(modulus_hex, M.m);

    // n0' = -m^-1 mod 2^64, by Newton iteration (doubles precision each step:
    // 1 → 2 → 4 → 8 → 16 → 32 → 64 bits). m is odd, so x = 1 is correct mod 2.
    uint64_t inv = 1;
    for (int i = 0; i < 6; i++) inv *= (uint64_t)2 - M.m[0] * inv;
    M.n0inv = (uint64_t)0 - inv;

    // R mod m == 2^256 - m, because 2^255 < m < 2^256 for both p and n.
    // Computing 0 - m over 4 limbs gives exactly that.
    uint64_t zero[4] = {0, 0, 0, 0};
    sub4(M.one, zero, M.m);

    // R^2 mod m = (R mod m) * 2^256 mod m — i.e. 256 modular doublings. Derived
    // rather than transcribed, so a mistyped constant is impossible.
    std::memcpy(M.rr, M.one, sizeof M.rr);
    for (int i = 0; i < 256; i++) mod_dbl(M.rr, M.rr, M);

    uint64_t two[4] = {2, 0, 0, 0};
    sub4(M.exp_m2, M.m, two);
}

// ════════════════════════════════════════════════════════════════════════════
//  Curve constants — FIPS 186-4 D.1.2.3. These five literals are the only
//  transcribed values in the file; everything else is derived.
// ════════════════════════════════════════════════════════════════════════════

static const char* kP_HEX  = "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff";
static const char* kN_HEX  = "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551";
static const char* kB_HEX  = "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b";
static const char* kGX_HEX = "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296";
static const char* kGY_HEX = "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";

struct Curve {
    Mod P;                 // field modulus
    Mod N;                 // group order
    uint64_t b_mont[4];    // curve b, Montgomery form
    uint64_t b3_mont[4];   // 3b, for the RCB formulas
    uint64_t gx_mont[4];
    uint64_t gy_mont[4];
};

static inline const Curve& curve() {
    static Curve C = [] {
        Curve c;
        mod_init(c.P, kP_HEX);
        mod_init(c.N, kN_HEX);
        uint64_t t[4];
        hex_to_limbs(kB_HEX,  t); to_mont(c.b_mont,  t, c.P);
        hex_to_limbs(kGX_HEX, t); to_mont(c.gx_mont, t, c.P);
        hex_to_limbs(kGY_HEX, t); to_mont(c.gy_mont, t, c.P);
        mod_add(c.b3_mont, c.b_mont, c.b_mont, c.P);
        mod_add(c.b3_mont, c.b3_mont, c.b_mont, c.P);
        return c;
    }();
    return C;
}

// ════════════════════════════════════════════════════════════════════════════
//  Points
//
//  Projective (X : Y : Z), all coordinates in Montgomery form.
//  The identity is (0 : 1 : 0).
// ════════════════════════════════════════════════════════════════════════════

struct Point {
    uint64_t X[4], Y[4], Z[4];
};

static inline void point_identity(Point& r) {
    const Curve& C = curve();
    std::memset(r.X, 0, sizeof r.X);
    std::memcpy(r.Y, C.P.one, sizeof r.Y);
    std::memset(r.Z, 0, sizeof r.Z);
}
static inline bool point_is_identity(const Point& p) {
    return is_zero4(p.Z);
}
static inline void point_cmov(Point& r, const Point& a, uint64_t bit) {
    cmov4(r.X, a.X, bit);
    cmov4(r.Y, a.Y, bit);
    cmov4(r.Z, a.Z, bit);
}
static inline void point_neg(Point& r, const Point& a) {
    const Curve& C = curve();
    std::memcpy(r.X, a.X, sizeof r.X);
    mod_neg(r.Y, a.Y, C.P);
    std::memcpy(r.Z, a.Z, sizeof r.Z);
}

// Renes–Costello–Batina, Algorithm 4 — complete addition for a = −3.
// Exception-free for every input pair, including P == Q and the identity.
static inline void point_add(Point& R, const Point& p, const Point& q) {
    const Curve& C = curve();
    const Mod& F = C.P;
    uint64_t t0[4], t1[4], t2[4], t3[4], t4[4], X3[4], Y3[4], Z3[4];

    mont_mul(t0, p.X, q.X, F);            // t0 = X1*X2
    mont_mul(t1, p.Y, q.Y, F);            // t1 = Y1*Y2
    mont_mul(t2, p.Z, q.Z, F);            // t2 = Z1*Z2
    mod_add(t3, p.X, p.Y, F);             // t3 = X1+Y1
    mod_add(t4, q.X, q.Y, F);             // t4 = X2+Y2
    mont_mul(t3, t3, t4, F);              // t3 = t3*t4
    mod_add(t4, t0, t1, F);               // t4 = t0+t1
    mod_sub(t3, t3, t4, F);               // t3 = t3-t4
    mod_add(t4, p.Y, p.Z, F);             // t4 = Y1+Z1
    mod_add(X3, q.Y, q.Z, F);             // X3 = Y2+Z2
    mont_mul(t4, t4, X3, F);              // t4 = t4*X3
    mod_add(X3, t1, t2, F);               // X3 = t1+t2
    mod_sub(t4, t4, X3, F);               // t4 = t4-X3
    mod_add(X3, p.X, p.Z, F);             // X3 = X1+Z1
    mod_add(Y3, q.X, q.Z, F);             // Y3 = X2+Z2
    mont_mul(X3, X3, Y3, F);              // X3 = X3*Y3
    mod_add(Y3, t0, t2, F);               // Y3 = t0+t2
    mod_sub(Y3, X3, Y3, F);               // Y3 = X3-Y3
    mont_mul(Z3, C.b_mont, t2, F);       // Z3 = b3*t2
    mod_sub(X3, Y3, Z3, F);               // X3 = Y3-Z3
    mod_add(Z3, X3, X3, F);               // Z3 = X3+X3
    mod_add(X3, X3, Z3, F);               // X3 = X3+Z3
    mod_sub(Z3, t1, X3, F);               // Z3 = t1-X3
    mod_add(X3, t1, X3, F);               // X3 = t1+X3
    mont_mul(Y3, C.b_mont, Y3, F);       // Y3 = b3*Y3
    mod_add(t1, t2, t2, F);               // t1 = t2+t2
    mod_add(t2, t1, t2, F);               // t2 = t1+t2
    mod_sub(Y3, Y3, t2, F);               // Y3 = Y3-t2
    mod_sub(Y3, Y3, t0, F);               // Y3 = Y3-t0
    mod_add(t1, Y3, Y3, F);               // t1 = Y3+Y3
    mod_add(Y3, t1, Y3, F);               // Y3 = t1+Y3
    mod_add(t1, t0, t0, F);               // t1 = t0+t0
    mod_add(t0, t1, t0, F);               // t0 = t1+t0
    mod_sub(t0, t0, t2, F);               // t0 = t0-t2
    mont_mul(t1, t4, Y3, F);              // t1 = t4*Y3
    mont_mul(t2, t0, Y3, F);              // t2 = t0*Y3
    mont_mul(Y3, X3, Z3, F);              // Y3 = X3*Z3
    mod_add(Y3, Y3, t2, F);               // Y3 = Y3+t2
    mont_mul(X3, t3, X3, F);              // X3 = t3*X3
    mod_sub(X3, X3, t1, F);               // X3 = X3-t1
    mont_mul(Z3, t4, Z3, F);              // Z3 = t4*Z3
    mont_mul(t1, t3, t0, F);              // t1 = t3*t0
    mod_add(Z3, Z3, t1, F);               // Z3 = Z3+t1

    std::memcpy(R.X, X3, sizeof X3);
    std::memcpy(R.Y, Y3, sizeof Y3);
    std::memcpy(R.Z, Z3, sizeof Z3);
}

// Renes–Costello–Batina, Algorithm 6 — complete doubling for a = −3.
static inline void point_dbl(Point& R, const Point& p) {
    const Curve& C = curve();
    const Mod& F = C.P;
    uint64_t t0[4], t1[4], t2[4], t3[4], X3[4], Y3[4], Z3[4];

    mont_mul(t0, p.X, p.X, F);            // t0 = X*X
    mont_mul(t1, p.Y, p.Y, F);            // t1 = Y*Y
    mont_mul(t2, p.Z, p.Z, F);            // t2 = Z*Z
    mont_mul(t3, p.X, p.Y, F);            // t3 = X*Y
    mod_add(t3, t3, t3, F);               // t3 = t3+t3
    mont_mul(Z3, p.X, p.Z, F);            // Z3 = X*Z
    mod_add(Z3, Z3, Z3, F);               // Z3 = Z3+Z3
    mont_mul(Y3, C.b_mont, t2, F);       // Y3 = b3*t2
    mod_sub(Y3, Y3, Z3, F);               // Y3 = Y3-Z3
    mod_add(X3, Y3, Y3, F);               // X3 = Y3+Y3
    mod_add(Y3, X3, Y3, F);               // Y3 = X3+Y3
    mod_sub(X3, t1, Y3, F);               // X3 = t1-Y3
    mod_add(Y3, t1, Y3, F);               // Y3 = t1+Y3
    mont_mul(Y3, X3, Y3, F);              // Y3 = X3*Y3
    mont_mul(X3, X3, t3, F);              // X3 = X3*t3
    mod_add(t3, t2, t2, F);               // t3 = t2+t2
    mod_add(t2, t2, t3, F);               // t2 = t2+t3
    mont_mul(Z3, C.b_mont, Z3, F);       // Z3 = b3*Z3
    mod_sub(Z3, Z3, t2, F);               // Z3 = Z3-t2
    mod_sub(Z3, Z3, t0, F);               // Z3 = Z3-t0
    mod_add(t3, Z3, Z3, F);               // t3 = Z3+Z3
    mod_add(Z3, Z3, t3, F);               // Z3 = Z3+t3
    mod_add(t3, t0, t0, F);               // t3 = t0+t0
    mod_add(t0, t3, t0, F);               // t0 = t3+t0
    mod_sub(t0, t0, t2, F);               // t0 = t0-t2
    mont_mul(t0, t0, Z3, F);              // t0 = t0*Z3
    mod_add(Y3, Y3, t0, F);               // Y3 = Y3+t0
    mont_mul(t0, p.Y, p.Z, F);            // t0 = Y*Z
    mod_add(t0, t0, t0, F);               // t0 = t0+t0
    mont_mul(Z3, t0, Z3, F);              // Z3 = t0*Z3
    mod_sub(X3, X3, Z3, F);               // X3 = X3-Z3
    mont_mul(Z3, t0, t1, F);              // Z3 = t0*t1
    mod_add(Z3, Z3, Z3, F);               // Z3 = Z3+Z3
    mod_add(Z3, Z3, Z3, F);               // Z3 = Z3+Z3

    std::memcpy(R.X, X3, sizeof X3);
    std::memcpy(R.Y, Y3, sizeof Y3);
    std::memcpy(R.Z, Z3, sizeof Z3);
}

// Affine coordinates (Montgomery form), with an explicit infinity flag.
struct Affine {
    uint64_t x[4], y[4];
    bool inf;
};

static inline void point_to_affine(Affine& a, const Point& p) {
    const Mod& F = curve().P;
    if (point_is_identity(p)) { a.inf = true; std::memset(a.x, 0, sizeof a.x); std::memset(a.y, 0, sizeof a.y); return; }
    a.inf = false;
    uint64_t zi[4];
    mont_inv(zi, p.Z, F);
    mont_mul(a.x, p.X, zi, F);
    mont_mul(a.y, p.Y, zi, F);
    secure_zero(zi, sizeof zi);
}
static inline void affine_to_point(Point& p, const Affine& a) {
    const Mod& F = curve().P;
    if (a.inf) { point_identity(p); return; }
    std::memcpy(p.X, a.x, sizeof p.X);
    std::memcpy(p.Y, a.y, sizeof p.Y);
    std::memcpy(p.Z, F.one, sizeof p.Z);
}

// y^2 == x^3 - 3x + b, with x, y in Montgomery form.
static inline bool on_curve_mont(const uint64_t x[4], const uint64_t y[4]) {
    const Curve& C = curve();
    const Mod& F = C.P;
    uint64_t lhs[4], rhs[4], t[4], three_x[4];
    mont_mul(lhs, y, y, F);                 // y^2
    mont_mul(rhs, x, x, F);
    mont_mul(rhs, rhs, x, F);               // x^3
    mod_add(three_x, x, x, F);
    mod_add(three_x, three_x, x, F);        // 3x
    mod_sub(t, rhs, three_x, F);            // x^3 - 3x
    mod_add(rhs, t, C.b_mont, F);           // + b
    return eq4(lhs, rhs);
}

static inline void generator(Point& g) {
    const Curve& C = curve();
    std::memcpy(g.X, C.gx_mont, sizeof g.X);
    std::memcpy(g.Y, C.gy_mont, sizeof g.Y);
    std::memcpy(g.Z, C.P.one,   sizeof g.Z);
}

// R = [k]P, with k a 32-byte big-endian scalar.
//
// Double-and-add over all 256 bits, ALWAYS computing the addition and selecting
// with a constant-time cmov. There is no precomputed table, so no secret value
// ever indexes memory. The complete formulas mean no case ever needs special
// handling — which is exactly why they were chosen.
static inline void scalar_mul(Point& R, const uint8_t k[32], const Point& P) {
    Point acc, tmp;
    point_identity(acc);
    for (int i = 255; i >= 0; i--) {
        point_dbl(acc, acc);
        point_add(tmp, acc, P);
        uint64_t bit = (uint64_t)((k[31 - (i >> 3)] >> (i & 7)) & 1);
        point_cmov(acc, tmp, bit);
    }
    R = acc;
    secure_zero(&tmp, sizeof tmp);
}

static inline void scalar_mul_base(Point& R, const uint8_t k[32]) {
    Point g;
    generator(g);
    scalar_mul(R, k, g);
}

// ════════════════════════════════════════════════════════════════════════════
//  Independent affine reference implementation.
//
//  Deliberately naive: textbook group law, explicit cases, one inversion per
//  operation. NOT constant-time and never used on secret data — it exists so the
//  selftest can cross-check the projective ladder against a second, structurally
//  different implementation. Two independent implementations agreeing on random
//  scalars is a far stronger signal than either passing vectors alone, and it
//  follows the differential-testing pattern already used for the digests (D10).
// ════════════════════════════════════════════════════════════════════════════

static inline void ref_add(Affine& r, const Affine& p, const Affine& q) {
    const Curve& C = curve();
    const Mod& F = C.P;
    if (p.inf) { r = q; return; }
    if (q.inf) { r = p; return; }

    uint64_t lam[4], num[4], den[4], t[4];
    if (eq4(p.x, q.x)) {
        uint64_t negy[4];
        mod_neg(negy, q.y, F);
        if (eq4(p.y, negy) || is_zero4(p.y)) { r.inf = true; return; }
        // lambda = (3x^2 - 3) / 2y
        mont_mul(num, p.x, p.x, F);
        mod_add(t, num, num, F);
        mod_add(num, t, num, F);                 // 3x^2
        uint64_t three[4] = {3, 0, 0, 0}, three_m[4];
        to_mont(three_m, three, F);
        mod_sub(num, num, three_m, F);           // 3x^2 - 3
        mod_add(den, p.y, p.y, F);               // 2y
    } else {
        mod_sub(num, q.y, p.y, F);
        mod_sub(den, q.x, p.x, F);
    }
    uint64_t di[4];
    mont_inv(di, den, F);
    mont_mul(lam, num, di, F);

    uint64_t x3[4], y3[4];
    mont_mul(x3, lam, lam, F);
    mod_sub(x3, x3, p.x, F);
    mod_sub(x3, x3, q.x, F);
    mod_sub(y3, p.x, x3, F);
    mont_mul(y3, lam, y3, F);
    mod_sub(y3, y3, p.y, F);

    r.inf = false;
    std::memcpy(r.x, x3, sizeof x3);
    std::memcpy(r.y, y3, sizeof y3);
}

static inline void ref_scalar_mul(Affine& r, const uint8_t k[32], const Affine& P) {
    Affine acc;
    acc.inf = true;
    std::memset(acc.x, 0, sizeof acc.x);
    std::memset(acc.y, 0, sizeof acc.y);
    bool started = false;
    for (int i = 255; i >= 0; i--) {
        if (started) ref_add(acc, acc, acc);
        int bit = (k[31 - (i >> 3)] >> (i & 7)) & 1;
        if (bit) {
            if (!started) { acc = P; started = true; }
            else          { ref_add(acc, acc, P); }
        }
    }
    r = acc;
}

// ════════════════════════════════════════════════════════════════════════════
//  SEC1 encoding
// ════════════════════════════════════════════════════════════════════════════

// 0x04 || X || Y, 65 bytes.
static inline void export_public(const Point& p, uint8_t out[65]) {
    Affine a;
    point_to_affine(a, p);
    const Mod& F = curve().P;
    uint64_t x[4], y[4];
    from_mont(x, a.x, F);
    from_mont(y, a.y, F);
    out[0] = 0x04;
    limbs_to_be(x, out + 1);
    limbs_to_be(y, out + 33);
}

// Import and FULLY VALIDATE an uncompressed public key.
//
// This is a security control, not a formality: in Web Push the `p256dh` value
// comes from the client, so an unvalidated import is an invalid-curve attack. We
// require the uncompressed marker, X and Y strictly below p (rejecting
// non-canonical encodings), and the point to satisfy the curve equation. P-256
// has cofactor 1, so on-curve plus not-identity is sufficient — no small
// subgroup check is needed.
static inline bool import_public(const uint8_t in[65], Point& p) {
    const Curve& C = curve();
    if (in[0] != 0x04) return false;

    uint64_t x[4], y[4];
    be_to_limbs(in + 1,  x);
    be_to_limbs(in + 33, y);
    if (!lt4(x, C.P.m) || !lt4(y, C.P.m)) return false;   // must be canonical
    if (is_zero4(x) && is_zero4(y)) return false;         // not the identity

    uint64_t xm[4], ym[4];
    to_mont(xm, x, C.P);
    to_mont(ym, y, C.P);
    if (!on_curve_mont(xm, ym)) return false;

    std::memcpy(p.X, xm, sizeof p.X);
    std::memcpy(p.Y, ym, sizeof p.Y);
    std::memcpy(p.Z, C.P.one, sizeof p.Z);
    return true;
}

// A private scalar is valid iff 0 < d < n.
static inline bool valid_scalar(const uint8_t d[32]) {
    const Curve& C = curve();
    uint64_t k[4];
    be_to_limbs(d, k);
    if (is_zero4(k)) return false;
    return lt4(k, C.N.m);
}

static inline bool public_from_private(const uint8_t priv[32], uint8_t pub[65]) {
    if (!valid_scalar(priv)) return false;
    Point q;
    scalar_mul_base(q, priv);
    if (point_is_identity(q)) return false;
    export_public(q, pub);
    return true;
}

// ECDH: the shared secret is the X coordinate of [d]Q, 32 bytes big-endian.
static inline bool ecdh(const uint8_t priv[32], const uint8_t peer_pub[65], uint8_t shared_x[32]) {
    if (!valid_scalar(priv)) return false;
    Point q;
    if (!import_public(peer_pub, q)) return false;
    Point s;
    scalar_mul(s, priv, q);
    if (point_is_identity(s)) return false;
    Affine a;
    point_to_affine(a, s);
    uint64_t x[4];
    from_mont(x, a.x, curve().P);
    limbs_to_be(x, shared_x);
    secure_zero(&s, sizeof s);
    secure_zero(&a, sizeof a);
    secure_zero(x, sizeof x);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
//  RFC 6979 — deterministic ECDSA nonce generation (HMAC_DRBG over SHA-256)
// ════════════════════════════════════════════════════════════════════════════

// bits2octets(h) = int2octets(bits2int(h) mod n). For P-256/SHA-256 qlen ==
// hlen == 256, so bits2int is a plain big-endian read and this reduces to a
// conditional subtraction of n.
static inline void bits2octets(const uint8_t h[32], uint8_t out[32]) {
    const Curve& C = curve();
    uint64_t z[4];
    be_to_limbs(h, z);
    uint64_t t[4];
    uint64_t borrow = sub4(t, z, C.N.m);
    cmov4(z, t, borrow ^ 1);          // subtract n when z >= n
    limbs_to_be(z, out);
}

// Produce the deterministic nonce k for message hash h and private key x.
// `attempt` re-runs the DRBG's rejection loop for the (vanishingly rare) cases
// where k lands out of range or the signature degenerates.
static inline void rfc6979_k(const uint8_t x[32], const uint8_t h[32], int attempt, uint8_t k_out[32]) {
    const Curve& C = curve();
    uint8_t V[32], K[32], h1o[32];
    std::memset(V, 0x01, 32);
    std::memset(K, 0x00, 32);
    bits2octets(h, h1o);

    uint8_t buf[32 + 1 + 32 + 32];
    // K = HMAC_K(V || 0x00 || int2octets(x) || bits2octets(h1))
    std::memcpy(buf, V, 32); buf[32] = 0x00;
    std::memcpy(buf + 33, x, 32);
    std::memcpy(buf + 65, h1o, 32);
    bantu_native::hmac_sha256_raw(K, 32, buf, sizeof buf, K);
    bantu_native::hmac_sha256_raw(K, 32, V, 32, V);
    // K = HMAC_K(V || 0x01 || int2octets(x) || bits2octets(h1))
    std::memcpy(buf, V, 32); buf[32] = 0x01;
    bantu_native::hmac_sha256_raw(K, 32, buf, sizeof buf, K);
    bantu_native::hmac_sha256_raw(K, 32, V, 32, V);

    int produced = 0;
    for (;;) {
        bantu_native::hmac_sha256_raw(K, 32, V, 32, V);   // T = V (qlen == hlen)
        uint64_t t[4];
        be_to_limbs(V, t);
        bool ok = !is_zero4(t) && lt4(t, C.N.m);
        if (ok) {
            if (produced == attempt) { std::memcpy(k_out, V, 32); break; }
            produced++;
        }
        // K = HMAC_K(V || 0x00) ; V = HMAC_K(V)
        uint8_t b2[33];
        std::memcpy(b2, V, 32); b2[32] = 0x00;
        bantu_native::hmac_sha256_raw(K, 32, b2, 33, K);
        bantu_native::hmac_sha256_raw(K, 32, V, 32, V);
    }
    secure_zero(V, sizeof V);
    secure_zero(K, sizeof K);
    secure_zero(buf, sizeof buf);
}

// ════════════════════════════════════════════════════════════════════════════
//  ECDSA
// ════════════════════════════════════════════════════════════════════════════

// Sign with a caller-supplied nonce. INTERNAL — exposed only so the selftest can
// run vectors that pin k. Never reachable from Bantu.
static inline bool ecdsa_sign_with_k(const uint8_t priv[32], const uint8_t hash[32],
                                     const uint8_t k[32], uint8_t sig_rs[64]) {
    const Curve& C = curve();
    if (!valid_scalar(priv) || !valid_scalar(k)) return false;

    Point R;
    scalar_mul_base(R, k);
    if (point_is_identity(R)) return false;
    Affine ra;
    point_to_affine(ra, R);
    uint64_t rx[4];
    from_mont(rx, ra.x, C.P);

    // r = Rx mod n
    uint64_t r[4], t[4];
    std::memcpy(r, rx, sizeof r);
    uint64_t borrow = sub4(t, r, C.N.m);
    cmov4(r, t, borrow ^ 1);
    if (is_zero4(r)) return false;

    // Everything below is mod n, in Montgomery form.
    uint64_t z[4], d[4], kk[4];
    be_to_limbs(hash, z);
    borrow = sub4(t, z, C.N.m);
    cmov4(z, t, borrow ^ 1);                      // z = bits2int(hash) mod n
    be_to_limbs(priv, d);
    be_to_limbs(k, kk);

    uint64_t zm[4], dm[4], km[4], rm[4];
    to_mont(zm, z,  C.N);
    to_mont(dm, d,  C.N);
    to_mont(km, kk, C.N);
    to_mont(rm, r,  C.N);

    uint64_t rd[4], sum[4], kinv[4], sm[4];
    mont_mul(rd, rm, dm, C.N);                    // r*d
    mod_add(sum, zm, rd, C.N);                    // z + r*d
    mont_inv(kinv, km, C.N);                      // k^-1
    mont_mul(sm, kinv, sum, C.N);                 // s = k^-1 (z + r*d)

    uint64_t s[4];
    from_mont(s, sm, C.N);
    if (is_zero4(s)) return false;

    // I2OSP to exactly 32 bytes each — NOT DER, and never minimal-length.
    limbs_to_be(r, sig_rs);
    limbs_to_be(s, sig_rs + 32);

    secure_zero(d, sizeof d);   secure_zero(kk, sizeof kk);
    secure_zero(dm, sizeof dm); secure_zero(km, sizeof km);
    secure_zero(kinv, sizeof kinv);
    return true;
}

// Deterministic ECDSA (RFC 6979). Signature is raw r||s, 64 bytes.
static inline bool ecdsa_sign(const uint8_t priv[32], const uint8_t hash[32], uint8_t sig_rs[64]) {
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t k[32];
        rfc6979_k(priv, hash, attempt, k);
        bool ok = ecdsa_sign_with_k(priv, hash, k, sig_rs);
        secure_zero(k, sizeof k);
        if (ok) return true;
    }
    return false;
}

static inline bool ecdsa_verify(const uint8_t pub[65], const uint8_t hash[32], const uint8_t sig_rs[64]) {
    const Curve& C = curve();
    Point Q;
    if (!import_public(pub, Q)) return false;

    uint64_t r[4], s[4];
    be_to_limbs(sig_rs, r);
    be_to_limbs(sig_rs + 32, s);
    if (is_zero4(r) || !lt4(r, C.N.m)) return false;      // 1 <= r < n
    if (is_zero4(s) || !lt4(s, C.N.m)) return false;      // 1 <= s < n

    uint64_t z[4], t[4];
    be_to_limbs(hash, z);
    uint64_t borrow = sub4(t, z, C.N.m);
    cmov4(z, t, borrow ^ 1);

    uint64_t zm[4], rm[4], sm[4], sinv[4], u1m[4], u2m[4], u1[4], u2[4];
    to_mont(zm, z, C.N);
    to_mont(rm, r, C.N);
    to_mont(sm, s, C.N);
    mont_inv(sinv, sm, C.N);
    mont_mul(u1m, zm, sinv, C.N);
    mont_mul(u2m, rm, sinv, C.N);
    from_mont(u1, u1m, C.N);
    from_mont(u2, u2m, C.N);

    uint8_t u1b[32], u2b[32];
    limbs_to_be(u1, u1b);
    limbs_to_be(u2, u2b);

    Point A, B, S;
    scalar_mul_base(A, u1b);
    scalar_mul(B, u2b, Q);
    point_add(S, A, B);
    if (point_is_identity(S)) return false;

    Affine sa;
    point_to_affine(sa, S);
    uint64_t sx[4];
    from_mont(sx, sa.x, C.P);
    borrow = sub4(t, sx, C.N.m);
    cmov4(sx, t, borrow ^ 1);                              // Sx mod n
    return eq4(sx, r);
}

} // namespace bantu_p256
