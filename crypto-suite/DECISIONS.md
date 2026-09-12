# Crypto Suite — Design Decisions

Rationale for the choices behind the `hash`/`crypto`/`uuid` modules, so they aren't re-litigated.
Each entry: **decision · why · alternatives rejected · security implication.**

---

### D1 — Engine: native primitives + pure-Bantu algorithms
**Decision:** Add general-purpose native primitives (bitwise, byte access, CSPRNG) to the
interpreter; implement the hash/UUID **algorithms in Bantu** on top.
**Why:** Bantu structurally cannot express crypto today (no bitwise ops, no `ord()`, ASCII-only
`chr()`, no bytes, float64-only). Adding *primitives* (not a C++ crypto library) keeps the
algorithms auditable and genuinely "written in Bantu," while making them correct and fast enough.
**Rejected:** (a) pure-Bantu bit-emulation — correct but unusably slow and error-prone → the user
explicitly rejected "educational"; (b) FFI to OpenSSL — output-buffer marshalling is impractical
(Bantu has no byte type to read digests back), Windows FFI is stubbed; (c) a full C++ crypto layer —
not "in Bantu," and larger attack surface to trust.
**Security:** Correctness is proven by bit-exact official test vectors; the trusted computing base is
small (a handful of arithmetic primitives).

