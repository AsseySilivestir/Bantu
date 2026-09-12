#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  webpush.hpp — Web Push: RFC 8188 (aes128gcm), RFC 8291 (message encryption),
//                RFC 8292 (VAPID). Built on p256.hpp + aes_gcm.hpp.
//
//  LAYERING (deliberate — each layer is independently pinned by vectors)
//  ---------------------------------------------------------------------
//    base64url + HKDF          ← RFC 5869
//    aes128gcm framing         ← RFC 8188 §3.1 / §3.2   (IKM supplied directly)
//    Web Push key derivation   ← RFC 8291 §5            (IKM from ECDH)
//    VAPID JWT                 ← RFC 8292 / RFC 7515
//
//  SAFETY
//  ------
//  `encrypt()` takes NEITHER the salt NOR the ephemeral keypair. Both are
//  generated internally from the OS CSPRNG. RFC 8291 §2 requires both to be
//  fresh per message; reusing either reuses (CEK, NONCE), which is a total break
//  of the AEAD. Exposing them would make that catastrophe a one-line mistake in
//  Bantu, and no legitimate caller needs to supply them. The deterministic
//  variant `encrypt_with_params()` exists ONLY so the selftest can reproduce the
//  RFC 8291 §5 vector — do not add a parameter to the public entry point.
//
//  `selftest()` runs the known-answer suite and is invoked once, memoised, on
//  first use of any builtin. It FAILS CLOSED: a miscompiled binary can never
//  silently emit a broken or insecure push.
// ════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "crypto_native.hpp"
#include "p256.hpp"
#include "aes_gcm.hpp"

namespace bantu_webpush {

typedef std::vector<uint8_t> Bytes;

// The host installs the OS CSPRNG here at startup (evaluator.hpp → bantuCsprng).
// Left null, every operation that needs randomness fails closed rather than
// falling back to a predictable source.
inline bool (*random_bytes)(unsigned char*, size_t) = nullptr;

static inline bool rng(uint8_t* p, size_t n) {
    if (!random_bytes) return false;
    return random_bytes(p, n);
}

// ── base64url, unpadded (RFC 4648 §5) ───────────────────────────────────────

static const char* kB64U = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static inline std::string b64url_encode(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i+1] << 8) | p[i+2];
        out += kB64U[(v >> 18) & 63]; out += kB64U[(v >> 12) & 63];
        out += kB64U[(v >>  6) & 63]; out += kB64U[v & 63];
        i += 3;
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)p[i] << 16;
        out += kB64U[(v >> 18) & 63]; out += kB64U[(v >> 12) & 63];
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i+1] << 8);
        out += kB64U[(v >> 18) & 63]; out += kB64U[(v >> 12) & 63]; out += kB64U[(v >> 6) & 63];
    }
    return out;
}
static inline std::string b64url_encode(const Bytes& b) { return b64url_encode(b.data(), b.size()); }

// Accepts both alphabets and tolerates padding — browsers are inconsistent about
// which they send in a subscription.
static inline bool b64url_decode(const std::string& s, Bytes& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xFF));
        }
    }
    return true;
}

// ── HKDF-SHA256 (RFC 5869) ──────────────────────────────────────────────────

static inline void hkdf_extract(const uint8_t* salt, size_t salt_len,
                                const uint8_t* ikm, size_t ikm_len, uint8_t prk[32]) {
    bantu_native::hmac_sha256_raw(salt, salt_len, ikm, ikm_len, prk);
}

// Expand to at most 32 octets (one block) — every Web Push use fits.
static inline void hkdf_expand(const uint8_t prk[32], const uint8_t* info, size_t info_len,
                               size_t out_len, uint8_t* out) {
    Bytes buf(info_len + 1);
    if (info_len) std::memcpy(buf.data(), info, info_len);
    buf[info_len] = 0x01;
    uint8_t t[32];
    bantu_native::hmac_sha256_raw(prk, 32, buf.data(), buf.size(), t);
    std::memcpy(out, t, out_len > 32 ? 32 : out_len);
    bantu_p256::secure_zero(t, sizeof t);
}

// ── RFC 8188 aes128gcm content coding ───────────────────────────────────────

static const char kCekInfo[]   = "Content-Encoding: aes128gcm";   // 26 chars
static const char kNonceInfo[] = "Content-Encoding: nonce";       // 23 chars

// CEK and NONCE from the record salt and the input keying material (§2.2/§2.3).
static inline void derive_cek_nonce(const uint8_t salt[16], const uint8_t* ikm, size_t ikm_len,
                                    uint8_t cek[16], uint8_t nonce_base[12]) {
    uint8_t prk[32];
    hkdf_extract(salt, 16, ikm, ikm_len, prk);

    uint8_t info[32];
    size_t n = sizeof(kCekInfo);              // includes the trailing NUL = the 0x00 separator
    std::memcpy(info, kCekInfo, n);
    hkdf_expand(prk, info, n, 16, cek);

    n = sizeof(kNonceInfo);
    std::memcpy(info, kNonceInfo, n);
    hkdf_expand(prk, info, n, 12, nonce_base);

    bantu_p256::secure_zero(prk, sizeof prk);
}

