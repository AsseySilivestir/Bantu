# Changelog

All notable changes to the Bantu programming language are documented in this file. The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

> **Searchable tags:** every entry below is prefixed with `[feature]`, `[bug fix]`, or
> `[patch]` so you can grep the log, e.g. `grep '\[bug fix\]' CHANGELOG.md`.

## [Unreleased]

### Added

- **[feature] Progressive Web Apps in `sua` (`sua.pwa`)** — any Bantu web app becomes installable and
  offline-capable from one config call. Modelled on Python's **django-pwa**; the research and design
  notes are in [docs/pwa-research.md](docs/pwa-research.md).
  `sua.pwa.configure({...})` takes one flat dict (the `PWA_APP_*` settings minus the prefix) and
  auto-registers **`/manifest.json`**, `/manifest.webmanifest`, **`/serviceworker.js`**,
  **`/offline`** and **`/pwa.js`**. The worker is served from the ROOT so its scope covers the whole
  origin — the single most common PWA bug. The generated worker precaches assets, serves navigations
  network-first with a cache and offline-page fallback, cleans up old cache versions, and handles
  `push`/`notificationclick`. `sua.pwa.meta()` renders the `<head>` block (django-pwa's
  `{% progressive_web_app_meta %}`); with `auto_inject` on it is patched into served HTML
  automatically, so an existing app becomes installable without touching a template.
  `window.BantuPWA` (from `/pwa.js`) exposes registration, `beforeinstallprompt` capture,
  `promptInstall()` and push subscription. Tests: `tests/sua_pwa_test.b` (101) and
  `tests/sua_pwa_http_test.sh` (32, boots a real server and curls it).
- **[feature] Web Push notifications (`sua.push`)** — real RFC 8291 (`aes128gcm`) payload encryption
  and RFC 8292 (VAPID) signing, which django-pwa does *not* provide (that is `django-webpush`).
  `sua.push.keys(path)` generates the VAPID keypair once and reuses it; `configure()` registers the
  subscribe endpoint; `send()`/`send_all()` deliver, and `send_all` **prunes subscriptions the push
  service reports as gone** (404/410). Subscriptions live in their own sqlite store and can be
  grouped by `tag`. Payloads are capped at 3993 octets and rejected before any network call.
- **[feature] P-256 and AES-128-GCM (`p256.hpp`, `aes_gcm.hpp`)** — self-contained, no new
  dependency, compiled unconditionally. Needed because Web Push mandates P-256, which libsodium does
  not provide; see **DECISIONS D14** for why this is a documented exception to "never hand-roll
  crypto" and the mitigations that make it acceptable (complete exception-free point formulas,
  no secret-dependent branches or memory indices, deterministic RFC 6979 nonces, computed rather
  than transcribed constants, validated public-key import). A known-answer selftest
  (FIPS 197, NIST GCM, RFC 5869, RFC 6979 §A.2.5, RFC 5903 — 52 checks) runs once on first use and
  **fails closed**: on any failure `has_native("webpush")` is false and every entry point returns
  `null`. Atoms are exposed as `webpush_*` plus `b64url_encode`/`b64url_decode`.
  Tests: `tests/webpush_test.b` (53).
- **[feature] `sua.http.request(opts)`** — the general HTTP client form, with arbitrary request
  headers and binary-safe bodies (string, byte-list, or object auto-serialised to JSON).
  The convenience helpers could not set an `Authorization` header at all.
- **[feature] `bantu init --pwa <name>`** — scaffolds an installable app: server with PWA and push
  wired up, offline page, placeholder icons, and a README explaining installability and key handling.
- **[feature] `file_exists(path)`** — `readfile()`/`open()` raise on a missing file, so there was no
  way to write a "create it if absent" flow in Bantu.
- **[feature] `docs/sua.md`** — the first reference for the sua framework: routing, `$req`/`$res`,
  static files, PWA, push, the HTTP client, and an honest list of known limitations.
- **[feature] ChatBantu is now a PWA** — installable, offline-capable, and pushes notifications while
  closed. Every existing route is untouched; `notify()` additionally sends a Web Push tagged to the
  recipient, and degrades to in-app-only when push is unavailable.

### Fixed

