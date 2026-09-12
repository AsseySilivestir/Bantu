# Crypto Suite — Hardening (C1–C7)

Follow-on to the initial suite. Fixes the documented challenges in `hash`/`crypto`/`uuid` by
touching the interpreter — **additive and capability-gated**, because Bantu is in production. The
pure-Bantu algorithms remain the auditable reference and the portability fallback; native fast paths
only *accelerate* them, guarded by a differential test that asserts `native == pure` on the official
vectors.

Status: `[x]` done · `[~]` wip · `[ ]` todo

---

## C1 — Throughput (was ~3 KB/s) → native digest accelerators  `[x]`
- **What:** byte-identical C++ digests (`crypto_native.hpp`) that the `hash` module delegates to via
  `has_native(...)`; `native_hash_file` reads+digests a file entirely in C++.
- **Result:** hundreds of MB/s; a 5 MB file (SHA-256 **and** SHA-512) hashes in ~0.15 s, matching
  `shasum`. Pure path still runs (fallback) at ~3 KB/s.
- **Proof:** `tests/crypto_differential_test.b` (native == pure over boundary lengths);
  `hash/hash_test.b` green on both native (new build) and pure (old binary) paths.

## C2 — No 64-bit words → native SHA-512 / SHA-384  `[x]`
- **What:** `sha512`/`sha384` (+`_hex`/`_bytes`) native-only (float64 can't hold 64-bit words; no
  pure fallback — returns `null` if the accelerator is absent).
- **Proof:** official `sha512("")`, `sha512("abc")`, `sha384("abc")`, and a >128-byte multi-block
  vector, all in the differential test.

## C3 — No AES/AEAD, no password KDF → libsodium (feature-gated)  `[x]`
- **What:** `crypto_sodium.hpp` binds libsodium: XChaCha20-Poly1305-IETF AEAD (random 192-bit nonce,
  constant-time tag) and argon2id password hashing. Exposed as `crypto.encrypt/decrypt`,
  `crypto.hash_password/verify_password`, gated on `crypto.encryption_available()`.
- **Production-safe:** compiled only with `BANTU_SODIUM=1` (statically links libsodium → no runtime
  dep); the **default build is unchanged** and gains no dependency. We never hand-roll these.
- **Proof:** `crypto/crypto_encrypt_test.b` — round-trip, nonce randomization, wrong-key/AAD/tamper
  all rejected, argon2id verify accept/reject, salt uniqueness (13/13 on a sodium build; SKIPs
  cleanly otherwise).

## C4 — Bytes = list-of-ints → (pragmatic) native paths avoid the tax  `[~ deferred]`
- **Decision:** a first-class `bytes` Value variant touches equality/serialization/OOP switches
  across the core — too invasive to rush into a production interpreter. The C1 win is achieved
  **without** it: native builtins read `string`/list directly, and `native_hash_file` keeps big
  data out of the interpreter entirely. A real `bytes` type stays a **decoupled future item**.

## C5 — MD5/SHA-1 misuse guard  `[x]`
- **What:** one-time **stderr** notice on `md5(...)`/`sha1(...)` full digests (not on the `*_bytes`
  cores, so UUID v3/v5 stay silent); silence with `hash.allow_insecure(true)`. Added an `eprint`
  builtin so the notice never corrupts stdout.

## C6 — Transitive dependency resolution  `[x]`
- **What:** `installPackage` recurses into the installed package's `dependencies` (cycle-safe
  visited-set) — `bantu add crypto` now auto-pulls `hash`. Backward compatible (no-deps packages
  behave as before).
- **Proof:** fresh project `bantu add crypto` installs `crypto` **and** `hash`.

## C7 — CSPRNG portability + fail-closed  `[x]`
- **What:** `bantuCsprng` now covers Windows (`BCryptGenRandom`, links `-lbcrypt`) and Linux
  (`getrandom(2)` → `/dev/urandom`), and **fails closed** — returns false rather than ever falling
  back to a predictable PRNG.

---

## Later (decoupled, not in this pass)
- First-class `bytes` type (C4), a real i64 integer type (would let 64-bit algos be written in
  Bantu), and a bytecode VM (the general tree-walk perf answer). Each is language-wide and staged
  separately; none change the module APIs above.

## Verification snapshot (default no-sodium build)
`lang 28 · scope 8 · primitives 40 · differential 69 · hash 25 · crypto 31 · encrypt SKIP · uuid 17
· random 33 · orm 61` — all green; lint clean; digests cross-checked vs `shasum`/`md5`.