// NONCE_i = NONCE_base XOR (SEQ as a 96-bit big-endian integer).
static inline void nonce_for(const uint8_t base[12], uint64_t seq, uint8_t out[12]) {
    std::memcpy(out, base, 12);
    for (int i = 0; i < 8; i++) out[11 - i] ^= (uint8_t)(seq >> (8 * i));
}

// Build the 21+idlen-octet content-coding header (§2.1).
static inline void build_header(const uint8_t salt[16], uint32_t rs,
                                const uint8_t* keyid, size_t keyid_len, Bytes& out) {
    out.clear();
    out.insert(out.end(), salt, salt + 16);
    out.push_back((uint8_t)(rs >> 24)); out.push_back((uint8_t)(rs >> 16));
    out.push_back((uint8_t)(rs >> 8));  out.push_back((uint8_t)rs);
    out.push_back((uint8_t)keyid_len);
    if (keyid_len) out.insert(out.end(), keyid, keyid + keyid_len);
}

// Single-record aes128gcm encryption with the IKM supplied directly. This is the
// RFC 8188 layer, independent of Web Push key agreement — it is what the §3.1
// vector pins.
static inline bool aes128gcm_encrypt(const uint8_t* ikm, size_t ikm_len,
                                     const uint8_t salt[16], uint32_t rs,
                                     const uint8_t* keyid, size_t keyid_len,
                                     const uint8_t* plaintext, size_t pt_len,
                                     Bytes& out) {
    if (rs < 18) return false;                       // §2.1: rs < 18 is invalid
    if (keyid_len > 255) return false;
    if (pt_len + 1 + 16 > rs) return false;          // must fit one record

    uint8_t cek[16], nonce_base[12], nonce[12];
    derive_cek_nonce(salt, ikm, ikm_len, cek, nonce_base);
    nonce_for(nonce_base, 0, nonce);

    Bytes record(plaintext, plaintext + pt_len);
    record.push_back(0x02);                          // last-record delimiter

    Bytes sealed;
    bool ok = bantu_aesgcm::gcm_seal(cek, nonce, 12, nullptr, 0,
                                     record.data(), record.size(), sealed);
    bantu_p256::secure_zero(cek, sizeof cek);
    bantu_p256::secure_zero(nonce_base, sizeof nonce_base);
    bantu_p256::secure_zero(record.data(), record.size());
    if (!ok) return false;

    build_header(salt, rs, keyid, keyid_len, out);
    out.insert(out.end(), sealed.begin(), sealed.end());
    return true;
}

// Decrypt a full aes128gcm body. Handles MULTIPLE records so the RFC 8188 §3.2
// vector is runnable; we only ever emit one.
static inline bool aes128gcm_decrypt(const uint8_t* ikm, size_t ikm_len,
                                     const uint8_t* body, size_t body_len,
                                     Bytes& out) {
    if (body_len < 21) return false;
    const uint8_t* salt = body;
    uint32_t rs = ((uint32_t)body[16] << 24) | ((uint32_t)body[17] << 16) |
                  ((uint32_t)body[18] << 8)  | (uint32_t)body[19];
    size_t idlen = body[20];
    if (rs < 18) return false;
    if (21 + idlen > body_len) return false;

    size_t off = 21 + idlen;
    uint8_t cek[16], nonce_base[12];
    derive_cek_nonce(salt, ikm, ikm_len, cek, nonce_base);

    out.clear();
    uint64_t seq = 0;
    bool sawLast = false;
    while (off < body_len) {
        size_t n = body_len - off;
        if (n > rs) n = rs;
        if (n < 17) { bantu_p256::secure_zero(cek, 16); return false; }

        uint8_t nonce[12];
        nonce_for(nonce_base, seq, nonce);
        Bytes rec;
        if (!bantu_aesgcm::gcm_open(cek, nonce, 12, nullptr, 0, body + off, n, rec)) {
            bantu_p256::secure_zero(cek, 16);
            return false;
        }
        // Strip zero padding back to the delimiter: 0x01 mid-stream, 0x02 last.
        size_t end = rec.size();
        while (end > 0 && rec[end - 1] == 0x00) end--;
        if (end == 0) { bantu_p256::secure_zero(cek, 16); return false; }
        uint8_t delim = rec[end - 1];
        if (delim != 0x01 && delim != 0x02) { bantu_p256::secure_zero(cek, 16); return false; }
        out.insert(out.end(), rec.begin(), rec.begin() + (end - 1));
        if (delim == 0x02) { sawLast = true; off += n; break; }

        off += n;
        seq++;
    }
    bantu_p256::secure_zero(cek, sizeof cek);
    bantu_p256::secure_zero(nonce_base, sizeof nonce_base);
    return sawLast && off == body_len;
}

// ── RFC 8291 Web Push key derivation ────────────────────────────────────────

static const char kWebPushInfo[] = "WebPush: info";   // 13 chars + NUL separator