- **[bug fix] Outbound HTTP truncated binary bodies at the first NUL byte** — `CURLOPT_POSTFIELDS`
  was set without `CURLOPT_POSTFIELDSIZE`, so libcurl called `strlen()` on the buffer. An
  `aes128gcm` push body starts with 16 random octets, so roughly two in five would have been
  silently truncated. Bodies are now length-explicit.
- **[bug fix] TLS certificates were never verified** — `CURLOPT_SSL_VERIFYPEER` was hard-coded to 0,
  making every outbound HTTPS request unauthenticated. Verification is now **on by default**, with an
  explicit per-request `"insecure": true` escape hatch (**DECISIONS D15**).
- **[bug fix] `sua.http.*` logged every request URL to stdout** — unconditionally, which would have
  written subscriber push endpoints into application output. The trace is now opt-in and goes to
  stderr.
- **[bug fix] Static file MIME types** — the table covered nine extensions and lacked
  `.webmanifest` (required for a manifest served as a file), fonts, WebP/AVIF, WASM, and media.
  Now ~28 types. HTML and manifests are also served `no-cache` so a stale shell cannot pin itself.

- **[feature] Native digest accelerators (hash performance)** — byte-identical C++ fast paths for
  MD5/SHA-1/SHA-224/SHA-256/HMAC-SHA256 (`crypto_native.hpp`), which the `hash` module transparently
  delegates to via a new `has_native(name)` feature check, falling back to the pure-Bantu reference
  when absent. Throughput goes from ~3 KB/s to **hundreds of MB/s**; `hash_file` now reads+digests a
  file entirely in C++ (a 5 MB file hashes in ~0.15 s, matching `shasum`). A differential test
  (`tests/crypto_differential_test.b`, 69 assertions) asserts native == pure at every block boundary.
- **[feature] SHA-512 / SHA-384** — `hash.sha512`/`hash.sha384` (+`_hex`/`_bytes`). Native-only
  (64-bit words can't be represented exactly by Bantu's float64); return `null` on a build without
  the accelerator. Validated against the official vectors.
- **[feature] Authenticated encryption + password hashing (libsodium, opt-in)** — `crypto.encrypt`/
  `decrypt` (XChaCha20-Poly1305-IETF, random per-message nonce, constant-time tag; `null` on
  tamper/wrong-key) and `crypto.hash_password`/`verify_password` (argon2id). Backed by libsodium via
  `crypto_sodium.hpp`; **compiled only with `BANTU_SODIUM=1`** (statically linked), so the default
  build gains no runtime dependency. Guard with `crypto.encryption_available()`. Covered by
  `crypto/crypto_encrypt_test.b` (skips cleanly when not built in).
- **[feature] `eprint(...)` builtin** — writes to stderr (diagnostics/warnings without corrupting
  stdout).

### Changed

- **[feature] Transitive dependency resolution** — `bantu add <pkg>` now installs the package's
  declared `dependencies` recursively (cycle-safe), so e.g. `bantu add crypto` auto-pulls `hash`.
  `crypto`/`uuid` manifests declare their `hash` dependency. No-dependency packages behave as before.
- **[patch] MD5/SHA-1 misuse guard** — `md5(...)`/`sha1(...)` full digests now emit a one-time
  stderr notice steering to SHA-256/HMAC (silence with `hash.allow_insecure(true)`); the `*_bytes`
  cores used by UUID v3/v5 stay silent.

### Fixed

- **[bug fix] CSPRNG portability + fail-closed** — `randbytes`/all secret material now sources OS
  entropy on Windows (`BCryptGenRandom`) and Linux (`getrandom(2)` → `/dev/urandom`) as well as
  Apple/BSD, and **fails closed** (errors rather than ever falling back to a predictable PRNG).

- **[bug fix] Function-local variable scoping** — a plain assignment `$x = v` inside a function used
  to walk the whole scope chain and mutate a caller's or a global variable of the same name. A
  callee reusing a caller's loop counter (e.g. `$i`) would reset it, causing infinite loops. Plain
  assignment is now **function-local** (Python-style): it resolves only up to the enclosing function
  boundary and otherwise defines a local; reads still fall through to enclosing scopes so functions
  can read globals. Module top-level `$vars` now correctly stay in the module (and are exposed as
  namespace members) instead of leaking into global. Implemented via an `Environment::assign()` with
  a `functionScope` boundary flag on function-call envs, module envs, and the global root. OOP
  dispatch/inheritance untouched. Guarded by `tests/scope_test.b`; no regressions (lang/classes,
  ORM, Sua, random all green). This is what let the hash/HMAC/UUID functions be called in loops.

