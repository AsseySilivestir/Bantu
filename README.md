# Bantu

**Bantu Programming Language v1.2.1 — Stable Release**

A high-level, dynamically-typed programming language implemented as a tree-walking interpreter in C++17. The entire toolchain — interpreter, package manager, HTTP server, WebRTC engine, SQLite/PostgreSQL/MySQL drivers, project scaffolding, VSCode extension, and Windows installer generator — ships as a single ~660 KB static binary with zero runtime dependencies.

```bash
bantu --version     # → Bantu v1.2.1
bantu init myapp
cd myapp && bantu run
```

## What's New in v1.2.1

| Feature | Highlights |
|---|---|
| **`include` keyword** | Language-level module imports. `include "./routes.b";` (direct) or `include "./ctrl.b" as ctrl;` (namespaced). Path resolution + cycle guard. |
| **PostgreSQL driver** | `sua.postgres.connect/query/exec/close`. Default binary ships a deterministic stub; build with `-DBANTU_POSTGRES=ON` for real libpq. |
| **MySQL driver** | `sua.mysql.connect/query/close`. Same stub-or-real pattern via `-DBANTU_MYSQL=ON`. |
| **WebRTC engine** | `sua.webrtc.peer/createOffer/createAnswer/addIceCandidate/dataChannel/send/close`. Real libdatachannel when built with `-DBANTU_WEBRTC=ON`. |
| **VSCode extension** | Syntax highlighting, autocomplete, hover hints, Go-to-Symbol, snippets, **blue-B file icon** for `*.b` files, one-click Run (F5). |
| **`bantu build-windows`** | One command → NSIS `.exe` installer. Bundles interpreter + source + launcher + Start Menu shortcuts + uninstaller. No admin rights required. |
| **`bantu bench`** | Built-in micro-benchmark suite (fib, loops, list/dict ops, string concat). |
| **Three sample apps** | `samples/blogsite` (modular Sua + SQLite), `samples/webrtc-chat` (signaling + browser UI), `samples/pg-dashboard` (PostgreSQL analytics). |
| **PDF documentation** | 30-page official guide at `docs/Bantu-Programming-Language-v1.2.1.pdf`. |

## Download