// IKM = HKDF(salt = auth_secret, ikm = ECDH, info = "WebPush: info"\0 ua||as).
static inline void webpush_ikm(const uint8_t ecdh_secret[32], const uint8_t* auth, size_t auth_len,
                               const uint8_t ua_public[65], const uint8_t as_public[65],
                               uint8_t ikm[32]) {
    uint8_t prk_key[32];
    // Extract: the AUTH SECRET is the HMAC key, the ECDH output is the message.
    hkdf_extract(auth, auth_len, ecdh_secret, 32, prk_key);

    uint8_t key_info[14 + 65 + 65];
    std::memcpy(key_info, kWebPushInfo, 14);         // includes the 0x00
    std::memcpy(key_info + 14, ua_public, 65);
    std::memcpy(key_info + 79, as_public, 65);
    hkdf_expand(prk_key, key_info, sizeof key_info, 32, ikm);

    bantu_p256::secure_zero(prk_key, sizeof prk_key);
}

static const uint32_t kRecordSize = 4096;
// 4096 body cap − 86-octet header − 1 delimiter − 16 tag.
static const size_t kMaxPayload = 4096 - 86 - 1 - 16;

// Deterministic encryption — INTERNAL, for the RFC 8291 §5 vector only.
static inline bool encrypt_with_params(const uint8_t ua_public[65], const uint8_t* auth, size_t auth_len,
                                       const uint8_t* plaintext, size_t pt_len,
                                       const uint8_t as_private[32], const uint8_t salt[16],
                                       Bytes& out) {
    if (pt_len > kMaxPayload) return false;

    uint8_t as_public[65];
    if (!bantu_p256::public_from_private(as_private, as_public)) return false;

    uint8_t ecdh_secret[32];
    if (!bantu_p256::ecdh(as_private, ua_public, ecdh_secret)) return false;

    uint8_t ikm[32];
    webpush_ikm(ecdh_secret, auth, auth_len, ua_public, as_public, ikm);
    bantu_p256::secure_zero(ecdh_secret, sizeof ecdh_secret);

    bool ok = aes128gcm_encrypt(ikm, 32, salt, kRecordSize, as_public, 65, plaintext, pt_len, out);
    bantu_p256::secure_zero(ikm, sizeof ikm);
    return ok;
}

// The public entry point. Salt and ephemeral key are generated internally and
// are NOT parameters — see the header note.
static inline bool encrypt(const uint8_t ua_public[65], const uint8_t* auth, size_t auth_len,
                           const uint8_t* plaintext, size_t pt_len, Bytes& out) {
    uint8_t as_private[32], salt[16];
    bool have_key = false;
    for (int attempt = 0; attempt < 16 && !have_key; attempt++) {
        if (!rng(as_private, 32)) return false;
        // Rejection sampling, not reduce-mod-n, which would be slightly biased.
        have_key = bantu_p256::valid_scalar(as_private);
    }
    if (!have_key) return false;
    if (!rng(salt, 16)) { bantu_p256::secure_zero(as_private, 32); return false; }
    bool ok = encrypt_with_params(ua_public, auth, auth_len, plaintext, pt_len, as_private, salt, out);
    bantu_p256::secure_zero(as_private, sizeof as_private);
    return ok;
}

// Decrypt as the user agent would. Used by the selftest and by tooling; a real
// Bantu server never calls this.
static inline bool decrypt(const uint8_t ua_private[32], const uint8_t* auth, size_t auth_len,
                           const uint8_t* body, size_t body_len, Bytes& out) {
    if (body_len < 21 + 65) return false;
    if (body[20] != 65) return false;
    const uint8_t* as_public = body + 21;

    uint8_t ua_public[65];
    if (!bantu_p256::public_from_private(ua_private, ua_public)) return false;

    uint8_t ecdh_secret[32];
    if (!bantu_p256::ecdh(ua_private, as_public, ecdh_secret)) return false;

    uint8_t ikm[32];
    webpush_ikm(ecdh_secret, auth, auth_len, ua_public, as_public, ikm);
    bantu_p256::secure_zero(ecdh_secret, sizeof ecdh_secret);

    bool ok = aes128gcm_decrypt(ikm, 32, body, body_len, out);
    bantu_p256::secure_zero(ikm, sizeof ikm);
    return ok;
}

// ── RFC 8292 VAPID ──────────────────────────────────────────────────────────

// The `aud` claim is the ORIGIN of the endpoint: scheme://host[:port], no path.
// Passing the full endpoint URL here works against Mozilla's autopush and is
// rejected by FCM — a bug that ships green from Firefox-only testing.
static inline bool origin_of(const std::string& endpoint, std::string& out) {
    const std::string https = "https://";
    if (endpoint.size() <= https.size()) return false;
    if (endpoint.compare(0, https.size(), https) != 0) return false;   // https only
    size_t start = https.size();
    size_t end = endpoint.find('/', start);
    std::string authority = (end == std::string::npos) ? endpoint.substr(start)
                                                       : endpoint.substr(start, end - start);
    if (authority.empty()) return false;
    // Drop an explicit :443, which is the scheme default.
    if (authority.size() > 4 && authority.compare(authority.size() - 4, 4, ":443") == 0)
        authority = authority.substr(0, authority.size() - 4);
    out = https + authority;
    return true;
}

