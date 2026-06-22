# Bantu

**Bantu Programming Language v1.2.2 — Stable Release**

A high-level, dynamically-typed programming language implemented as a tree-walking interpreter in C++17. The entire toolchain — interpreter, package manager, HTTP server, WebRTC engine, SQLite/PostgreSQL/MySQL drivers, project scaffolding, VSCode extension, and cross-platform desktop installer generator — ships as a single ~660 KB static binary with zero runtime dependencies.

```bash
bantu --version     # → Bantu v1.2.2
bantu init myapp
cd myapp && bantu run
```

## What's New in v1.2.2

v1.2.2 is a **drop-in maintenance release** on top of v1.2.1. No language changes, no breaking API changes — every v1.2.1 program runs unchanged. The release adds a new **cross-platform desktop installer generator** and improves robustness of the `include` module system.

| Change | Highlights |
|---|---|
| **`bantu installer` command [NEW]** | One command packages any Bantu project into a cross-platform installer: `.deb` for Linux, `.exe` for Windows (NSIS), `.app` for macOS, and a full Android Studio project + APK for Android phones. **Bundles the bantu interpreter by default** so the app runs on machines without Bantu installed. See `bantu installer --help` or the docs chapter. |
| **Path canonicalization** | `include` paths are now resolved through `realpath()` (POSIX) / `GetFullPathName` (Windows). The same file reached via `./pkg/x.b`, `../pkg/x.b`, and `pkg/x.b` now collapses to a single canonical key — so the cycle guard can no longer accidentally execute a module twice when it is reached via two different relative paths in the same project. |
| **Cycle-guard diagnostic** | Circular includes used to be silently skipped. They now print `Skipping already-loaded module: <path>` (or `[INCLUDE ERROR] Maximum include depth (64) exceeded` for pathological chains). |
| **Depth limit** | New `kMaxIncludeDepth = 64` guard prevents stack-exhaustion crashes on self-generating or pathological include chains. |
| **Error attribution** | `Module not found` errors now include the importing file: `Module not found: ./missing.b (imported from /app/routes.b)`. |
| **`--quiet` / `-q` flag** | `bantu --quiet run server.b` suppresses informational `[INCLUDE] Loaded …` and `[Executed in … us]` lines. Errors still print to `stderr`. Useful for production server logs. |
| **`$BANTU_PATH` search path** | When a module can't be found via the standard resolution order, `include` now falls back to each directory in `$BANTU_PATH` (POSIX `:`-separated, Windows `;`-separated). Enables shared module libraries outside the project tree: `export BANTU_PATH=/opt/bantu/lib:~/bantu-modules`. |

## What's New in v1.2.1 (the foundation release)

| Feature | Highlights |
|---|---|
| **`include` keyword** | Language-level module imports. `include "./routes.b";` (direct) or `include "./ctrl.b" as ctrl;` (namespaced). Path resolution + cycle guard. |
| **PostgreSQL driver** | `sua.postgres.connect/query/exec/close`. Default binary ships a deterministic stub; build with `-DBANTU_POSTGRES=ON` for real libpq. |
| **MySQL driver** | `sua.mysql.connect/query/close`. Same stub-or-real pattern via `-DBANTU_MYSQL=ON`. |
| **WebRTC engine** | `sua.webrtc.peer/createOffer/createAnswer/addIceCandidate/dataChannel/send/close`. Real libdatachannel when built with `-DBANTU_WEBRTC=ON`. |
| **VSCode extension** | Syntax highlighting, autocomplete, hover hints, Go-to-Symbol, snippets, **blue-B file icon** for `*.b` files, one-click Run (F5). |
| **`bantu build-windows`** | One command → NSIS `.exe` installer (Windows-only). Superseded in v1.2.2 by the cross-platform `bantu installer`. |
| **Three sample apps** | `samples/blogsite` (modular Sua + SQLite), `samples/webrtc-chat` (signaling + browser UI), `samples/pg-dashboard` (PostgreSQL analytics). |
| **PDF documentation** | 36-page official guide at `docs/Bantu-Programming-Language-v1.2.2.pdf` (Dracula-themed). |

## Download