Grab the latest zip from the [v1.2.1 release](https://github.com/AsseySilivestir/Bantu/releases/tag/v1.2.1):

| Asset | Size | Platform | Includes |
|---|---|---|---|
| [`Bantu-v1.2.1-linux-x64.zip`](https://github.com/AsseySilivestir/Bantu/releases/download/v1.2.1/Bantu-v1.2.1-linux-x64.zip) | ~350 KB | Linux x86-64 | Pre-built `bantu` binary + samples + docs + VSIX |
| [`Bantu-v1.2.1-windows-x64.zip`](https://github.com/AsseySilivestir/Bantu/releases/download/v1.2.1/Bantu-v1.2.1-windows-x64.zip) | ~840 KB | Windows x64 | Full C++ source + `setup.bat` + samples + VSIX |
| [`bantu-vscode-1.2.1.vsix`](https://github.com/AsseySilivestir/Bantu/releases/download/v1.2.1/bantu-vscode-1.2.1.vsix) | ~24 KB | VSCode 1.75+ | Standalone extension (also inside both zips) |
| [`Bantu-Programming-Language-v1.2.1.pdf`](https://github.com/AsseySilivestir/Bantu/releases/download/v1.2.1/Bantu-Programming-Language-v1.2.1.pdf) | ~65 KB | Any | 30-page official guide |

## Quick Start

Bantu v1.2.1 ships as a **zip distribution** with a built-in PATH integrator
and an offline package manager — so you can scaffold, install, and run new
Bantu projects with the same ergonomics as `npm init`, `cargo new`, or
Spring Initializr. **No internet required.**

### One-time setup (Linux x86-64)

```bash
# 1. Download and unzip the release
curl -L -o bantu.zip https://github.com/AsseySilivestir/Bantu/releases/download/v1.2.1/Bantu-v1.2.1-linux-x64.zip
unzip bantu.zip
cd bantu-v1.2.1-linux-x64

# 2. Add bantu to PATH (one-time) + seed local registry
chmod +x bantu
./bantu setup --seed

# 3. Open a NEW terminal (so PATH reloads), then verify
bantu --version
# → Bantu v1.2.1
```

### One-time setup (Windows x64)

Download `Bantu-v1.2.1-windows-x64.zip`, unzip, then build `bantu.exe` once
(requires Visual Studio 2022 with C++ workload, or MinGW-w64):

```bat
cd bantu-v1.2.1-windows-x64\bantu-src\compiler
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
copy build\Release\bantu.exe ..\..\..\
cd ..\..\
setup.bat --seed
```

Open a **new** terminal, then `bantu --version` → `Bantu v1.2.1`.

### Your first project

```bash
bantu init myproject         # new CLI project
cd myproject
bantu run                    # runs main.b

# Or scaffold a Sua web app (Spring Initializr-style)
bantu init --web shop
cd shop && bantu run server.b

# Install packages (offline, from the seeded local registry)
bantu search                 # browse available packages
bantu add math-utils         # install + add to bantu.json
bantu list                   # show installed packages
```

### VSCode extension (optional, recommended)

```bash
code --install-extension bantu-vscode-1.2.1.vsix
```

Gives you `.b` syntax highlighting, autocomplete, hover hints, snippets,
Go-to-Symbol, and the **blue-B file icon** in the VSCode explorer.

## Hello, Bantu!

```bantu
// hello.b
print("Hello, Bantu!");
```

```bash
bantu run hello.b
# → Hello, Bantu!
```

## Modular App (v1.2.1 `include`)

```bantu
// server.b
include "./db.b";              // direct — brings listPosts, getPost, createPost into scope
include "./routes.b" as routes; // namespaced — exposes routes.registerAll(sua)

initDb();
routes.registerAll(sua);
sua.server.listen(3000);
```

## Sua Web Framework

```bantu
sua.server.get("/api/health", def($req, $res) {
    $res.json({ "ok": true, "version": "1.2.1" });
});

sua.server.post("/api/users", def($req, $res) {
    $name = $req.body["name"];
    $id = createUser($name);
    $res.status(201);
    $res.json({ "id": $id, "name": $name });
});

sua.server.listen(3000);
```

## Database Drivers

```bantu
// SQLite (always available)
sua.sqlite.connect("app.db");
$rows = sua.sqlite.query("SELECT * FROM users");

// PostgreSQL (real with -DBANTU_POSTGRES=ON)
sua.postgres.connect("host=localhost dbname=app user=postgres password=secret");
$rows = sua.postgres.query("SELECT * FROM users");

// MySQL (real with -DBANTU_MYSQL=ON)
sua.mysql.connect("localhost", "root", "secret", "app", 3306);
$rows = sua.mysql.query("SELECT * FROM users");
```

## WebRTC (v1.2.1)

```bantu
$peer  = sua.webrtc.peer("alice");
$offer = sua.webrtc.createOffer("alice");
$chan  = sua.webrtc.dataChannel("chat");
sua.webrtc.send("chat", "hello, world!");
```

## Build Windows Installer (v1.2.1)

```bash
# Generates dist/MyApp-Setup-1.0.0.exe (NSIS installer)
bantu build-windows --name "MyApp" --version "1.0.0"
```

## Build from Source

```bash
git clone https://github.com/AsseySilivestir/Bantu.git
cd Bantu/bantu-src/compiler
mkdir build && cd build

# Default build (stub drivers, ~660 KB binary)
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Full build with all real drivers
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DBANTU_POSTGRES=ON \
         -DBANTU_MYSQL=ON \
         -DBANTU_WEBRTC=ON
make -j$(nproc)

sudo cp bantu /usr/local/bin/
bantu --version    # → Bantu v1.2.1
```

## Repository Layout

```
Bantu/
├── bantu-src/compiler/      # C++17 interpreter (lexer, parser, evaluator, server, package_manager)
│   ├── src/
│   │   ├── main.cpp         # CLI entry point (run/build/init/setup/build-windows/bench/...)
│   │   ├── lexer.hpp        # Tokenizer + keywords
│   │   ├── parser.hpp       # Recursive-descent parser
│   │   ├── evaluator.hpp    # Tree-walking evaluator (3.4k lines, includes sua.* framework)
│   │   ├── module_resolver.hpp  # [v1.2.1] include path resolution
│   │   └── ...
│   └── CMakeLists.txt       # Build config (with -DBANTU_POSTGRES/MYSQL/WEBRTC flags)
├── drivers/                 # [v1.2.1] Optional real-driver glue
│   ├── postgres_driver.hpp  # libpq wrapper (HAS_LIBPQ)
│   ├── mysql_driver.hpp     # mysqlclient wrapper (HAS_MYSQL)
│   └── webrtc_engine.hpp    # libdatachannel wrapper (HAS_RTC)
├── vscode-extension/        # [v1.2.1] VSCode extension
│   ├── package.json
│   ├── syntaxes/bantu.tmLanguage.json
│   ├── snippets/bantu.json
│   ├── language/bantu-language-configuration.json
│   ├── icons/               # blue-B file icon (light + dark)
│   ├── src/                 # TypeScript sources (extension, completion, hover, symbol, task)
│   └── README.md
├── windows-installer/       # [v1.2.1] NSIS template reference
├── samples/                 # [v1.2.1] Real apps
│   ├── blogsite/            # Modular Sua + SQLite blog (uses include keyword)
│   ├── webrtc-chat/         # WebRTC signaling + browser UI
│   └── pg-dashboard/        # PostgreSQL analytics dashboard
├── benchmarks/              # [v1.2.1] bench.b + run.sh + results.md
├── docs/
│   └── Bantu-Programming-Language-v1.2.1.pdf  # 30-page official guide
├── windows/                 # Windows .bat helpers (start, stop, reset-db)
├── public/                  # Sua default static files
├── README.md                # This file
├── CHANGELOG.md             # v1.0.0 → v1.1.0 → v1.2.0 → v1.2.1
├── LICENSE                  # MIT
└── QUICKSTART.md
```

## Documentation

- **Official guide:** [`docs/Bantu-Programming-Language-v1.2.1.pdf`](docs/Bantu-Programming-Language-v1.2.1.pdf) — 30 pages, covers every feature
- **Quick start:** [`QUICKSTART.md`](QUICKSTART.md)
- **Samples:** [`samples/`](samples/) — three runnable apps
- **Benchmarks:** [`benchmarks/`](benchmarks/) — run `./benchmarks/run.sh`
- **VSCode extension:** [`vscode-extension/README.md`](vscode-extension/README.md)
- **Changelog:** [`CHANGELOG.md`](CHANGELOG.md)

## Benchmarks (Reference)

Generic x86-64 Linux, Ubuntu 22.04, GCC 11, single core, Release build:

| Benchmark | Bantu v1.2.1 | Node.js 20 | Python 3.11 | Lua 5.4 |
|---|---|---|---|---|
| fib(28) | 614 ms | 38 ms | 285 ms | 280 ms |
| 1M arithmetic loop | 196 ms | 2.4 ms | 38 ms | 9.6 ms |
| list push 100k | 142 ms | 4.1 ms | 18 ms | 8.1 ms |
| string concat 10k | 413 ms | 1.9 ms | 9.2 ms | 5.4 ms |
| dict set 100k | 285 ms | 11 ms | 22 ms | 14 ms |

Bantu is ~10–100x slower than V8/CPython on tight loops because it's a tree-walking interpreter without a JIT. The trade-off is binary size (~660 KB) and install simplicity (zero dependencies). For I/O-bound web work, the Sua HTTP server uses native sockets and is competitive with Node's `http` module on the same hardware. The v1.3 roadmap includes a register-based bytecode VM targeting 5–10x speedup.

See [`benchmarks/results.md`](benchmarks/results.md) for full numbers and reproduction steps.

## Community

- **Source:** https://github.com/AsseySilivestir/Bantu
- **Issues:** https://github.com/AsseySilivestir/Bantu/issues
- **Original fork:** https://github.com/AsseySilivestir/bantusua-local

## License

MIT — see [LICENSE](LICENSE).

## Attribution

Bantu was created by **Assey Silivestir Peter**. The language is named after the Bantu language family spoken across sub-Saharan Africa — a nod to the developer's Tanzanian roots and the language's goal of being accessible to developers in low-bandwidth, offline-first environments.

---

*v1.2.1 stable release · June 2026*
