# Bantu `crypto`

Practical cryptography built on the OS CSPRNG (`randbytes`) and the pure-Bantu
[`hash`](hash.md) module: secure random values & tokens, HMAC-SHA256 with constant-time
verification, HKDF-SHA256 (RFC 5869), and base64/base64url/hex encoding.

- **Source:** [`crypto/crypto.b`](../crypto/crypto.b) · **Tests:** [`crypto/crypto_test.b`](../crypto/crypto_test.b)

```bantu
include "./crypto.b" as crypto;

$token = crypto.token_urlsafe(32);              // a URL-safe secret string
$mac   = crypto.hmac_sha256("key", "message");  // hex MAC
if (crypto.verify_hmac("key", "message", $mac)) { /* authentic */ }

$key = crypto.hkdf_sha256($ikm, $salt, "app v1 encryption key", 32);   // 32-byte derived key
```

```sh
bantu run crypto/crypto_test.b   # RFC 4648 + RFC 5869 vectors → ALL GREEN
```

---

## ⚠ Scope & security

- **Randomness comes from the OS CSPRNG only.** Never use the `random` module for keys, salts,
  nonces, tokens, or anything secret — it is a predictable PRNG.
- **Encryption & passwords are backed by libsodium**, not hand-rolled (see *Authenticated
  encryption* below). They're available when the interpreter is built with libsodium
  (`crypto.encryption_available()`); we never implement AEAD or KDFs ourselves.
- **Verify MACs with `verify_hmac`** (constant-time), never with `==` on the hex string.

---

## API

### Secure random
| Function | Returns |
|---|---|
| `crypto.random_bytes(n)` | list of `n` CSPRNG bytes |
| `crypto.token_hex(n)` | `2n`-char hex string of `n` secure bytes |
| `crypto.token_urlsafe(n)` | URL-safe base64 (no padding) of `n` secure bytes |
| `crypto.random_int(max)` | unbiased integer in `[0, max)` (rejection sampling) |

### Integrity
| Function | Returns |
|---|---|
| `crypto.hmac_sha256(key, msg)` / `_bytes` | hex / byte-list MAC |
| `crypto.verify_hmac(key, msg, tag)` | bool, constant-time (`tag` = hex string or bytes) |

### Key derivation
| Function | Returns |
|---|---|
| `crypto.hkdf_sha256(ikm, salt, info, length)` / `_hex` | derived key (bytes / hex), RFC 5869 |

### Encoding
| Function | Returns |
|---|---|
| `crypto.base64_encode/decode(x)` | standard base64 (with padding) |
| `crypto.base64url_encode/decode(x)` | URL-safe base64 (no padding) |
| `crypto.hex_encode/decode(x)` | hex |

### Authenticated encryption (libsodium)
Available when `crypto.encryption_available()` is true (interpreter built with `BANTU_SODIUM=1`,
libsodium statically linked). Uses **XChaCha20-Poly1305-IETF**: a fresh 192-bit random nonce per
message (you never manage nonces), and decrypt returns **`null` on any tampering or wrong key** —
treat `null` as *reject*, never as empty plaintext.

| Function | Returns |
|---|---|
| `crypto.new_key()` | fresh 32-byte AEAD key (byte-list) — store it safely |
| `crypto.encrypt(key, message, aad?)` | base64url token (`nonce‖ciphertext‖tag`) |
| `crypto.decrypt(key, token, aad?)` | plaintext byte-list, or `null` if invalid |
| `crypto.encrypt_bytes` / `decrypt_bytes` | same, with raw byte-list blobs |

```
$key = crypto.new_key();
$tok = crypto.encrypt($key, "attack at dawn", "context-v1");
$pt  = crypto.decrypt($key, $tok, "context-v1");   // frombytes($pt) == "attack at dawn"
```

### Password hashing (argon2id, libsodium)
Memory-hard, salted, self-describing; verification is constant-time.

| Function | Returns |
|---|---|
| `crypto.hash_password(password)` | encoded argon2id string (store this) |
| `crypto.verify_password(encoded, password)` | bool, constant-time |

```
$h  = crypto.hash_password($pw);        // "$argon2id$v=19$m=…,t=…,p=…$salt$hash"
$ok = crypto.verify_password($h, $pw);
```

If encryption isn't compiled in, these throw a clear error; guard with
`crypto.encryption_available()`. Build with libsodium: `BANTU_SODIUM=1 bash bantu-src/compiler/build-mac.sh`.

All functions accept a **string or a byte-list**; decoders return a **byte-list** (use `frombytes`
to get a string back).
