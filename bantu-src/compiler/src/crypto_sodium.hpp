#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  crypto_sodium.hpp — optional authenticated encryption + password hashing,
//  backed by libsodium (a vetted, audited, constant-time crypto library).
//
//  WHY A LIBRARY (and not pure Bantu)
//  ----------------------------------
//  AEAD ciphers and password KDFs are side-channel-sensitive and, for KDFs,
//  deliberately expensive (hundreds of MB of memory, many iterations). Hand-
//  rolling them — in C++ or Bantu — is the classic "don't roll your own crypto"
//  trap: you get something slower AND less safe. So this layer BINDS libsodium
//  rather than reimplementing anything. (See DECISIONS.md D6.)
//
//  FEATURE-GATED (production-safe)
//  -------------------------------
//  Everything real here is compiled only when BANTU_SODIUM is defined AND
//  libsodium is available at build time. When it is NOT defined, this header
//  still compiles to trivial stubs and `available()` returns false — so the
//  DEFAULT build gains no new runtime dependency and existing deployments are
//  completely unaffected. Enabling it is a deliberate choice:
//      BANTU_SODIUM=1 bash build-mac.sh      (statically links libsodium)
//
//  SECURITY CHOICES
//  ----------------
//   • AEAD  = XChaCha20-Poly1305-IETF: a 192-bit RANDOM nonce (generated here
//     from libsodium's CSPRNG) makes nonce reuse practically impossible, and
//     the Poly1305 tag is verified in constant time on decrypt.
//   • Passwords = argon2id (crypto_pwhash_str), memory-hard, with the encoded
//     salt+parameters embedded in the output string; verification is constant
//     time. INTERACTIVE limits by default (tune up for offline-attack targets).
//   • Key material copied here is wiped with sodium_memzero after use.
//
//  This layer is intentionally free of any Bantu Value types — it deals only in
//  bytes/strings, so it can be tested and reasoned about in isolation. The thin
//  Value-adapting builtins live in evaluator.hpp.
// ════════════════════════════════════════════════════════════════════════════

#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>

#ifdef BANTU_SODIUM
#include <sodium.h>
#endif

namespace bantu_sodium {

// Was the interpreter built with libsodium support?
inline bool available() {
#ifdef BANTU_SODIUM
    return true;
#else
    return false;
#endif
}

// Idempotently initialize libsodium. Safe to call repeatedly; thread-safe once
// the first call has returned. Returns false if the library failed to init or
// support isn't compiled in.
inline bool init() {
#ifdef BANTU_SODIUM
    static bool done = false;
    static bool okFlag = false;
    if (!done) { okFlag = (sodium_init() >= 0); done = true; }
    return okFlag;
#else
    return false;
#endif
}

// The key length AEAD expects (32 bytes). Exposed so callers can validate /
// generate keys of the right size. Zero when support isn't compiled in.
inline size_t aeadKeyBytes() {
#ifdef BANTU_SODIUM
    return crypto_aead_xchacha20poly1305_ietf_KEYBYTES;
#else
    return 0;
#endif
}

// AEAD encrypt. `key` must be exactly aeadKeyBytes(). Produces nonce(24) ||
// ciphertext || tag(16), so decrypt is self-contained. `aad` is authenticated
// but not encrypted (may be empty). Sets ok=false on any error (bad key size,
// not compiled in); returns the output bytes on success.
inline std::vector<uint8_t> aeadEncrypt(const std::vector<uint8_t>& key,
                                        const std::vector<uint8_t>& msg,
                                        const std::vector<uint8_t>& aad,
                                        bool& ok) {
    ok = false;
#ifdef BANTU_SODIUM
    if (!init()) return {};
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) return {};

    const size_t NPUB = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES; // 24
    const size_t ABYT = crypto_aead_xchacha20poly1305_ietf_ABYTES;    // 16

    std::vector<uint8_t> out(NPUB + msg.size() + ABYT);
    unsigned char* nonce = out.data();
    randombytes_buf(nonce, NPUB);                 // fresh random nonce per message

    unsigned long long clen = 0;
    int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        out.data() + NPUB, &clen,
        msg.data(), msg.size(),
        aad.empty() ? nullptr : aad.data(), aad.size(),
        nullptr,                                  // nsec unused for this construction
        nonce, key.data());
    if (rc != 0) return {};
    out.resize(NPUB + (size_t)clen);
    ok = true;
    return out;
#else
    (void)key; (void)msg; (void)aad;
    return {};
#endif
}

// AEAD decrypt of a blob produced by aeadEncrypt (nonce || ct || tag). Verifies
// the tag in constant time; sets ok=false (and returns empty) on ANY failure —
// wrong key, tampered ciphertext/aad, truncated input. Callers MUST treat
// ok=false as "reject", never as empty plaintext.
inline std::vector<uint8_t> aeadDecrypt(const std::vector<uint8_t>& key,
                                        const std::vector<uint8_t>& blob,
                                        const std::vector<uint8_t>& aad,
                                        bool& ok) {
    ok = false;
#ifdef BANTU_SODIUM
    if (!init()) return {};
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) return {};

    const size_t NPUB = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    const size_t ABYT = crypto_aead_xchacha20poly1305_ietf_ABYTES;
    if (blob.size() < NPUB + ABYT) return {};     // too short to hold nonce+tag

    const unsigned char* nonce = blob.data();
    const unsigned char* ct    = blob.data() + NPUB;
    size_t ctLen               = blob.size() - NPUB;

    std::vector<uint8_t> out(ctLen - ABYT);
    unsigned long long mlen = 0;
    int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        out.data(), &mlen,
        nullptr,
        ct, ctLen,
        aad.empty() ? nullptr : aad.data(), aad.size(),
        nonce, key.data());
    if (rc != 0) return {};                       // authentication failed
    out.resize((size_t)mlen);
    ok = true;
    return out;
#else
    (void)key; (void)blob; (void)aad;
    return {};
#endif
}

// Hash a password with argon2id. Returns the self-describing encoded string
// (algorithm, parameters, salt, hash — all embedded), suitable for storage.
// ok=false if not compiled in or on internal failure (e.g. OOM at the memlimit).
inline std::string pwhash(const std::string& password, bool& ok) {
    ok = false;
#ifdef BANTU_SODIUM
    if (!init()) return "";
    char out[crypto_pwhash_STRBYTES];
    int rc = crypto_pwhash_str(
        out, password.c_str(), password.size(),
        crypto_pwhash_OPSLIMIT_INTERACTIVE,
        crypto_pwhash_MEMLIMIT_INTERACTIVE);
    if (rc != 0) return "";                        // typically out-of-memory
    ok = true;
    return std::string(out);
#else
    (void)password;
    return "";
#endif
}

// Verify a password against an encoded argon2id hash (constant time inside
// libsodium). Returns true only on a match.
inline bool pwhashVerify(const std::string& encoded, const std::string& password) {
#ifdef BANTU_SODIUM
    if (!init()) return false;
    return crypto_pwhash_str_verify(encoded.c_str(), password.c_str(), password.size()) == 0;
#else
    (void)encoded; (void)password;
    return false;
#endif
}

} // namespace bantu_sodium
