#!/usr/bin/env bash
# ============================================================================
#  build-macos-pkg.sh — build a standard macOS installer package (.pkg) for the
#  Bantu toolchain, the way python.org ships Python.
#
#  Double-clicking the resulting .pkg installs the `bantu` binary to
#  /usr/local/bin (on PATH by default), with a friendly welcome pane.
#
#  Usage:
#      scripts/installers/build-macos-pkg.sh <path-to-bantu-binary> [version]
#
#  Output:
#      dist/Bantu-<version>-macos-<arch>.pkg
#
#  Requires: pkgbuild + productbuild (ship with the Xcode command-line tools).
#  To sign/notarize for distribution, set:
#      BANTU_PKG_SIGN_ID="Developer ID Installer: Your Name (TEAMID)"
# ============================================================================
set -euo pipefail

BIN="${1:-}"
VERSION="${2:-1.2.2}"
IDENTIFIER="io.bantu.toolchain"

if [ -z "$BIN" ] || [ ! -f "$BIN" ]; then
    echo "usage: $0 <path-to-bantu-binary> [version]" >&2
    exit 1
fi

ARCH="$(uname -m)"
case "$ARCH" in
    arm64)  ARCH="arm64" ;;
    x86_64) ARCH="x64" ;;
esac

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT
DIST="$ROOT/dist"
mkdir -p "$DIST"

echo "── Staging payload ──"
# The payload tree mirrors the final install locations. COPYFILE_DISABLE keeps
# macOS from writing AppleDouble (._*) sidecar files into the package payload.
export COPYFILE_DISABLE=1
PAYLOAD="$BUILD/payload"
mkdir -p "$PAYLOAD/usr/local/bin"
cp "$BIN" "$PAYLOAD/usr/local/bin/bantu"
xattr -c "$PAYLOAD/usr/local/bin/bantu" 2>/dev/null || true
chmod 755 "$PAYLOAD/usr/local/bin/bantu"

echo "── Welcome text ──"
RES="$BUILD/resources"
mkdir -p "$RES"
cat > "$RES/welcome.txt" <<EOF
Bantu Programming Language v${VERSION}

This installer places the 'bantu' command in /usr/local/bin, which is already
on your PATH. After installation, open a new terminal and run:

    bantu --version
    bantu run yourfile.b

Bantu is a single, dependency-free binary. To uninstall, simply delete
/usr/local/bin/bantu.
EOF

echo "── pkgbuild (component) ──"
# Clear any extended attributes so the payload is free of AppleDouble sidecars.
xattr -rc "$PAYLOAD" 2>/dev/null || true
dot_clean -m "$PAYLOAD" 2>/dev/null || true
COMPONENT="$BUILD/bantu-component.pkg"
pkgbuild \
    --root "$PAYLOAD" \
    --identifier "$IDENTIFIER" \
    --version "$VERSION" \
    --install-location "/" \
    "$COMPONENT"

echo "── productbuild (distribution) ──"
DISTXML="$BUILD/distribution.xml"
cat > "$DISTXML" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Bantu ${VERSION}</title>
    <welcome file="welcome.txt"/>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <choices-outline>
        <line choice="default">
            <line choice="${IDENTIFIER}"/>
        </line>
    </choices-outline>
    <choice id="default"/>
    <choice id="${IDENTIFIER}" visible="false">
        <pkg-ref id="${IDENTIFIER}"/>
    </choice>
    <pkg-ref id="${IDENTIFIER}" version="${VERSION}" onConclusion="none">bantu-component.pkg</pkg-ref>
</installer-gui-script>
EOF

OUT="$DIST/Bantu-${VERSION}-macos-${ARCH}.pkg"

if [ -n "${BANTU_PKG_SIGN_ID:-}" ]; then
    echo "── signing with: $BANTU_PKG_SIGN_ID ──"
    productbuild \
        --distribution "$DISTXML" \
        --resources "$RES" \
        --package-path "$BUILD" \
        --sign "$BANTU_PKG_SIGN_ID" \
        "$OUT"
else
    productbuild \
        --distribution "$DISTXML" \
        --resources "$RES" \
        --package-path "$BUILD" \
        "$OUT"
fi

echo ""
echo "✓ Built: $OUT"