### Added

- **[feature] Native crypto primitives** — additive builtins in the interpreter enabling
  production-grade cryptography written in pure Bantu: 32-bit bitwise/modular ops
  (`band/bor/bxor/bnot/shl/shr/rotl/rotr/add32/mul32`), byte/hex conversion (`bytes/frombytes/ord/
  tohex/fromhex`, with `chr` extended 0–127 → 0–255), an OS CSPRNG (`randbytes`), and constant-time
  `ct_equal`. Bytes are represented as a Bantu list of 0–255. Covered by
  `tests/crypto_primitives_test.b` (40 assertions).
- **[feature] `hash` package** — MD5, SHA-1, SHA-224, SHA-256 and HMAC-SHA256, written in pure
  Bantu on the primitives above and **bit-exact to the RFC/NIST test vectors** (cross-checked vs
  `shasum`/`md5`/`openssl`), plus non-crypto `djb2`/`fnv1a` and `hash_file`. MD5/SHA-1 documented as
  non-secure (checksums/UUID-namespace only). `hash/hash_test.b` (25 vectors). Docs `docs/hash.md`.
- **[feature] `crypto` package** — OS-CSPRNG secure random (`random_bytes`, `token_hex`,
  `token_urlsafe`, unbiased `random_int`), HMAC-SHA256 with constant-time `verify_hmac`,
  HKDF-SHA256 (RFC 5869), and base64/base64url/hex (RFC 4648). AES and password-KDFs are
  intentionally out of scope (a future vetted native layer). `crypto/crypto_test.b` (31 tests).
  Docs `docs/crypto.md`.
- **[feature] `uuid` package** — RFC 4122 / RFC 9562 `uuid4` (CSPRNG), `uuid7` (time-ordered),
  `uuid3`/`uuid5` (name-based, bit-exact to RFC vectors), namespaces, and
  `parse/format/is_valid/version_of`. `uuid/uuid_test.b` (17 tests). Docs `docs/uuid.md`.
- **[feature] `random` standard-library package** — a Python-style random-number module written
  entirely in Bantu (`random/random.b`), with a **seedable** pure-Bantu generator so
  `random.seed(n)` reproduces sequences exactly (Bantu's built-in `random()` is a
  Mersenne-Twister that cannot be seeded). API: `seed`/`getstate`/`setstate`, `random`, `uniform`,
  `randint`, `randbelow`, `randrange`, `randrangeStep`, `randbool`, `choice`, `choices`,
  `choicesUniform`, `sample`, `shuffle` (returns a copy — Bantu lists are pass-by-value),
  `gauss`/`normalvariate`, `expovariate`, `triangular`, and `getrandbits`. Engine is a 32-bit LCG
  whose arithmetic stays exact in doubles; documented as non-cryptographic. Include with
  `include "./random.b" as random;`. Covered by `random/random_test.b`; docs at `docs/random.md`.

## [1.3.2] — 2026-09-06

WebSocket + voice release: real-time bidirectional communication via
`sua.ws` namespace with binary frame support for voice/audio data.
Also includes a collaborative IDE demo with chat, voice, and live code
editing.

### Added

- **[feature] WebSocket support (`sua.ws` namespace)** — Bantu now has
  true real-time bidirectional communication via WebSockets (RFC 6455).
  No more HTTP long-polling — this is Socket.IO-speed (sub-50ms latency).
  New builtins: `sua.ws.on`, `sua.ws.send`, `sua.ws.broadcast`,
  `sua.ws.clients`, `sua.ws.send_binary`, `sua.ws.broadcast_binary`,
  `sua.ws.send_to`.
- **[feature] Binary WebSocket frames** — voice/audio data can be
  transmitted as binary frames (opcode 0x02). The on(message) handler
  receives `{data, bytes, binary, client, json}` where `bytes` is
  a list of 0-255 integers and `binary` is true.
