# Changelog

All notable changes to the Bantu programming language are documented in this file. The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

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
