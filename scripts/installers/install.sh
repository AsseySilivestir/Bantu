#!/bin/sh
# ============================================================================
#  install.sh — standalone installer for the Bantu toolchain (Linux & macOS)
#
#  One-line install:
#      curl -fsSL https://raw.githubusercontent.com/AsseySilivestir/Bantu/main/scripts/installers/install.sh | sh
#
#  It detects your OS/arch, downloads the matching release binary, installs it
#  to a bin directory on PATH, and verifies `bantu --version`. No admin needed
#  when installing to ~/.local/bin.
#
#  Environment overrides:
#      BANTU_VERSION      release tag to install (default: latest)
#      BANTU_INSTALL_DIR  install directory (default: /usr/local/bin if
#                         writable, else ~/.local/bin)
#      BANTU_LOCAL_BIN    path to an already-built `bantu` binary to install
#                         instead of downloading (offline / CI / testing)
#      BANTU_REPO         GitHub owner/repo (default: AsseySilivestir/Bantu)
# ============================================================================
set -eu

REPO="${BANTU_REPO:-AsseySilivestir/Bantu}"
RESET='\033[0m'; BOLD='\033[1m'; GREEN='\033[32m'; RED='\033[31m'; DIM='\033[2m'

say()  { printf "%b\n" "$1"; }
info() { say "  $1"; }
ok()   { say "  ${GREEN}✓${RESET} $1"; }
die()  { say "  ${RED}error:${RESET} $1"; exit 1; }

# HTTP helpers — use curl if present, else wget.
_fetch() {    # _fetch URL  → stdout
    if command -v curl >/dev/null 2>&1; then curl -fsSL "$1"
    elif command -v wget >/dev/null 2>&1; then wget -qO- "$1"
    else die "need curl or wget installed"; fi
}
_download() { # _download URL OUTFILE
    if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then wget -q "$1" -O "$2"
    else die "need curl or wget installed"; fi
}

say ""
say "  ${BOLD}Bantu toolchain installer${RESET}"
say "  ${DIM}${REPO}${RESET}"
say ""

# ── Detect platform ────────────────────────────────────────────────────────
os_raw="$(uname -s)"
arch_raw="$(uname -m)"

case "$os_raw" in
    Linux)  OS="linux" ;;
    Darwin) OS="macos" ;;
    *)      die "unsupported OS: $os_raw (use install.ps1 on Windows)" ;;
esac

case "$arch_raw" in
    x86_64|amd64)   ARCH="x64" ;;
    arm64|aarch64)  ARCH="arm64" ;;
    *)              die "unsupported architecture: $arch_raw" ;;
esac
info "platform: ${BOLD}${OS}-${ARCH}${RESET}"

# ── Choose install dir ─────────────────────────────────────────────────────
if [ -n "${BANTU_INSTALL_DIR:-}" ]; then
    INSTALL_DIR="$BANTU_INSTALL_DIR"
elif [ -w "/usr/local/bin" ] 2>/dev/null; then
    INSTALL_DIR="/usr/local/bin"
else
    INSTALL_DIR="$HOME/.local/bin"
fi
mkdir -p "$INSTALL_DIR" || die "cannot create $INSTALL_DIR"
info "install dir: ${BOLD}${INSTALL_DIR}${RESET}"

TARGET="$INSTALL_DIR/bantu"

# ── Acquire the binary ─────────────────────────────────────────────────────
if [ -n "${BANTU_LOCAL_BIN:-}" ]; then
    # Offline / testing path: install a binary that already exists locally.
    [ -f "$BANTU_LOCAL_BIN" ] || die "BANTU_LOCAL_BIN not found: $BANTU_LOCAL_BIN"
    info "installing local binary: $BANTU_LOCAL_BIN"
    cp "$BANTU_LOCAL_BIN" "$TARGET"
else
    # Resolve version.
    VERSION="${BANTU_VERSION:-latest}"
    if [ "$VERSION" = "latest" ]; then
        info "resolving latest release…"
        API="https://api.github.com/repos/${REPO}/releases/latest"
        VERSION="$(_fetch "$API" 2>/dev/null | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -n1)"
        [ -n "$VERSION" ] || die "could not resolve latest version (set BANTU_VERSION)"
    fi
    info "version: ${BOLD}${VERSION}${RESET}"

    ASSET="bantu-${OS}-${ARCH}.tar.gz"
    URL="https://github.com/${REPO}/releases/download/${VERSION}/${ASSET}"
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT

    info "downloading ${ASSET}…"
    _download "$URL" "$TMP/$ASSET" || die "download failed: $URL"

    info "extracting…"
    tar -xzf "$TMP/$ASSET" -C "$TMP" || die "extraction failed"

    # Find the bantu binary inside the archive.
    BIN="$(find "$TMP" -type f -name bantu | head -n1)"
    [ -n "$BIN" ] || die "archive did not contain a 'bantu' binary"
    cp "$BIN" "$TARGET"
fi

chmod +x "$TARGET" || die "cannot mark $TARGET executable"
ok "installed ${BOLD}${TARGET}${RESET}"

# ── PATH hint ──────────────────────────────────────────────────────────────
case ":$PATH:" in
    *":$INSTALL_DIR:"*) : ;;
    *)
        say ""
        info "${BOLD}${INSTALL_DIR}${RESET} is not on your PATH. Add it:"
        RC="$HOME/.profile"
        [ -n "${ZSH_VERSION:-}" ] && RC="$HOME/.zshrc"
        [ -n "${BASH_VERSION:-}" ] && RC="$HOME/.bashrc"
        say "      echo 'export PATH=\"$INSTALL_DIR:\$PATH\"' >> $RC"
        say "      export PATH=\"$INSTALL_DIR:\$PATH\""
        ;;
esac

# ── Verify ─────────────────────────────────────────────────────────────────
say ""
if "$TARGET" --version >/dev/null 2>&1; then
    ok "$("$TARGET" --version) ready"
    say ""
    say "  Get started:  ${BOLD}bantu run yourfile.b${RESET}"
    say ""
else
    die "installed but '$TARGET --version' failed to run"
fi