Grab the latest zip from the [v1.2.2 release](https://github.com/AsseySilivestir/Bantu/releases/tag/v1.2.2):

| Asset | Size | Platform | Includes |
|---|---|---|---|
| [`Bantu-v1.2.2-linux-x64.zip`](https://github.com/AsseySilivestir/Bantu/releases/download/v1.2.2/Bantu-v1.2.2-linux-x64.zip) | ~380 KB | Linux x86-64 | Pre-built `bantu` binary + samples + docs + VSIX |
| [`Bantu-v1.2.2-windows-x64.zip`](https://github.com/AsseySilivestir/Bantu/releases/download/v1.2.2/Bantu-v1.2.2-windows-x64.zip) | ~650 KB | Windows x64 | Pre-built `bantu.exe` + `install.bat` + samples + VSIX + PDF (no compilation needed) |
| [`bantu-vscode-1.2.2.vsix`](https://github.com/AsseySilivestir/Bantu/releases/download/v1.2.2/bantu-vscode-1.2.2.vsix) | ~24 KB | VSCode 1.75+ | Standalone extension (also inside both zips) |
| [`Bantu-Programming-Language-v1.2.2.pdf`](https://github.com/AsseySilivestir/Bantu/releases/download/v1.2.2/Bantu-Programming-Language-v1.2.2.pdf) | ~75 KB | Any | Dracula-themed 31-page official guide |
| — *(build from source)* | — | **macOS** | No pre-built binary — see [One-time setup (macOS)](#one-time-setup-macos--build-from-source) below |

## Quick Start

Bantu v1.2.2 ships as a **zip distribution** with a built-in PATH integrator
and an offline package manager — so you can scaffold, install, and run new
Bantu projects with the same ergonomics as `npm init`, `cargo new`, or
Spring Initializr. **No internet required.**

### One-time setup (Linux x86-64)

```bash
# 1. Download and unzip the release
curl -L -o bantu.zip https://github.com/AsseySilivestir/Bantu/releases/download/v1.2.2/Bantu-v1.2.2-linux-x64.zip
unzip bantu.zip
cd bantu-v1.2.2-linux-x64

# 2. Add bantu to PATH (one-time) + seed local registry
chmod +x bantu
./bantu setup --seed

# 3. Open a NEW terminal (so PATH reloads), then verify
bantu --version
# → Bantu v1.2.2
```

### One-time setup (Windows x64) — download & double-click

The Windows release ships with a **pre-built `bantu.exe`** — no Visual
Studio, no CMake, no compilation required. The whole setup is three
steps:

1. Download `Bantu-v1.2.2-windows-x64.zip` from the
   [v1.2.2 release](https://github.com/AsseySilivestir/Bantu/releases/tag/v1.2.2).
2. Unzip it anywhere (e.g. `Downloads\Bantu-v1.2.2-windows-x64`).
3. **Double-click `install.bat`** inside the unzipped folder.

The installer:
- Copies `bantu.exe` to `%LOCALAPPDATA%\Bantu\bin\`
- Adds that folder to your user `PATH`
- Registers `.b` as a Bantu source file (double-clicking a `.b` runs it)
- **Auto-installs the VSCode extension** if VSCode is detected
- Creates a desktop shortcut and a Start Menu entry
- Registers an uninstaller in *Add/Remove Programs*

Open a **new** Command Prompt (so PATH reloads) and verify:

```bat
bantu --version
:: → Bantu v1.2.2
```

To uninstall later: run `uninstall.bat` from the original zip folder,
or use *Add/Remove Programs* in Windows Settings.

### One-time setup (macOS) — build from source

The v1.2.2 release ships pre-built binaries for **Linux x86-64** and
**Windows x64** only. There is no pre-built macOS binary in the release
(yet) — on macOS, build `bantu` from source. It's a single C++17 file
that compiles in ~30 seconds with the Xcode command-line tools.

```bash
# 1. Install Xcode command-line tools (one-time, ~200 MB)
xcode-select --install

# 2. Clone the Bantu source
git clone https://github.com/AsseySilivestir/Bantu.git
cd Bantu/bantu-src/compiler

# 3. Build (uses clang++ shipped with Xcode)
./build.sh

# 4. The freshly-built bantu binary lands at ./build/bantu
#    Add it to your PATH:
sudo cp build/bantu /usr/local/bin/bantu
sudo chmod +x /usr/local/bin/bantu

# 5. Open a NEW terminal, then verify
bantu --version
# → Bantu v1.2.2
```

**Optional — real PostgreSQL/MySQL/WebRTC drivers:**

The default macOS build ships deterministic stubs (same as Linux/Windows
release builds). To link real drivers:

```bash
# Install dependencies via Homebrew
brew install libpq mysql-client libdatachannel

# Rebuild with the drivers you want
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DBANTU_POSTGRES=ON \
         -DBANTU_MYSQL=ON \
         -DBANTU_WEBRTC=ON \
         -DCMAKE_PREFIX_PATH="$(brew --prefix libpq);$(brew --prefix libdatachannel)"
cmake --build . --config Release -j
sudo cp bantu /usr/local/bin/bantu
```

**Common macOS install failures and fixes:**

| Symptom | Cause | Fix |
|---|---|---|
| `xcode-select: error: command line tools are already installed` then build fails | Stale Xcode CLT | `sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install` |
| `'iostream' file not found` | Xcode CLT missing or broken | `xcode-select --install` (allow the ~200 MB install to finish) |
| `clang: error: no such file or directory: 'build.sh'` | Wrong directory | `cd Bantu/bantu-src/compiler` (build.sh lives there) |
| `bantu: command not found` after install | PATH not refreshed | Open a **new** Terminal tab, or `source ~/.zshrc` |
| `bantu installer ... --platform macos` warns "Could not locate bantu binary to bundle" | Older bantu binary using PATH-lookup fallback | Rebuild from this commit or later — `_NSGetExecutablePath` is now used |
| `permission denied: /usr/local/bin/bantu` | Owned by root | `sudo chown $(whoami) /usr/local/bin/bantu` (or reinstall with `sudo`) |
| dyld library load errors at runtime | Built with shared libpq / libdatachannel but those aren't on DYLD_LIBRARY_PATH | Either rebuild without `-DBANTU_POSTGRES=ON` or `export DYLD_LIBRARY_PATH="$(brew --prefix libpq)/lib"` |

### Building a macOS `.app` for your own Bantu app

Once Bantu itself is installed, you can package any `.b` program as a
standard macOS `.app` bundle:

```bash
cd /path/to/your-bantu-app
bantu installer server.b --platform macos --name "MyApp" --version "1.0.0"
# → dist/MyApp.app

open dist/MyApp.app       # runs it
# Distribute as a .dmg:
hdiutil create -volname 'MyApp' -srcfolder dist/MyApp.app -ov -format UDZO dist/MyApp-1.0.0.dmg
```

The `.app` bundles the `bantu` interpreter at `Contents/MacOS/bantu`
(via `_NSGetExecutablePath` — so the bundle is fully self-contained and
runs on any macOS 10.13+ machine, no Bantu install required on the
target).

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
code --install-extension bantu-vscode-1.2.2.vsix
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

## Modular App (v1.2.2 `include`)

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
    $res.json({ "ok": true, "version": "1.2.2" });
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

## WebRTC (v1.2.2)

```bantu
$peer  = sua.webrtc.peer("alice");
$offer = sua.webrtc.createOffer("alice");
$chan  = sua.webrtc.dataChannel("chat");
sua.webrtc.send("chat", "hello, world!");
```

## Build Desktop Installers (v1.2.2 NEW — cross-platform, incl. Android)

The `bantu installer` command packages any Bantu project into a single
self-contained desktop installer — **the installer bundles the Bantu
interpreter itself**, so end users can run your app on machines that
don't have Bantu installed. Ship the installer to a friend, they
double-click it, the app appears in their launcher. No internet required.

```bash
bantu installer                            # auto-detect host platform
bantu installer app.b --platform linux     # → dist/<name>_<ver>_amd64.deb
bantu installer app.b --platform windows   # → dist/<name>-Setup-<ver>.exe
bantu installer app.b --platform macos     # → dist/<Name>.app
bantu installer app.b --platform android   # → dist/android/<name>/ (Android Studio project + APK)
```

| Platform | Output | What it does |
|---|---|---|
| **Linux**   | `.deb` + `.desktop` entry | Bundles `bantu` at `/usr/lib/<name>/`, drops launcher at `/usr/bin/<name>`, adds app launcher icon |
| **Windows** | NSIS `.exe` installer     | Installs to `%LOCALAPPDATA%\<name>`, adds Start Menu shortcut, registers uninstaller in *Add/Remove Programs* |
| **macOS**   | `.app` bundle             | Standard `Contents/{MacOS,Resources}` layout with `Info.plist` — wrap into `.dmg` with `hdiutil` |
| **Android** | Android Studio project    | Full Gradle project under `dist/android/<name>/` — bundles `.b` sources in `assets/`, optional arm64 `bantu` in `jniLibs/`, builds a debug APK via `./build-apk.sh`. App runs fully offline in a WebView pointed at the bundled Bantu HTTP server. |

### Android specifics

The Android target generates a complete, ready-to-build Android Studio
project. End users sideload the resulting `app-debug.apk` on any Android
7.0+ phone — **no Bantu installed on the device required**.

The project bundles:

- your `.b` source files + `bantu.json` under `app/src/main/assets/bantu/`
- a pre-built `arm64-v8a/libbantu.so` under `app/src/main/jniLibs/`
  (if you provide one — see `BUILD-ANDROID.md` for NDK cross-compile
  instructions; the installer looks in `./android/libbantu.so`,
  `~/.bantu/android/arm64-v8a/libbantu.so`, or `$BANTU_ANDROID_ARM64`)
- a `MainActivity.kt` that launches `./bantu run <entry>.b` as a
  subprocess and renders the Sua web app in a WebView
- a `build-apk.sh` wrapper that runs `gradle assembleDebug`

```bash
# Generate the Android project from your Bantu web app
bantu installer app.b --platform android

# Build the APK (one-time: install Android SDK + NDK + cross-compile bantu for arm64)
cd dist/android/MyApp
./build-apk.sh

# Install on a connected phone
adb install -r app-debug.apk
```

Options: `--name`, `--version`, `--icon`, `--bundle-bantu` / `--no-bundle-bantu`.
Defaults are read from `bantu.json` if present. See **Chapter 9** of the
official guide PDF for the full walkthrough.

### Legacy: Build Windows Installer (v1.2.1)

The older `bantu build-windows` command (Windows-only NSIS installer) is
still available for backwards compatibility:

```bash
# Generates dist/MyApp-Setup-1.0.0.exe (NSIS installer, Windows-only)
bantu build-windows --name "MyApp" --version "1.0.0"
```

For new projects, prefer `bantu installer --platform windows`.

## Build from Source (optional — only if you want to hack on Bantu itself)

You only need this if you're contributing to Bantu or want a custom build
with the optional real PostgreSQL / MySQL / WebRTC drivers. **For normal
use, just download the pre-built release zip** (Linux or Windows) — see
the [Download](#download) section above.

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
bantu --version    # → Bantu v1.2.2
```

## Repository Layout

```
Bantu/
├── bantu-src/compiler/      # C++17 interpreter (lexer, parser, evaluator, server, package_manager)
│   ├── src/
│   │   ├── main.cpp         # CLI entry point (run/build/init/setup/installer/build-windows/bench/...)
│   │   ├── lexer.hpp        # Tokenizer + keywords
│   │   ├── parser.hpp       # Recursive-descent parser
│   │   ├── evaluator.hpp    # Tree-walking evaluator (3.4k lines, includes sua.* framework)
│   │   ├── module_resolver.hpp  # [v1.2.2] include path resolution
│   │   └── ...
│   └── CMakeLists.txt       # Build config (with -DBANTU_POSTGRES/MYSQL/WEBRTC flags)
├── drivers/                 # [v1.2.2] Optional real-driver glue
│   ├── postgres_driver.hpp  # libpq wrapper (HAS_LIBPQ)
│   ├── mysql_driver.hpp     # mysqlclient wrapper (HAS_MYSQL)
│   └── webrtc_engine.hpp    # libdatachannel wrapper (HAS_RTC)
├── vscode-extension/        # [v1.2.2] VSCode extension
│   ├── package.json
│   ├── syntaxes/bantu.tmLanguage.json
│   ├── snippets/bantu.json
│   ├── language/bantu-language-configuration.json
│   ├── icons/               # blue-B file icon (light + dark)
│   ├── src/                 # TypeScript sources (extension, completion, hover, symbol, task)
│   └── README.md
├── windows-installer/       # [v1.2.2] NSIS template reference
├── samples/                 # [v1.2.2] Real apps
│   ├── blogsite/            # Modular Sua + SQLite blog (uses include keyword)
│   ├── webrtc-chat/         # WebRTC signaling + browser UI
│   └── pg-dashboard/        # PostgreSQL analytics dashboard
├── docs/
│   └── Bantu-Programming-Language-v1.2.2.pdf  # 36-page official guide (Dracula theme)
├── windows/                 # Windows .bat helpers (start, stop, reset-db)
├── public/                  # Sua default static files
├── README.md                # This file
├── CHANGELOG.md             # v1.0.0 → v1.1.0 → v1.2.0 → v1.2.2
├── LICENSE                  # MIT
└── QUICKSTART.md
```

## Documentation

- **Official guide:** [`docs/Bantu-Programming-Language-v1.2.2.pdf`](docs/Bantu-Programming-Language-v1.2.2.pdf) — 36 pages, Dracula-themed, covers every feature including `bantu installer`
- **Quick start:** [`QUICKSTART.md`](QUICKSTART.md)
- **Samples:** [`samples/`](samples/) — three runnable apps
- **VSCode extension:** [`vscode-extension/README.md`](vscode-extension/README.md)
- **Changelog:** [`CHANGELOG.md`](CHANGELOG.md)

## Community

- **Source:** https://github.com/AsseySilivestir/Bantu
- **Issues:** https://github.com/AsseySilivestir/Bantu/issues
- **Original fork:** https://github.com/AsseySilivestir/bantusua-local

## License

MIT — see [LICENSE](LICENSE).

## Attribution

Bantu was created by **Assey Silivestir Peter**. The language is named after the Bantu language family spoken across sub-Saharan Africa — a nod to the developer's Tanzanian roots and the language's goal of being accessible to developers in low-bandwidth, offline-first environments.

---

*v1.2.2 stable release · June 2026*
