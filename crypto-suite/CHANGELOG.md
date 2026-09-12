# Crypto Suite — Changelog (initiative-scoped)

Granular progress log for the `hash`/`crypto`/`uuid` initiative. Tags match the repo convention:
`[feature]`, `[bug fix]`, `[patch]`. The top-level `CHANGELOG.md` receives summarized `[feature]`
entries at ship time; this file tracks day-to-day progress. Newest first.

## [Unreleased]

### 2026-08-28
- **[bug fix]** Function-local variable scoping (`environment.hpp` + `evaluator.hpp`). Stress
  testing revealed the "O(N²)" from D8 was actually a scoping defect: `$x = v` in a function
  clobbered a same-named caller/global variable, so hashing in a loop (`$i` collision) hung. Now
  function-local via `assign()` + `functionScope` boundaries (call/module/global envs). Also fixes
  module top-level `$vars` leaking to global. Guarded by `tests/scope_test.b` (8/8). Zero
  regressions: lang/classes 28, ORM+Sua 61, random 33, orm-demo, live Sua HTTP server.
- **[patch]** Stress suite (all vs the local build, ALL GREEN): differential fuzz of 480 digests
  (sha256/sha1/md5 + base64) over 120 random inputs vs Python `hashlib`/`base64` → 0 mismatches;
  8KB multi-block sha256 matches `openssl`; 2000× uuid4 all distinct + valid v4; base64 round-trip
  80/80; Sua server returning module-computed hash/uuid/HMAC. Throughput note: pure-Bantu hashing is
  ~3 KB/s (great for passwords/tokens/messages/UUIDs; not for large files).
### 2026-08-27
- **[feature]** Phase 4: `uuid/uuid.b` — RFC 4122/9562 `uuid4` (CSPRNG), `uuid7` (time-ordered),
  `uuid3`/`uuid5` (MD5/SHA-1), namespaces, `parse/format/is_valid/version_of`. Name-based vectors
  bit-exact (`uuid5(DNS,"python.org")=886313e1-…`). `uuid/uuid_test.b` → 17/17 ALL GREEN. Docs
  `docs/uuid.md`.
- **[feature]** Phase 3: `crypto/crypto.b` — CSPRNG secure random (`random_bytes/token_hex/
  token_urlsafe/random_int`), HMAC-SHA256 + constant-time `verify_hmac`, HKDF-SHA256 (RFC 5869),
  base64/base64url/hex. Vectors: RFC 4648 + RFC 5869 case 1. `crypto/crypto_test.b` → 31/31 ALL
  GREEN. AES/password-KDF deliberately excluded (documented). Docs `docs/crypto.md`.
- **[feature]** Phase 2: `hash/hash.b` — MD5, SHA-1, SHA-224, SHA-256, HMAC-SHA256 (pure Bantu on
  the native primitives), plus non-crypto `djb2`/`fnv1a` and `hash_file`. Bit-exact to RFC/NIST
  vectors and cross-checked vs `shasum`/`md5`/`openssl`. Tests `hash/hash_test.b` → 25/25 ALL GREEN
  in 0.34s (branchless compression loops → no O(N²)). MD5/SHA-1 documented as non-secure. Docs at
  `docs/hash.md`.
- **[feature]** Phase 1: native crypto primitives in `evaluator.hpp` (additive) — u32 bitwise/modular
  `band/bor/bxor/bnot/shl/shr/rotl/rotr/add32/mul32`; byte/hex `bytes/frombytes/ord/tohex/fromhex`
  and `chr` extended 0–127 → 0–255; OS CSPRNG `randbytes(n)` (arc4random/`/dev/urandom`);
  constant-time `ct_equal`. Bytes represented as a Bantu list of 0–255. Tests:
  `tests/crypto_primitives_test.b` → 40/40 ALL GREEN. Regressions clean: lang 28/28, ORM+Sua 61/61,
  random 33/33, orm-demo end-to-end. Built to `build/bantu` only — production binary untouched.
- **[patch]** Phase 0: created initiative tracking docs — `ROADMAP.md` (feature/test checklist),
  `DECISIONS.md` (D1–D8 rationale), and this changelog. Scope locked to **Core 32-bit**: native
  primitives + pure-Bantu algorithms; SHA-512/AES/password-KDF out of scope; CSPRNG-only for
  secrets; MD5/SHA-1 shipped but marked non-secure.

<!--
Append one line per landed item as work proceeds, e.g.:
- [feature] u32 bitwise builtins band/bor/bxor/bnot/shl/shr/rotl/rotr + add32/mul32 (evaluator.hpp)
- [feature] byte builtins bytes/frombytes/ord + chr extended to 0–255 + tohex/fromhex
- [feature] randbytes(n) OS CSPRNG; ct_equal constant-time compare
- [feature] hash/hash.b: md5/sha1/sha224/sha256/hmac_sha256 (bit-exact to RFC/NIST vectors)
- [feature] crypto/crypto.b: random_bytes/token_*/random_int, hkdf_sha256, base64/base64url, verify_hmac
- [feature] uuid/uuid.b: uuid4/uuid7/uuid3/uuid5, namespaces, parse/format/is_valid/version_of
- [bug fix] evaluator: <O(N^2) scope-accumulation fix, if pursued>
-->
