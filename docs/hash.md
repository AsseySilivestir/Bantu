# Bantu `hash`

Cryptographic and non-cryptographic hashing, written **in pure Bantu** on the interpreter's native
32-bit primitives (`band/bxor/rotr/add32/…`, `bytes/tohex`). Every digest is **bit-exact to the
official RFC/NIST test vectors** and cross-checked against `shasum`/`md5`/`openssl`.

- **Source:** [`hash/hash.b`](../hash/hash.b) · **Tests:** [`hash/hash_test.b`](../hash/hash_test.b)

```bantu
include "./hash.b" as hash;

print(hash.sha256("abc"));   // ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
print(hash.md5("abc"));      // 900150983cd24fb0d6963f7d28e17f72
print(hash.hmac_sha256("key", "message"));   // hex MAC
```

```sh
bantu run hash/hash_test.b   # 25 vectors → RESULT: ALL GREEN
```

---

## ⚠ Security notice

**MD5 and SHA-1 are cryptographically broken** (practical collisions exist). This module ships them
for **checksums, legacy interop, and RFC-4122 UUID namespaces only**. Do **not** use them for
integrity against an adversary, digital signatures, passwords, or deduplicating untrusted data.
Use **SHA-256** and **HMAC-SHA256** for anything security-relevant.

The non-cryptographic hashes (`djb2`, `fnv1a`) are for hash tables / checksums and are **not**
collision-resistant.

---

## API

Every function accepts a **string or a byte-list** (a list of integers 0–255). The default form
returns a lowercase **hex string**; the `*_bytes` form returns a **byte-list**.

| Function | Returns | Notes |
|---|---|---|
| `hash.md5(x)` / `md5_bytes(x)` | hex / bytes | RFC 1321 · **non-secure** |
| `hash.sha1(x)` / `sha1_bytes(x)` | hex / bytes | RFC 3174 · **non-secure** |
| `hash.sha224(x)` / `sha224_bytes(x)` | hex / bytes | FIPS 180 |
| `hash.sha256(x)` / `sha256_bytes(x)` | hex / bytes | FIPS 180 |
| `hash.hmac_sha256(key, msg)` / `hmac_sha256_bytes(...)` | hex / bytes | RFC 2104 / 4231 |
| `hash.djb2(x)` | 32-bit number | non-crypto |
| `hash.fnv1a(x)` | 32-bit number | non-crypto |
| `hash.hash_file(path, algo)` | hex | `algo` ∈ `"md5"`,`"sha1"`,`"sha224"`,`"sha256"` |

Each `*_hex(x)` alias is also provided (identical to the bare form).

### Verifying a MAC safely
Compare MACs with the constant-time primitive to avoid timing side-channels:

```bantu
$expected = hash.hmac_sha256_bytes($key, $msg);
$actual   = fromhex($tagFromClient);
if (ct_equal($expected, $actual)) { /* authentic */ }
```

---

## SHA-512 / SHA-384
`sha512`, `sha384` (plus `*_hex` / `*_bytes`) are also provided. Their 64-bit words can't be
represented exactly by Bantu's float64, so these are **native-only**: they return a digest when the
interpreter ships the native accelerator (the normal case) and `null` on a build without it.

```
print(hash.sha512("abc"));  // ddaf35a1…54ca49f
print(hash.sha384("abc"));  // cb00753f…34c825a7
```

## How it works
The hash **algorithms are written in Bantu** on the native 32-bit primitives
(`add32`/`mul32`/bitwise), and are **bit-exact to the RFC/NIST vectors**. For speed, each public
digest transparently **delegates to a byte-identical native (C++) accelerator** when the interpreter
provides one (`has_native(...)`); otherwise it runs the pure-Bantu reference. A differential test
(`tests/crypto_differential_test.b`) asserts the two paths agree over random inputs at every block
boundary, so the fast path can never drift from the audited spec.

**Performance.** With the native accelerator (default build) throughput is **hundreds of MB/s** — a
multi-MB file hashes in milliseconds, and `hash_file` reads + digests the file entirely in C++ (the
bytes never cross into the interpreter). On a build without the accelerator the pure-Bantu path still
runs correctly at ~3 KB/s (fine for passwords/tokens/UUIDs; slow for large files). Correctness is
validated by differential fuzzing against Python's `hashlib` and cross-checked against
`openssl`/`shasum`/`md5`.

**MD5 / SHA-1 notice.** Calling `md5(...)` or `sha1(...)` as a full digest prints a one-time notice
to **stderr** steering you to SHA-256/HMAC (they remain available for checksums / UUID v3·v5).
Silence it with `hash.allow_insecure(true)`. The `*_bytes` cores (used by UUID v3/v5) never warn.
