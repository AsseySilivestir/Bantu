# Bantu toolchain installers

Standalone installers that install **Bantu itself** (the `bantu` binary) on Linux,
macOS and Windows with zero-overhead, Python-style DX. These are distinct from the
`bantu installer` CLI command, which packages *user apps* written in Bantu.

| File | Platform | What it does |
|---|---|---|
| [`install.sh`](install.sh) | Linux / macOS | `curl \| sh` installer: detects OS/arch, downloads the release binary, installs to a PATH dir, verifies `bantu --version`. |
| [`install.ps1`](install.ps1) | Windows | `irm \| iex` installer: downloads the release zip, installs to `%LOCALAPPDATA%\Bantu\bin`, updates user PATH. |
| [`build-macos-pkg.sh`](build-macos-pkg.sh) | macOS | Builds a standard `.pkg` (via `pkgbuild`/`productbuild`) that installs to `/usr/local/bin`. |
| [`bantu.wxs`](bantu.wxs) + [`build-windows-msi.ps1`](build-windows-msi.ps1) | Windows | WiX source + wrapper that builds an `.msi` installing to `Program Files\Bantu` and adding it to PATH. |

Release artifacts (`bantu-<os>-<arch>.tar.gz`, `.pkg`, `bantu-windows-x64.zip`, `.msi`)
are produced by [`.github/workflows/release.yml`](../../.github/workflows/release.yml) on
each `v*` tag, and are what the one-line scripts download.

## Usage

**End users** (from the README):

```sh
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/AsseySilivestir/Bantu/main/scripts/installers/install.sh | sh
```
```powershell
# Windows
irm https://raw.githubusercontent.com/AsseySilivestir/Bantu/main/scripts/installers/install.ps1 | iex
```

**Maintainers** — build installer packages locally from a freshly built binary:

```sh
# macOS .pkg
scripts/installers/build-macos-pkg.sh bantu-src/compiler/build/bantu 1.2.2
# → dist/Bantu-1.2.2-macos-<arch>.pkg
```
```powershell
# Windows .msi (requires the WiX .NET tool; the script installs it if missing)
scripts\installers\build-windows-msi.ps1 -Binary .\build\bantu.exe -Version 1.2.2
# → dist\Bantu-1.2.2-x64.msi
```

## Environment overrides

Both scripts accept overrides for offline / CI / testing:

| `install.sh` | `install.ps1` | meaning |
|---|---|---|
| `BANTU_VERSION` | `-Version` | release tag to install (default: latest) |
| `BANTU_INSTALL_DIR` | — | install directory |
| `BANTU_LOCAL_BIN` | `-LocalBin` | install an already-built binary instead of downloading |
| `BANTU_REPO` | `-Repo` | GitHub `owner/repo` (default `AsseySilivestir/Bantu`) |

```sh
# Test the installer against a locally built binary, no network:
BANTU_LOCAL_BIN=bantu-src/compiler/build/bantu BANTU_INSTALL_DIR=/tmp/bin sh scripts/installers/install.sh
```