- **[feature] Collaborative IDE** — chat + voice + real-time code
  editing demo using CodeMirror, all powered by Bantu WebSocket.
- **[feature] SHA-1 + Base64** — inline implementations for the
  RFC 6455 WebSocket handshake.
- **[feature] Wildcard /* route matching** — enables SPA fallback
  for `bantu-auto-frontend`.

### Changed

- Bumped version constant in `main.cpp`: `1.3.1` → `1.3.2`.

## [1.3.1] — 2026-09-06

Installer + tooling release: real Windows installer with brand icon, live linting in
VS Code that shows red squiggles on syntax errors as you type, and a new `sua.udp`
namespace for native UDP networking. **No language semantics changed** — every v1.3.0
program runs unchanged.

### Added

- **[feature] Native UDP networking (`sua.udp` namespace)** — Bantu can now open raw UDP
  sockets and speak UDP-only protocols directly. Seven new builtins:
  `sua.udp.socket`, `sua.udp.bind`, `sua.udp.send_to`, `sua.udp.recvfrom`,
  `sua.udp.send` (one-shot), `sua.udp.close`, `sua.udp.getsockname`. Verified against
  real DNS (8.8.8.8), STUN (stun.l.google.com), and self-echo. Enables STUN/TURN servers,
  DNS clients, IoT relays, real-time games, all in pure Bantu.
- **[feature] Windows installer with brand icon** — the NSIS installer now embeds the
  official Bantu icon (multi-resolution .ico, 16/32/48/64/128/256 px). Shows in the
  installer wizard, Add/Remove Programs, file associations, and Start Menu shortcuts.
- **[feature] Live linting in VS Code** — the Bantu VS Code extension's
  `diagnosticsProvider.ts` runs `bantu lint --json` on the current buffer every
  300 ms (debounced). Syntax errors show as red squiggles as you type, not only on save.
  The interpreter's compile gate refuses to `run`/`build` files that still contain
  errors, so the editor and the toolchain agree.
- **[feature] VS Code extension icon** — replaced the placeholder blue-B with the
  official Bantu icon at 256×256 px (extension-icon.png) and 64×64 px (file icons).

### Changed

- Bumped version constant in `main.cpp`: `1.3.0` → `1.3.1`.
- VS Code extension package version: `1.3.0` → `1.3.1`.

### Compatibility

- v1.3.1 is a drop-in replacement for v1.3.0. No language changes, no breaking API
  changes. All v1.3.0 programs run unchanged.

## [1.3.0] — 2026-07-10

Core-language correctness release: the features that were advertised via keywords but silently
failed now work, the parser can no longer spin on a bad token, and several requested capabilities
(dict iteration, file I/O, FFI, parameterized SQL) land. **Object-oriented features were not
touched.**

### Fixed

- **[bug fix] Compound assignment** — `+=`, `-=`, `*=`, `/=` now apply the base arithmetic
  operator (the parser had passed the compound token `PLUS_EQUALS` straight into the evaluator,
  which rejected it as "Unknown operator"). This also removes an infinite loop where a `for`
  counter using `i += 1` never advanced. (`parser.hpp` `parseAssignment`)
- **[bug fix] `break` / `continue`** — were inert no-op nodes; they now compile to real AST nodes
  and throw the loop-control signals that `while`/`for`/`each` already caught.
- **[bug fix] `try` / `catch`** — runtime errors are now actually catchable. `ErrorHandler` throws
  a `BantuError` instead of merely printing, so `catch ($e)` receives a structured
  `{message, type, line}` (or the thrown value for `throw`).
- **[bug fix] Deterministic parser recovery** — a syntax error is reported once and the parser
  synchronizes to the next statement instead of re-reading the stuck token forever. `bantu run`
  now prints all diagnostics and refuses to execute (compile gate).
- **[bug fix] `$`-prefixed reserved words** — `$db`, `$create`, `$list`, `$switch`, … are valid
  variable names again; the `$` sigil forces the following word to be an identifier.
- **[bug fix] Bare `return;`** — returning with no value (or at the end of a block) yields null
  instead of a parse error.
- **[bug fix] `push()`, `$l.push()` and `$l.pop()` now mutate** — all three captured the list by
  value and were silent no-ops. They are resolved against the real storage now. `push()` returns
  the mutated list, so the older `$l = push($l, x)` idiom works correctly too.
- **[bug fix] ORM binds values instead of escaping them** — `orm/orm.b` now emits placeholders and
  collects bound parameters (SQLite `?`, PostgreSQL `$1…$n`), retiring string interpolation as the
  injection boundary. `_escape()` remains only for identifiers/DDL defaults and dialects without a
  parameter path. Covered by injection tests in `orm/orm_test.b` (61 assertions).
- **[patch] `sua.mysql.*` documented as a simulation** — `HAS_MYSQL` is defined by CMake but never
  referenced by the evaluator, so MySQL returns canned rows regardless of build flags, and
  `drivers/mysql_driver.hpp` / `drivers/postgres_driver.hpp` are included by nothing. The README's
  "real with `-DBANTU_MYSQL=ON`" claim is corrected; see `docs/v1.3.0-status.md`.

### Added

- **[feature] `switch` / `case` / `default`** — `switch ($x) { case 1 { … } default { … } }`,
  braces required, no fallthrough (first match wins).
- **[feature] `throw`** — `throw <value>;` raises any value; caught by `try/catch`.
- **[feature] `const` is truly constant** — reassigning a `const` binding is now an error (like
  Java `final`), enforced at runtime and flagged by the linter. The referenced object may still
  be mutated.
- **[feature] Anonymous functions** — `def($a, $b) { … }` is a first-class value (usable as a
  dict entry, argument, etc.).
- **[feature] Python-style `for … in …`** — `for $x in $list { }` and
  `for $key, $value in $dict.items() { }`; `each` also accepts a second variable.
- **[feature] Dict iteration & methods** — `$d.items()`, `$d.keys()`, `$d.values()`,
  `$d.size()`, plus `keys()`/`values()`/`entries()` builtins; `.size()` on lists/strings.
- **[feature] In-place list mutators** — `append(l, x)`, `pop(l)`, `insert(l, i, x)`,
  `remove(l, i)`, `extend(l, l2)`.
- **[feature] Python-style file I/O** — `open(path, mode)` (`"r"`/`"w"`/`"a"`),
  `read`, `readline`, `readlines`, `write`, `close`, plus one-shot `readfile`, `writefile`,
  `appendfile`.
- **[feature] FFI via libffi** — `loadlib("libm.dylib")` + `func(lib, "sqrt", "double",
  ["double"])` returns a callable that invokes the C symbol. Types: `int`, `double`, `string`,
  `pointer`, `void`. Built in on Linux/macOS (`-DBANTU_FFI -lffi -ldl`).
- **[feature] Parameterized SQL** — `sua.sqlite.exec/query(sql, [params])` binds `?` placeholders
  via prepared statements, and `sua.postgres.exec/query(sql, [params])` binds `$1…$n` via
  `PQexecParams` (both injection-safe). The ORM now uses this path for every value.
- **[feature] Linter + compile gate** — `bantu lint <file> [--json]` reports syntax errors and
  const/type issues (error = red, warning = yellow). `run`/`build` refuse to execute on errors
  (`--no-lint` opts out). The VS Code extension shows these live as you type.

### Notes

- **[patch]** Deferred to future releases: a bytecode VM (performance), `async`/`await`, and
  native binary compilation.
- **[patch]** Windows FFI is stubbed for now (the builtins raise a clear "not available" error);
  Linux/macOS ship it enabled.

## [1.2.2] — 2026-06-20

### Fixed

- **Module path canonicalization** — `include` now resolves through `realpath()` (POSIX) / `GetFullPathName` (Windows) so the same file reached via different relative paths (`./pkg/x.b`, `../pkg/x.b`, `pkg/x.b`) collapses to a single canonical key. This prevents the cycle guard from accidentally executing a module twice when it is reached via two different paths in the same project.
- **Cycle-guard diagnostic** — Previously, a circular include was silently skipped with no message. It now prints `[INCLUDE] Skipping already-loaded module: <path>` so users can see what was elided. The depth limit also surfaces a clear `[INCLUDE ERROR] Maximum include depth (64) exceeded` if a generated or pathological include chain escapes the cycle guard.
- **Depth limit** — A new `kMaxIncludeDepth = 64` guard prevents stack-exhaustion crashes on pathological include chains (e.g. self-generating scripts).
- **Error attribution** — `Module not found` errors now include the importing file path so the user knows which file made the bad `include` call:
  ```
  [INCLUDE ERROR] Module not found: ./missing.b (imported from /app/routes.b)
  ```
- **`[INCLUDE]` log channel** — Errors now go to `stderr` with the `[INCLUDE ERROR]` tag (was previously mixed into `stdout` as `[INCLUDE]`), so users can filter them separately from informational messages.

### Added

- **`--quiet` / `-q` global flag** — Suppresses informational `[INCLUDE] Loaded …` and `[Executed in … us]` lines. Useful for production server logs and benchmark harnesses. Errors still print to `stderr`.
  ```bash
  bantu --quiet run server.b
  bantu -q run server.b
  bantu run server.b --quiet   # also accepted
  ```
- **`$BANTU_PATH` module search path** — When `include` cannot find a module via the standard resolution order (absolute → importing-file dir → cwd → with `.b` appended), it now falls back to each directory listed in the `BANTU_PATH` environment variable (POSIX `:`-separated, Windows `;`-separated). Lets users install shared module libraries outside their project tree:
  ```bash
  export BANTU_PATH=/opt/bantu/lib:~/bantu-modules
  bantu run app.b   # can `include "auth.b";` from either dir
  ```
- **`bantu installer --platform android`** — Cross-platform desktop installer generator now also produces a complete Android Studio project (Gradle, Kotlin, WebView-based launcher) ready to build into a sideloadable APK. Bundles `.b` sources in `app/src/main/assets/bantu/`, looks for a pre-built arm64 `libbantu.so` in `./android/`, `~/.bantu/android/arm64-v8a/`, or `$BANTU_ANDROID_ARM64`, and ships a `build-apk.sh` wrapper plus `BUILD-ANDROID.md` with full NDK cross-compile instructions. The resulting APK runs fully offline on Android 7.0+ phones with no Bantu installed on the device.

### Changed

- Bumped version constant in `main.cpp`: `1.2.1` → `1.2.2`.
- VSCode extension package version: `1.2.1` → `1.2.2` (re-packaged as `bantu-vscode-1.2.2.vsix`).
- Sample app `bantu.json` versions bumped to `1.2.2`.
- Benchmark script header updated to `Bantu v1.2.2 Benchmark Suite`.

### Compatibility

- v1.2.2 is a drop-in replacement for v1.2.1. No language changes, no breaking API changes. All v1.2.1 programs run unchanged.
- Optional real-driver glue (`-DBANTU_POSTGRES`, `-DBANTU_MYSQL`, `-DBANTU_WEBRTC`) unchanged.

## [1.2.1] — 2026-06-20

### Added

- **`include` keyword** — language-level module imports with two forms:
  - `include "./routes.b";` — direct: symbols flow into the importer's scope
  - `include "./ctrl.b" as ctrl;` — namespaced: symbols bound under an alias
  - Path resolution: relative to importing file → relative to cwd → with `.b` appended
  - Idempotent: a module is executed only once per execution
  - Cycle guard: circular includes are detected and broken silently
- **`sua.include(path)` runtime function** — load a module dynamically, returns it as a dict (does not pollute scope)
- **`sua.webrtc.*` namespace** (v1.2.2) — explicit WebRTC peer / data-channel API:
  - `sua.webrtc.peer(id)`
  - `sua.webrtc.createOffer(peerId)` / `createAnswer(peerId)`
  - `sua.webrtc.addIceCandidate(peerId, candidate)`
  - `sua.webrtc.dataChannel(label)`
  - `sua.webrtc.send(channel, msg)`
  - `sua.webrtc.close(peerId)`
- **`bantu build-windows` command** — generates an NSIS `.exe` installer from any Bantu project
  - Bundles interpreter, source files, manifest, launcher, Start Menu shortcuts, uninstaller
  - `--name <Name>` and `--version <x.y.z>` flags
  - No admin rights required (uses `RequestExecutionLevel user`)
- **`bantu bench` command** — built-in micro-benchmark suite
- **VSCode extension** (`vscode-extension/`) — first-class editor support:
  - TextMate grammar for syntax highlighting
  - Context-aware autocomplete (keywords, types, `sua.*` namespaces & methods, `$variable` hints)
  - Hover hints for every keyword and `sua.*` method
  - Go-to-Symbol (`Ctrl+Shift+O`) for `def`, `class`, `include`, top-level `$var`
  - 20+ snippets (def, class, if, each, include, sua.server, etc.)
  - **Blue-B file icon** for `*.b` files (light + dark variants)
  - Commands: Run File (F5), Initialize Project, Initialize Sua Web Project, Build Windows Installer, Run Benchmarks
- **Three sample apps** in `samples/`:
  - `blogsite/` — modular blog backend (Sua + SQLite, uses `include` keyword across 5 files)
  - `webrtc-chat/` — WebRTC signaling server + browser chat UI
  - `pg-dashboard/` — analytics dashboard backed by PostgreSQL
- **Optional real-driver glue** (`drivers/`) for compile-time linking:
  - `postgres_driver.hpp` (libpq, `-DBANTU_POSTGRES=ON`)
  - `mysql_driver.hpp` (mysqlclient, `-DBANTU_MYSQL=ON`)
  - `webrtc_engine.hpp` (libdatachannel, `-DBANTU_WEBRTC=ON`)
- **Module resolver** (`bantu-src/compiler/src/module_resolver.hpp`) — handles path resolution + lex/parse of included files
- **30-page official PDF guide** at `docs/Bantu-Programming-Language-v1.2.2.pdf`

### Changed

- Bumped version: `1.2.0` → `1.2.1`
- `CMakeLists.txt` now declares `project(bantu VERSION 1.2.2)` and adds three CMake options (`BANTU_POSTGRES`, `BANTU_MYSQL`, `BANTU_WEBRTC`) with `find_path` / `find_library` detection
- `runCode()` in `main.cpp` now calls `evaluator.setEntryPoint(filename)` so that `include` statements resolve relative to the file being run
- `sua.webrtc.peer()` returns a `platform` field indicating whether libdatachannel or the stub is in use

### Documentation

- New 30-page PDF: `docs/Bantu-Programming-Language-v1.2.2.pdf`
- Updated `README.md` to reflect v1.2.2 features
- New `CHANGELOG.md` (this file)
- New `benchmarks/README.md` and `benchmarks/results.md`
- New `vscode-extension/README.md`
- New `samples/blogsite/README.md`, `samples/webrtc-chat/README.md`, `samples/pg-dashboard/README.md`

## [1.2.0] — Earlier

- PATH integration (`bantu setup`, `bantu setup --system`, `bantu setup --seed`)
- Offline package manager (`bantu install / add / remove / update / list / search / publish`)
- `bantu init` and `bantu init --web` scaffolders
- `bantu doctor` diagnostics
- `bantu.json` project manifest
- Local package registry at `~/.bantu/registry/`

## [1.1.0] — Earlier

- Sua HTTP framework (`sua.server.get/post/put/delete/patch/head/options/use/static/listen`)
- Sua HTTP client (`sua.http.get/post/put/delete/patch/head`) backed by libcurl
- Sua JSON helpers (`sua.json.parse/stringify`)
- Sua SQLite driver (`sua.sqlite.connect/exec/query/close`)
- Sua PostgreSQL stub (`sua.postgres.connect/query/close`)
- Sua MySQL stub (`sua.mysql.connect/query/close`)
- Real-time primitives: `sua.channel/signal/stun/broadcast/relay/stream/connect`
- Classes with `extends`, `super`, `public`/`private` visibility
- `try`/`catch`, `switch`/`case`, `each` loop
- Type annotations: `number`, `string`, `bool`, `list`, `dict`, `any`, `func`

## [1.0.0] — Initial Release

- Lexer, parser, tree-walking evaluator
- Variables (`$name = value`), numbers, strings, booleans, lists, dicts
- Control flow: `if`/`else`, `while`, `for`, `break`, `continue`, `return`
- Functions (`def`), closures, higher-order functions
- Built-in `print`, `read`, `clock`, `sleep`, `len`, `keys`, `values`
- REPL
- `bantu run <file.b>` and `bantu build <file.b>` commands
