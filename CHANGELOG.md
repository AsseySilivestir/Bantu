# Changelog

All notable changes to the Bantu programming language are documented in this file. The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

> **Searchable tags:** every entry below is prefixed with `[feature]`, `[bug fix]`, or
> `[patch]` so you can grep the log, e.g. `grep '\[bug fix\]' CHANGELOG.md`.

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
