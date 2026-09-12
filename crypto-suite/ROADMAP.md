# Crypto Suite — Roadmap

Production-grade `hash`, `crypto`, and `uuid` modules for Bantu. The **algorithms are written in
pure Bantu**; the interpreter gains a small set of **general-purpose native primitives** (bitwise,
byte access, CSPRNG) that the algorithms build on. Everything is validated **bit-exact against
official test vectors** — that is what makes it production-valid rather than educational.

Status legend: `[ ]` todo · `[~]` in progress · `[x]` done (test vectors pass)

---

## Phase 0 — Tracking docs
- [x] `crypto-suite/ROADMAP.md`, `DECISIONS.md`, `CHANGELOG.md` created

## Phase 1 — Native primitives ✅ DONE (`bantu-src/compiler/src/evaluator.hpp`)
Bytes are represented as a **Bantu list of integers 0–255** (no new type).

| Item | Primitive | Notes / proof |
|---|---|---|
| [x] u32 bitwise | `band bor bxor bnot shl shr rotl rotr` | operate on float64 treated as u32, return u32; `bnot` masks to 32 bits |
| [x] u32 modular | `add32 mul32` | wrap at 2³² (exact — 32-bit sums/products < 2⁵³) |
| [x] byte access | `bytes(str)` → list0–255, `frombytes(list)` → str, `ord(char)`, `chr(n)` 0–255 | `chr` currently ASCII-clamped; must extend to 0–255 |
| [x] hex | `tohex(list)` `fromhex(str)` | lowercase, no separators |
| [x] CSPRNG | `randbytes(n)` → list of n secure bytes | `arc4random_buf` (macOS/BSD) / `getrandom`/`/dev/urandom` (Linux); libc only |
| [x] constant-time | `ct_equal(a,b)` → bool | over byte-lists or strings; no early exit |
| [x] tests | `tests/crypto_primitives_test.b` | op identities, wraparound, round-trips, randbytes length/range, ct_equal |

## Phase 2 — `hash/` module ✅ DONE
| Item | Implemented via | Test vector |
|---|---|---|
| [x] `md5` (+`_hex`/`_bytes`) | u32 ops, little-endian | `md5("")`=`d41d8cd98f00b204e9800998ecf8427e`, `md5("abc")`=`900150983cd24fb0d6963f7d28e17f72` |
| [x] `sha1` | u32 ops, big-endian | `sha1("abc")`=`a9993e364706816aba3e25717850c26c9cd0d89d` |
| [x] `sha224` | sha256 core, truncated | `sha224("abc")`=`23097d22…a2b0416` |
| [x] `sha256` | u32 ops, big-endian | `sha256("")`=`e3b0c442…b855`, `sha256("abc")`=`ba7816bf…f20015ad` |
| [x] `hmac_sha256` (+`_hex`) | sha256 + key padding + bxor | RFC 4231 test cases |
| [x] `djb2`, `fnv1a` | non-crypto (fnv uses `bxor`) | labeled NON-cryptographic |
| [x] `hash_file(path, algo)` | file-I/O builtins | matches `shasum`/`md5` on a file |
| [x] byte-list input + >64-byte multi-block cases | | multi-block correctness |
## Phase 3 — `crypto/` module ✅ DONE
| Item | Implemented via | Test |
|---|---|---|
| [x] `random_bytes(n)`, `token_hex(n)`, `token_urlsafe(n)` | `randbytes` + hex/base64url | length + charset |
| [x] `random_int(max)` | rejection sampling over `randbytes` | unbiased, bounds |
| [x] `hkdf_sha256(ikm,salt,info,len)` | `hmac_sha256` (RFC 5869) | RFC 5869 vectors |
| [x] `verify_hmac` | `hmac_sha256` + `ct_equal` | accept/reject |
| [x] `base64_encode/decode`, `base64url_*`, `hex_*` | arithmetic | RFC 4648 vectors, round-trip, padding |
| [x] OUT OF SCOPE (documented) | AES / password-KDF | future vetted native layer |

## Phase 4 — `uuid/` module ✅ DONE
| Item | Implemented via | Test vector |
|---|---|---|
| [x] `uuid4()` | `randbytes(16)` + set version/variant via `band/bor` | version=4, variant=10xx, uniqueness |
| [x] `uuid7()` | 48-bit `clock()` ms + CSPRNG tail | time-ordered, version=7 |
| [x] `uuid3(ns,name)` (MD5) | `hash.md5` | `uuid3(DNS,"python.org")`=`6fa459ea-ee8a-3ca4-894e-db77e160355e` |
| [x] `uuid5(ns,name)` (SHA-1) | `hash.sha1` | `uuid5(DNS,"python.org")`=`886313e1-3b8a-5372-9b90-0c9aee199e5d` |
| [x] `NAMESPACE_DNS/URL/OID/X500`, `parse/format/is_valid/version_of` | | round-trips |

## Ship ✅ DONE
- [x] `package.json` + `*_test.b` + `docs/{hash,crypto,uuid}.md` per module
- [x] top-level `CHANGELOG.md` `[feature]` entries
- [x] built local `build/bantu`; all suites green; digests cross-checked vs system tools
- [x] stress-tested (differential fuzz vs Python, large multi-block vs openssl, 2000× uuid4, live
  Sua server) — surfaced & fixed the function-local scoping bug
- [x] **installed to production `/usr/local/bin/bantu`** (user-approved 2026-08-28) — prior binary
  backed up to `/usr/local/bin/bantu.bak-20260828-002729`; global `bantu` re-verified: all 243
  assertions green