static inline std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}

// Signed ES256 JWT. `exp` is UNIX SECONDS (not milliseconds) and RFC 8292 §2
// caps it at 24 hours out.
static inline bool jwt(const uint8_t priv[32], const std::string& aud,
                       const std::string& sub, int64_t exp, std::string& out) {
    if (aud.empty() || sub.empty()) return false;

    std::string header = "{\"typ\":\"JWT\",\"alg\":\"ES256\"}";
    std::string claims = "{\"aud\":\"" + json_escape(aud) + "\",\"exp\":" + std::to_string(exp) +
                         ",\"sub\":\"" + json_escape(sub) + "\"}";

    std::string signing_input = b64url_encode((const uint8_t*)header.data(), header.size()) + "." +
                                b64url_encode((const uint8_t*)claims.data(), claims.size());

    uint8_t digest[32];
    bantu_native::sha256_raw((const uint8_t*)signing_input.data(), signing_input.size(), digest);

    uint8_t sig[64];
    if (!bantu_p256::ecdsa_sign(priv, digest, sig)) return false;   // raw r||s, never DER

    out = signing_input + "." + b64url_encode(sig, 64);
    return true;
}

// "vapid t=<jwt>, k=<base64url(public key)>"  (RFC 8292 §3)
static inline bool vapid_header(const uint8_t priv[32], const std::string& aud,
                                const std::string& sub, int64_t exp, std::string& out) {
    uint8_t pub[65];
    if (!bantu_p256::public_from_private(priv, pub)) return false;
    std::string token;
    if (!jwt(priv, aud, sub, exp, token)) return false;
    out = "vapid t=" + token + ", k=" + b64url_encode(pub, 65);
    return true;
}

// ── Known-answer selftest (fail-closed) ─────────────────────────────────────

struct SelftestResult {
    bool ok = false;
    int ran = 0;
    std::string first_failure;
};

namespace detail {

static inline bool hex_eq(const uint8_t* got, size_t n, const char* want_hex) {
    static const char* H = "0123456789abcdef";
    std::string g;
    for (size_t i = 0; i < n; i++) { g += H[got[i] >> 4]; g += H[got[i] & 15]; }
    std::string w;
    for (const char* p = want_hex; *p; p++)
        if (*p != ' ') w += (char)((*p >= 'A' && *p <= 'F') ? *p - 'A' + 'a' : *p);
    return g == w;
}
static inline Bytes unhex(const char* h) {
    Bytes b;
    auto v = [](char c) { return (c >= '0' && c <= '9') ? c - '0'
                               : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                               : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0; };
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) b.push_back((uint8_t)((v(h[i]) << 4) | v(h[i + 1])));
    return b;
}

} // namespace detail

#define BWP_CHECK(cond, name) do { r.ran++; if (!(cond)) { r.first_failure = (name); return r; } } while (0)