### D2 — Bytes = Bantu list of integers 0–255
**Decision:** Represent binary data as a `list` of numbers 0–255 rather than adding a `bytes` type.
**Why:** Minimal interpreter surface; reuses `len`/indexing/list builtins; avoids a large type
addition for Core.
**Rejected:** a first-class `bytes` type (more invasive, deferred); using `string` as bytes (breaks
on values ≥128 and UTF-8, and there's no `ord`).
**Security:** Avoids silent corruption of binary input/keys/digests that a string-as-bytes model
would cause.

### D3 — CSPRNG only; never the `random` module for crypto
**Decision:** All key/salt/nonce/UUIDv4 material comes from native `randbytes(n)` (OS entropy:
`arc4random_buf` / `getrandom` / `/dev/urandom`).
**Why:** The `random` module is a Mersenne-Twister/LCG — predictable; using it for secrets is the
vulnerability, not the fix.
**Rejected:** seeding the LCG from `clock()` for "randomness" (predictable); reading `/dev/urandom`
from pure Bantu (blocked — no `ord()` to turn bytes ≥128 into numbers).
**Security:** Unpredictable secrets; this is the single most important control in the suite.

### D4 — MD5 and SHA-1 shipped, but documented as NON-secure
**Decision:** Provide `md5`/`sha1` for checksums, legacy interop, and RFC-4122 UUID v3/v5 — with
prominent "broken for security; do not use for integrity/passwords/signatures" notes.
**Why:** They're still needed (ETags, file checksums, UUID namespaces) and users expect them.
**Rejected:** omitting them (breaks UUID v3/v5 and common interop); shipping them silently (a
security expert must label collision-broken primitives).
**Security:** Explicit non-security labeling prevents misuse; steer users to SHA-256/HMAC.

### D5 — u32 only for Core; SHA-512 deferred
**Decision:** Ship the 32-bit families (MD5, SHA-1, SHA-224/256, HMAC-SHA256, HKDF-SHA256). Defer
SHA-384/512 and BLAKE2b.
**Why:** SHA-256 words are 32-bit → exact in float64 with the `add32/mul32` primitives. SHA-512 uses
**64-bit** words, which float64 cannot represent exactly → needs a real int64 path (a bigger,
separate change).
**Rejected:** faking 64-bit with hi/lo 32-bit pairs now (extra complexity/risk for Core).
**Security:** SHA-256 + HMAC-SHA256 cover the vast majority of modern needs; no security lost.

### D6 — AES and password-KDFs (PBKDF2/scrypt/argon2) out of scope for Core
**Decision:** No symmetric encryption or password hashing in Core; documented as a future **vetted
native** layer.
**Why:** AEAD (GHASH/GF-math, nonce discipline) and high-iteration KDFs (600k+ iterations) are
complex, side-channel-sensitive, and impractically slow in a tree-walking interpreter.
Reimplementing them in Bantu would be *less* secure and unusably slow.
**Rejected:** hand-rolled AES-GCM / PBKDF2 (the classic "don't roll your own crypto" trap).
**Security:** Avoids shipping weak/slow primitives users would trust for encryption and passwords.

### D7 — Constant-time comparison for all secret/tag checks
**Decision:** Native `ct_equal`; `verify_hmac` and any tag/digest comparison use it.
**Why:** Byte-by-byte `==` with early exit leaks timing; interpreter-level timing is unreliable, so
the primitive is native.
**Security:** Closes a standard timing side-channel on MAC verification.

### D9 — Fixed a real language bug: function-local variable scoping
**Decision:** Changed plain-variable assignment (`$x = v`) so it resolves only up to the enclosing
**function boundary**, defining a local if not found there — instead of walking the whole scope chain
and mutating a caller's/global's binding. Implemented with a `functionScope` flag on `Environment`
(set on function-call envs, module envs, and the global root) and a new `Environment::assign()`.
**Why:** During stress testing, calling a function that internally reuses a caller's variable name
(e.g. a loop counter `$i`) clobbered the caller's variable → infinite loops. This is what looked
like an "O(N²)"/perf cliff in D8; it was actually a **scoping defect**. Every hash/HMAC uses `$i`,
so `sha256()` called from a `while ($i < n)` loop hung. Reads still fall through all scopes, so
functions can read globals/module state.
**Rejected:** working around it by renaming variables everywhere (fragile; the language stays
broken); leaving it and documenting (a crypto lib you can't call in a loop isn't production-grade).
**Compatibility:** Verified no regressions — lang/classes 28/28, ORM+Sua 61/61, random 33/33,
orm-demo, and a live Sua HTTP server all pass. Existing code was already written defensively around
this (the ORM uses shared dicts for cross-scope state, which is unaffected — field/index mutation
targets the object, not a variable binding). Also fixes module top-level `$vars` leaking into global
(they now stay in the module namespace). Guarded by `tests/scope_test.b`. This supersedes D8: no
mitigation needed — hash cores are fast per-call and now safe to call in loops.

### D8 — O(N²) call-overhead bug: mitigate now, fix as stretch (SUPERSEDED by D9)
**Decision:** Keep hash hot-loops to **native builtins + arithmetic + array indexing only** (no
`if`/user-call inside loops; use constant tables and branchless `Ch/Maj`). Investigate the
underlying evaluator scope-accumulation separately.
**Why:** A single loop-heavy call is fast; the bug only bites *repeated* calls to functions with
`if`/calls inside loops. Native-only hot loops sidestep it, so a single `sha256()` stays fast and
HMAC/HKDF (few calls) remain cheap.
**Security:** No security impact; a real fix (stretch) also benefits the rest of the language.

---

## Hardening pass (C1–C7) — see HARDENING.md for status

### D10 — Native accelerators with a pure-Bantu fallback (not a rewrite)
**Decision:** Add byte-identical native (C++) digests (`crypto_native.hpp`) and have the `hash`
module delegate to them via `has_native(...)`, keeping the pure-Bantu implementations as the
reference + fallback.
**Why:** ~10^5x speedup where it matters, without deleting the auditable spec or breaking builds that
lack the accelerator. `has_native` is probed once, inside try/catch, so the module also runs on an
older interpreter (graceful degradation).
**Rejected:** replacing the Bantu code with C++ (loses auditability, "in Bantu"); leaving it pure
(unusable for files). **Security:** a differential test asserts native == pure on the official
vectors at every block boundary, so the fast path cannot silently diverge.

### D11 — Encryption/passwords bind libsodium, opt-in at build time
**Decision:** AEAD (XChaCha20-Poly1305) and argon2id come from libsodium (`crypto_sodium.hpp`),
compiled only with `BANTU_SODIUM=1` and **statically linked**; the default build has no libsodium
dependency. This *reverses* D6's "out of scope" — but via a vetted library, never hand-rolled.
**Why:** these primitives are exactly what apps need, but must not be reimplemented; feature-gating
+ static link keeps existing production deployments byte-for-byte unaffected until they opt in.
**Security:** random per-message nonce, constant-time tag/verify, memory-hard argon2id, key
zeroization. `null`/throw on failure — never silent.

### D12 — First-class `bytes` type deferred (C4)
**Decision:** Do NOT add a `bytes` Value variant in this pass. Achieve the C1 perf win with native
builtins that read `string`/list directly and a native `hash_file` that keeps large data out of the
interpreter.
**Why:** a new Value type touches equality/serialization/OOP switches across the core — too invasive
to rush into a production interpreter, and unnecessary for the performance goal. Remains a decoupled
future item (alongside an i64 type and a bytecode VM). **Security:** none; correctness unaffected.

### D13 — CSPRNG fails closed
**Decision:** `bantuCsprng` returns false rather than ever producing predictable bytes; added
Windows (`BCryptGenRandom`) and Linux `getrandom(2)` sources.
**Why:** a silent fallback to a PRNG for keys/salts/nonces is the vulnerability the suite exists to
avoid. **Security:** this is the single most important control; callers surface the failure.

### D14 — P-256 and AES-128-GCM are hand-written, as a documented exception to D6/D11
**Decision:** Implement NIST P-256 (ECDH, ECDSA) and AES-128-GCM from scratch in C++
(`p256.hpp`, `aes_gcm.hpp`), compiled unconditionally, reachable **only** through the `webpush_*`
builtins. This is a deliberate exception to D6 ("don't roll your own") and D11 ("encryption comes
from libsodium").
**Why:** Web Push mandates P-256 (RFC 8291 §3.1) and `aes128gcm`. libsodium offers neither — it has
Curve25519/Ed25519 and (X)ChaCha20-Poly1305 only — so there is nothing to delegate to. The remaining
option, linking OpenSSL, would make libcrypto a hard runtime dependency of *every* Bantu binary,
which is precisely what D11's opt-in static link exists to prevent. Once P-256 must be written by
hand (the hard part), AES-128-GCM is a small marginal addition and is fully pinned by NIST vectors.
Unlike sodium and Arrow this is **not** feature-gated: it adds no external dependency, and Bantu's
float64 numbers make a pure-Bantu fallback impossible (the D5 reasoning that makes SHA-512
native-only), so gating it would leave `has_native("webpush")` false with nothing to fall back to.
**Rejected:** linking OpenSSL (new hard dependency); shipping push without a server-side sender
(the feature's whole point); driving OpenSSL through the FFI (its marshalling is NUL-terminated
strings only — it cannot carry binary or out-params).
**Security — the mitigations that make this exception acceptable:**
- **Complete (exception-free) point formulas** (Renes–Costello–Batina, a = −3), so there are no
  `P == Q` or point-at-infinity special cases. Incomplete Jacobian formulas fail on a sparse,
  scalar-dependent input set — the bug that passes a test suite and then corrupts one signature in
  ten thousand.
- **No secret-dependent branches or memory indices:** constant-time `cmov` in a table-free ladder,
  Fermat inversion over a public exponent (binary xgcd is rejected — ECDSA inverts the secret `k`),
  and a table-free bitwise GHASH (the 4-bit table is the published cache-timing target).
- **Deterministic nonces (RFC 6979):** removes nonce reuse, which would leak a long-lived VAPID key,
  and — the deciding argument — makes signing byte-comparable against published vectors.
- **Every modulus-derived constant is computed, not transcribed** (`n0'`, R mod m, R² mod m), so a
  mistyped magic number is impossible. Only p, n, b, Gx, Gy are literals.
- **Subscriber public keys are validated on import** (uncompressed marker, X and Y < p, on-curve).
  `p256dh` is attacker-supplied, so skipping this is the invalid-curve attack.
- **Salt and ephemeral key are not parameters** of `webpush_encrypt`. RFC 8291 §2 requires both
  fresh per message; reusing either reuses (CEK, NONCE) and breaks the AEAD completely.
- **Fail-closed selftest:** FIPS 197, NIST GCM, RFC 5869, RFC 6979 §A.2.5 and RFC 5903 vectors run
  once on first use. On any failure `has_native("webpush")` is false and every entry point returns
  `null`, so a miscompiled binary cannot emit a broken or insecure push.
- **Threat model stated in the headers:** a remote attacker against a server-side VAPID key. A
  co-resident cache-observing attacker is out of scope, so scalar blinding and bitsliced AES are
  deliberately absent.

### D15 — TLS certificate verification is on by default
**Decision:** `bantuHttpRequest` now sets `CURLOPT_SSL_VERIFYPEER`/`VERIFYHOST`; callers opt out
per-request with `sua.http.request({"insecure": true})`.
**Why:** it was unconditionally disabled, so every outbound HTTPS call was unauthenticated. Web Push
POSTs bearer-equivalent VAPID credentials to third-party endpoints, which made this untenable.
**Security:** the escape hatch is explicit and per-request, never global.
