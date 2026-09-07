# Bantu v1.2.2 — Windows Install (Offline)

This folder contains everything you need to install Bantu on Windows,
**without any internet connection**.

## One-time setup

1. Unzip the release.
2. If `bantu.exe` is **not** in this folder yet, build it from source:

   ```bat
   cd bantu-src\compiler
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --config Release
   copy build\Release\bantu.exe ..\..\
   cd ..\..\
   ```

   (You need Visual Studio 2022 with the **Desktop C++** workload, plus CMake.
   MinGW-w64 also works: replace the cmake lines with
   `cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release`.)

3. Run `setup.bat` (double-click or from a terminal):

   ```bat
   setup.bat --seed
   ```

   This adds the Bantu folder to your **user PATH** (persistent) and seeds
   the local package registry with starter packages.

4. Open a **new** terminal and verify:

   ```bat
   bantu --version
   rem → Bantu v1.2.2
   ```

5. Scaffold your first project:

   ```bat
   bantu init myproject
   cd myproject
   bantu run
   ```

## Files in this folder

| File | Purpose |
|---|---|
| `setup.bat` | One-time installer (adds to PATH, optional `--seed`) |
| `start.bat` | Start the bundled Sua sample server in background |
| `stop.bat`  | Stop the background server |
| `reset-db.bat` | Delete the SQLite DB (will reseed on next start) |
| `README.md` | This file |

## Diagnose

```bat
bantu doctor
```

Checks PATH, registry, and runs a tiny sample. Tells you what's wrong.

## Build a Windows installer for your own Bantu app

Once you've written a Bantu app, you can wrap it in an NSIS installer:

```bat
bantu build-windows --name MyBlog --version 1.0.0 server.b
```

This produces `dist/MyBlog-Setup-1.0.0.exe`. (Requires
[NSIS](https://nsis.sourceforge.io/) installed.)