static inline SelftestResult run_selftest() {
    using namespace detail;
    SelftestResult r;

    // ── base64url ──
    {
        Bytes b = unhex("000102fbfcfd");
        std::string e = b64url_encode(b);
        Bytes back;
        BWP_CHECK(b64url_decode(e, back) && back == b, "base64url round-trip");
        BWP_CHECK(e.find('=') == std::string::npos, "base64url is unpadded");
        BWP_CHECK(b64url_encode(unhex("fbff")) == "-_8", "base64url uses the URL alphabet");
    }

    // ── AES-128, FIPS 197 Appendix B ──
    {
        Bytes k = unhex("000102030405060708090a0b0c0d0e0f");
        Bytes p = unhex("00112233445566778899aabbccddeeff");
        bantu_aesgcm::Aes128 a;
        bantu_aesgcm::aes128_expand(a, k.data());
        uint8_t o[16];
        bantu_aesgcm::aes128_encrypt_block(a, p.data(), o);
        BWP_CHECK(hex_eq(o, 16, "69c4e0d86a7b0430d8cdb78070b4c55a"), "FIPS 197 AES-128 block");
    }

    // ── AES-128-GCM, McGrew & Viega case 4 (exercises AAD and a partial block) ──
    {
        Bytes k   = unhex("feffe9928665731c6d6a8f9467308308");
        Bytes iv  = unhex("cafebabefacedbaddecaf888");
        Bytes aad = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
        Bytes pt  = unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                          "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
        Bytes out;
        BWP_CHECK(bantu_aesgcm::gcm_seal(k.data(), iv.data(), 12, aad.data(), aad.size(),
                                         pt.data(), pt.size(), out), "GCM seal");
        BWP_CHECK(hex_eq(out.data(), pt.size(),
                  "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
                  "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091"), "GCM ciphertext");
        BWP_CHECK(hex_eq(out.data() + pt.size(), 16, "5bc94fbc3221a5db94fae95ae7121a47"), "GCM tag");
        Bytes back;
        BWP_CHECK(bantu_aesgcm::gcm_open(k.data(), iv.data(), 12, aad.data(), aad.size(),
                                         out.data(), out.size(), back) && back == pt, "GCM open");
        out[out.size() - 1] ^= 1;
        BWP_CHECK(!bantu_aesgcm::gcm_open(k.data(), iv.data(), 12, aad.data(), aad.size(),
                                          out.data(), out.size(), back), "GCM rejects a bad tag");
    }

    // ── HKDF-SHA256, RFC 5869 §A.1 ──
    {
        Bytes ikm  = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
        Bytes salt = unhex("000102030405060708090a0b0c");
        uint8_t prk[32];
        hkdf_extract(salt.data(), salt.size(), ikm.data(), ikm.size(), prk);
        BWP_CHECK(hex_eq(prk, 32, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"),
                  "RFC 5869 A.1 PRK");
        Bytes info = unhex("f0f1f2f3f4f5f6f7f8f9");
        uint8_t okm[32];
        hkdf_expand(prk, info.data(), info.size(), 32, okm);
        BWP_CHECK(hex_eq(okm, 32, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"),
                  "RFC 5869 A.1 OKM (first block)");
    }

    // ── P-256 keygen + deterministic ECDSA, RFC 6979 §A.2.5 "test" ──
    // The "test" case has s = 019F41..., whose leading zero byte pins the
    // requirement that r and s are each padded to exactly 32 octets.
    {
        Bytes x = unhex("C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721");
        uint8_t pub[65];
        BWP_CHECK(bantu_p256::public_from_private(x.data(), pub), "P-256 keygen");
        BWP_CHECK(hex_eq(pub + 1, 32, "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6"),
                  "RFC 6979 public key x");
        uint8_t h[32];
        bantu_native::sha256_raw((const uint8_t*)"test", 4, h);
        uint8_t sig[64];
        BWP_CHECK(bantu_p256::ecdsa_sign(x.data(), h, sig), "ECDSA sign");
        BWP_CHECK(hex_eq(sig, 32, "f1abb023518351cd71d881567b1ea663ed3efcf6c5132b354f28d3b0b7d38367"),
                  "RFC 6979 r");
        BWP_CHECK(hex_eq(sig + 32, 32, "019f4113742a2b14bd25926b49c649155f267e60d3814b4c0cc84250e46f0083"),
                  "RFC 6979 s (leading zero octet)");
        BWP_CHECK(bantu_p256::ecdsa_verify(pub, h, sig), "ECDSA verify");
        sig[5] ^= 1;
        BWP_CHECK(!bantu_p256::ecdsa_verify(pub, h, sig), "ECDSA rejects a tampered signature");
    }

    // ── ECDH agreement between two pinned keypairs ──
    {
        Bytes a = unhex("C6EF9C5D78AE012A011164ACB397CE2088685D8F06BF9BE0B283AB46476BEE53");
        Bytes b = unhex("C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721");
        uint8_t pa[65], pb[65], z1[32], z2[32];
        BWP_CHECK(bantu_p256::public_from_private(a.data(), pa), "ECDH pubkey a");
        BWP_CHECK(hex_eq(pa + 1, 32, "d12dfb5289c8d4f81208b70270398c342296970a0bccb74c736fc7554494bf63"),
                  "RFC 5903 gr.x");
        BWP_CHECK(bantu_p256::public_from_private(b.data(), pb), "ECDH pubkey b");
        BWP_CHECK(bantu_p256::ecdh(a.data(), pb, z1), "ECDH a·B");
        BWP_CHECK(bantu_p256::ecdh(b.data(), pa, z2), "ECDH b·A");
        BWP_CHECK(std::memcmp(z1, z2, 32) == 0, "ECDH agreement");
    }

    // ── Invalid public keys must be rejected (invalid-curve defence) ──
    {
        Bytes d = unhex("C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721");
        uint8_t pub[65];
        bantu_p256::public_from_private(d.data(), pub);
        bantu_p256::Point pt;
        uint8_t bad[65];
        std::memcpy(bad, pub, 65); bad[64] ^= 1;
        BWP_CHECK(!bantu_p256::import_public(bad, pt), "rejects an off-curve public key");
        std::memcpy(bad, pub, 65); bad[0] = 0x02;
        BWP_CHECK(!bantu_p256::import_public(bad, pt), "rejects a compressed point");
        std::memcpy(bad, pub, 65); std::memset(bad + 1, 0xff, 32);
        BWP_CHECK(!bantu_p256::import_public(bad, pt), "rejects X >= p");
    }

    // ── RFC 8188 §3.1 — published single-record vector ──
    //
    // The IKM is supplied directly, so this pins the content-coding layer alone:
    // HKDF → CEK/NONCE → record padding → header framing → AES-128-GCM. No
    // elliptic curve involved, so a failure here is never a P-256 bug.
    //
    // NOTE: the RFC's prose says "54-octet content body" and Content-Length: 54,
    // but the published base64url decodes to 53 octets
    // (21 header + 15 plaintext + 1 delimiter + 16 tag). The encoded value is
    // authoritative; the octet count is an editorial slip.
    {
        Bytes ikm, salt_full, want;
        BWP_CHECK(b64url_decode("yqdlZ-tYemfogSmv7Ws5PQ", ikm), "8188 3.1 ikm decodes");
        BWP_CHECK(b64url_decode("I1BsxtFttlv3u_Oo94xnmw", salt_full), "8188 3.1 salt decodes");
        BWP_CHECK(b64url_decode(
            "I1BsxtFttlv3u_Oo94xnmwAAEAAA-NAVub2qFgBEuQKRapoZu-IxkIva3MEB1PD-ly8Thjg",
            want), "8188 3.1 body decodes");
        BWP_CHECK(want.size() == 53, "8188 3.1 body is 53 octets");

        // The intermediates the RFC publishes, checked individually so a failure
        // localises to one derivation step.
        uint8_t cek[16], nonce[12], prk[32];
        hkdf_extract(salt_full.data(), 16, ikm.data(), ikm.size(), prk);
        Bytes wprk, wcek, wnonce;
        b64url_decode("zyeH5phsIsgUyd4oiSEIy35x-gIi4aM7y0hCF8mwn9g", wprk);
        b64url_decode("_wniytB-ofscZDh4tbSjHw", wcek);
        b64url_decode("Bcs8gkIRKLI8GeI8", wnonce);
        BWP_CHECK(wprk.size() == 32 && std::memcmp(prk, wprk.data(), 32) == 0, "8188 3.1 PRK");
        derive_cek_nonce(salt_full.data(), ikm.data(), ikm.size(), cek, nonce);
        BWP_CHECK(wcek.size() == 16 && std::memcmp(cek, wcek.data(), 16) == 0, "8188 3.1 CEK");
        BWP_CHECK(wnonce.size() == 12 && std::memcmp(nonce, wnonce.data(), 12) == 0, "8188 3.1 NONCE");

        const char* pt = "I am the walrus";
        Bytes got;
        BWP_CHECK(aes128gcm_encrypt(ikm.data(), ikm.size(), salt_full.data(), 4096,
                                    nullptr, 0, (const uint8_t*)pt, std::strlen(pt), got),
                  "8188 3.1 encrypt");
        BWP_CHECK(got == want, "8188 3.1 body matches the published vector");

        Bytes back;
        BWP_CHECK(aes128gcm_decrypt(ikm.data(), ikm.size(), want.data(), want.size(), back),
                  "8188 3.1 decrypt");
        BWP_CHECK(back.size() == std::strlen(pt) &&
                  std::memcmp(back.data(), pt, back.size()) == 0, "8188 3.1 plaintext");
    }

    // ── RFC 8188 §3.2 — published MULTI-record vector (decrypt) ──
    //
    // rs = 25, keyid "a1", two records: 7 message octets + one 0x00 pad octet in
    // the first, 8 in the second. We only ever emit a single record, but this is
    // the only published vector that pins the SEQ-XOR nonce derivation and the
    // 0x01-vs-0x02 delimiter distinction, so it runs on the decrypt path.
    {
        Bytes ikm, body;
        BWP_CHECK(b64url_decode("BO3ZVPxUlnLORbVGMpbT1Q", ikm), "8188 3.2 ikm decodes");
        BWP_CHECK(b64url_decode(
            "uNCkWiNYzKTnBN9ji3-qWAAAABkCYTHOG8chz_gnvgOqdGYovxyjuqRyJFjEDyoF"
            "1Fvkj6hQPdPHI51OEUKEpgz3SsLWIqS_uA", body), "8188 3.2 body decodes");
        BWP_CHECK(body.size() == 73, "8188 3.2 body is 73 octets");
        BWP_CHECK(body[20] == 2 && body[21] == 'a' && body[22] == '1', "8188 3.2 keyid is \"a1\"");

        Bytes back;
        BWP_CHECK(aes128gcm_decrypt(ikm.data(), ikm.size(), body.data(), body.size(), back),
                  "8188 3.2 multi-record decrypt");
        const char* pt = "I am the walrus";
        BWP_CHECK(back.size() == std::strlen(pt) &&
                  std::memcmp(back.data(), pt, back.size()) == 0, "8188 3.2 plaintext");
    }

    // ── RFC 8291 §5 + Appendix A — the published Web Push vector ──
    //
    // as_private and salt are pinned, so the entire body is deterministic and
    // byte-comparable. This is the single strongest test in the suite: it
    // exercises ECDH on P-256, the "WebPush: info" key combination, both HKDF
    // expansions, AES-128-GCM and the framing, end to end, against bytes
    // published by the IETF.
    //
    // NOTE: as in §3.1, the RFC's Content-Length says 145 while the published
    // base64url decodes to 144 octets (86 header + 41 plaintext + 1 + 16).
    {
        Bytes as_priv, ua_priv, ua_pub_v, as_pub_v, salt, auth, want, want_hdr;
        BWP_CHECK(b64url_decode("yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw", as_priv), "8291 as_private");
        BWP_CHECK(b64url_decode("q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94", ua_priv), "8291 ua_private");
        BWP_CHECK(b64url_decode("BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcx"
                                "aOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4", ua_pub_v), "8291 ua_public");
        BWP_CHECK(b64url_decode("BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIg"
                                "Dll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8", as_pub_v), "8291 as_public");
        BWP_CHECK(b64url_decode("DGv6ra1nlYgDCS1FRnbzlw", salt), "8291 salt");
        BWP_CHECK(b64url_decode("BTBZMqHH6r4Tts7J_aSIgg", auth), "8291 auth_secret");
        BWP_CHECK(b64url_decode(
            "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27ml"
            "mlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A_yl95bQpu6cVPT"
            "pK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN",
            want), "8291 body decodes");
        BWP_CHECK(want.size() == 144, "8291 body is 144 octets");
        BWP_CHECK(salt.size() == 16 && auth.size() == 16, "8291 salt/auth are 16 octets");

        // Both public keys must be derivable from their private keys — this alone
        // pins scalar multiplication against IETF-published values.
        uint8_t derived[65];
        BWP_CHECK(bantu_p256::public_from_private(as_priv.data(), derived) &&
                  std::memcmp(derived, as_pub_v.data(), 65) == 0, "8291 as_public from as_private");
        BWP_CHECK(bantu_p256::public_from_private(ua_priv.data(), derived) &&
                  std::memcmp(derived, ua_pub_v.data(), 65) == 0, "8291 ua_public from ua_private");

        // ECDH must agree in both directions and match the published secret.
        uint8_t s1[32], s2[32];
        Bytes want_ecdh;
        b64url_decode("kyrL1jIIOHEzg3sM2ZWRHDRB62YACZhhSlknJ672kSs", want_ecdh);
        BWP_CHECK(bantu_p256::ecdh(as_priv.data(), ua_pub_v.data(), s1), "8291 ECDH as->ua");
        BWP_CHECK(bantu_p256::ecdh(ua_priv.data(), as_pub_v.data(), s2), "8291 ECDH ua->as");
        BWP_CHECK(std::memcmp(s1, s2, 32) == 0, "8291 ECDH is symmetric");
        BWP_CHECK(want_ecdh.size() == 32 && std::memcmp(s1, want_ecdh.data(), 32) == 0,
                  "8291 ecdh_secret matches Appendix A");

        // The combined IKM, then the content-encryption PRK/CEK/NONCE.
        uint8_t ikm[32];
        Bytes want_ikm, want_cek, want_nonce;
        b64url_decode("S4lYMb_L0FxCeq0WhDx813KgSYqU26kOyzWUdsXYyrg", want_ikm);
        b64url_decode("oIhVW04MRdy2XN9CiKLxTg", want_cek);
        b64url_decode("4h_95klXJ5E_qnoN", want_nonce);
        webpush_ikm(s1, auth.data(), auth.size(), ua_pub_v.data(), as_pub_v.data(), ikm);
        BWP_CHECK(want_ikm.size() == 32 && std::memcmp(ikm, want_ikm.data(), 32) == 0,
                  "8291 IKM matches Appendix A");
        uint8_t cek[16], nonce[12];
        derive_cek_nonce(salt.data(), ikm, 32, cek, nonce);
        BWP_CHECK(want_cek.size() == 16 && std::memcmp(cek, want_cek.data(), 16) == 0,
                  "8291 CEK matches Appendix A");
        BWP_CHECK(want_nonce.size() == 12 && std::memcmp(nonce, want_nonce.data(), 12) == 0,
                  "8291 NONCE matches Appendix A");

        // And finally the whole body, byte for byte.
        const char* msg = "When I grow up, I want to be a watermelon";
        Bytes got;
        BWP_CHECK(encrypt_with_params(ua_pub_v.data(), auth.data(), auth.size(),
                                      (const uint8_t*)msg, std::strlen(msg),
                                      as_priv.data(), salt.data(), got), "8291 encrypt");
        BWP_CHECK(got == want, "8291 body matches the published vector");

        // And the receiver's side of the same vector.
        Bytes back;
        BWP_CHECK(decrypt(ua_priv.data(), auth.data(), auth.size(),
                          want.data(), want.size(), back), "8291 decrypt the published body");
        BWP_CHECK(back.size() == std::strlen(msg) &&
                  std::memcmp(back.data(), msg, back.size()) == 0, "8291 published plaintext");
    }

    // ── RFC 8291 end-to-end, against our own decryptor ──
    // Encrypt to a subscriber keypair and decrypt as that subscriber would. This
    // pins the full composition: ECDH → IKM → CEK/NONCE → record → framing.
    {
        Bytes ua_priv = unhex("C6EF9C5D78AE012A011164ACB397CE2088685D8F06BF9BE0B283AB46476BEE53");
        uint8_t ua_pub[65];
        BWP_CHECK(bantu_p256::public_from_private(ua_priv.data(), ua_pub), "subscriber keypair");
        Bytes auth = unhex("0102030405060708090a0b0c0d0e0f10");
        const char* msg = "When I grow up, I want to be a watermelon";
        Bytes as_priv = unhex("C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721");
        Bytes salt    = unhex("0c6bfaada76795880309a1b2c3d4e5f6");   // 16 octets

        Bytes body;
        BWP_CHECK(encrypt_with_params(ua_pub, auth.data(), auth.size(),
                                      (const uint8_t*)msg, std::strlen(msg),
                                      as_priv.data(), salt.data(), body), "RFC 8291 encrypt");
        BWP_CHECK(body.size() == 16 + 4 + 1 + 65 + std::strlen(msg) + 1 + 16,
                  "RFC 8291 body length (86-octet header + record)");
        BWP_CHECK(body[20] == 65, "keyid length is 65");
        BWP_CHECK(std::memcmp(body.data(), salt.data(), 16) == 0, "salt is at the front");
        BWP_CHECK(body[16] == 0 && body[17] == 0 && body[18] == 0x10 && body[19] == 0,
                  "rs == 4096, big-endian");

        Bytes back;
        BWP_CHECK(decrypt(ua_priv.data(), auth.data(), auth.size(),
                          body.data(), body.size(), back), "RFC 8291 decrypt");
        BWP_CHECK(back.size() == std::strlen(msg) &&
                  std::memcmp(back.data(), msg, back.size()) == 0, "RFC 8291 round-trip");

        // A tampered body must be rejected, not silently truncated.
        body[body.size() - 1] ^= 1;
        BWP_CHECK(!decrypt(ua_priv.data(), auth.data(), auth.size(),
                           body.data(), body.size(), back), "RFC 8291 rejects a tampered body");
    }

    // ── Payload size limit ──
    {
        Bytes ua_priv = unhex("C6EF9C5D78AE012A011164ACB397CE2088685D8F06BF9BE0B283AB46476BEE53");
        uint8_t ua_pub[65];
        bantu_p256::public_from_private(ua_priv.data(), ua_pub);
        Bytes auth = unhex("0102030405060708090a0b0c0d0e0f10");
        Bytes as_priv = unhex("C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721");
        uint8_t salt[16] = {0};
        Bytes big(kMaxPayload + 1, 'x'), body;
        BWP_CHECK(!encrypt_with_params(ua_pub, auth.data(), auth.size(), big.data(), big.size(),
                                       as_priv.data(), salt, body), "rejects an oversized payload");
        Bytes okp(kMaxPayload, 'x');
        BWP_CHECK(encrypt_with_params(ua_pub, auth.data(), auth.size(), okp.data(), okp.size(),
                                      as_priv.data(), salt, body), "accepts the maximum payload");
        BWP_CHECK(body.size() == 4096, "maximum body is exactly 4096 octets");
    }

    // ── VAPID ──
    {
        std::string o;
        BWP_CHECK(origin_of("https://fcm.googleapis.com/fcm/send/abc123", o) &&
                  o == "https://fcm.googleapis.com", "aud is the origin, not the endpoint");
        BWP_CHECK(origin_of("https://host:8443/p/x", o) && o == "https://host:8443", "aud keeps a non-default port");
        BWP_CHECK(origin_of("https://host:443/x", o) && o == "https://host", "aud drops :443");
        BWP_CHECK(origin_of("https://updates.push.services.mozilla.com/wpush/v2/xyz", o) &&
                  o == "https://updates.push.services.mozilla.com", "aud for autopush");
        BWP_CHECK(!origin_of("http://insecure.example/x", o), "rejects a non-https endpoint");

        Bytes d = unhex("C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721");
        std::string token;
        BWP_CHECK(jwt(d.data(), "https://fcm.googleapis.com", "mailto:a@example.com", 1700000000, token),
                  "JWT signs");
        size_t p1 = token.find('.'), p2 = token.find('.', p1 + 1);
        BWP_CHECK(p1 != std::string::npos && p2 != std::string::npos, "JWT has three parts");
        BWP_CHECK(token.find('=') == std::string::npos && token.find('+') == std::string::npos &&
                  token.find('/') == std::string::npos, "JWT is unpadded base64url");

        Bytes hdr, sigb;
        BWP_CHECK(b64url_decode(token.substr(0, p1), hdr) &&
                  std::string(hdr.begin(), hdr.end()) == "{\"typ\":\"JWT\",\"alg\":\"ES256\"}",
                  "JWT header");
        BWP_CHECK(b64url_decode(token.substr(p2 + 1), sigb) && sigb.size() == 64,
                  "ES256 signature is raw r||s, 64 octets");

        // Verify our own JWT — covers the signing input and r||s assembly.
        uint8_t pub[65], digest[32];
        bantu_p256::public_from_private(d.data(), pub);
        std::string signing_input = token.substr(0, p2);
        bantu_native::sha256_raw((const uint8_t*)signing_input.data(), signing_input.size(), digest);
        BWP_CHECK(bantu_p256::ecdsa_verify(pub, digest, sigb.data()), "JWT verifies");

        std::string h;
        BWP_CHECK(vapid_header(d.data(), "https://fcm.googleapis.com", "mailto:a@example.com", 1700000000, h),
                  "vapid header builds");
        BWP_CHECK(h.compare(0, 8, "vapid t=") == 0 && h.find(", k=") != std::string::npos,
                  "vapid header shape");
    }

    r.ok = true;
    return r;
}

#undef BWP_CHECK

// Memoised, fail-closed. Every builtin calls this before doing anything.
static inline const SelftestResult& selftest() {
    static SelftestResult cached = run_selftest();
    return cached;
}
static inline bool available() { return selftest().ok; }

} // namespace bantu_webpush
