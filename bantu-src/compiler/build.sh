#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
#  build.sh — Build the Bantu interpreter binary (Ubuntu 22.04 compatible)
#
#  Design:
#    - Compiles each .cpp separately (smaller per-TU memory footprint,
#      clearer error messages, survives one-file failures).
#    - Does NOT use `set -e` globally; every step is checked explicitly
#      so that diagnostic commands (objdump/grep) cannot abort the build.
#    - Prints clear [PASS]/[FAIL] markers so Render logs are readable.
#
#  Max runtime requirements (verified locally):
#    GLIBC_2.34, GLIBCXX_3.4.9  →  all ≤ Ubuntu 22.04's runtime
# ─────────────────────────────────────────────────────────────────────

set -u   # treat unset vars as error, but DO NOT use -e/-o pipefail globally

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "════════════════════════════════════════════════════════════════"
echo "  Bantu interpreter build"
echo "  script dir: $SCRIPT_DIR"
echo "  g++ version: $(g++ --version | head -1)"
echo "  gcc version: $(gcc --version | head -1)"
echo "════════════════════════════════════════════════════════════════"

# ─── Pre-flight: required tools ───────────────────────────────────
MISSING=()
for tool in g++ gcc ar ld; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        MISSING+=("$tool")
    fi
done
if [ ${#MISSING[@]} -gt 0 ]; then
    echo "[FAIL] Missing required tools: ${MISSING[*]}"
    exit 1
fi
echo "[PASS] All required build tools present."

# ─── Pre-flight: required headers ─────────────────────────────────
echo "── Checking headers ──"
for hdr in sqlite3.h curl/curl.h; do
    if printf '#include <%s>\nint main(){return 0;}\n' "$hdr" \
        | g++ -std=c++17 -x c++ - -o /tmp/hdrtest 2>/dev/null; then
        echo "[PASS] <$hdr> found"
    else
        echo "[FAIL] <$hdr> not found — install libsqlite3-dev and libcurl4-openssl-dev"
        rm -f /tmp/hdrtest
        exit 1
    fi
done
rm -f /tmp/hdrtest

# ─── Clean ────────────────────────────────────────────────────────
echo "── Cleaning previous build ──"
rm -rf build
mkdir -p build

# ─── Compile ios_base_library_initv stub ──────────────────────────
echo "── Compiling stub: stubs/ios_base_library_initv.c ──"
if gcc -O2 -c stubs/ios_base_library_initv.c -o build/ios_stub.o 2>&1; then
    echo "[PASS] stub compiled"
else
    echo "[FAIL] stub compilation failed"
    exit 1
fi

# ─── Compile each .cpp separately ─────────────────────────────────
# Order matters: smaller files first to fail fast on simple errors.
SOURCES=(
    src/lexer.cpp
    src/parser.cpp
    src/ast.cpp
    src/types.cpp
    src/function.cpp
    src/class.cpp
    src/evaluator.cpp
    src/main.cpp
)

CPP_FLAGS=(
    -std=c++17
    -O2
    -mtune=generic
    -fno-plt
    -pthread
    -I src
    -DBANTU_FFI          # enable the FFI builtins (loadlib/func)
)

# Use -l:libcurl.so.4 (explicit filename) instead of -lcurl.
# Why: on Debian 13 the default `libcurl.so` symlink points to the GnuTLS
# flavor (`libcurl-gnutls.so.4`), which would make our binary depend on
# `libcurl-gnutls.so.4` at runtime — a package that doesn't exist on
# Ubuntu 22.04 (`libcurl4` there only provides `libcurl.so.4`, OpenSSL flavor).
# Forcing the exact soname `libcurl.so.4` makes the binary depend on the
# library that IS universally shipped as `libcurl4` on both Debian and
# Ubuntu, matching what the Dockerfile installs at runtime.
LINK_LIBS=( -lsqlite3 -l:libcurl.so.4 -lffi -ldl -lpthread )

OBJECTS=()
for src in "${SOURCES[@]}"; do
    obj="build/$(basename "${src%.cpp}").o"
    echo
    echo "── Compiling $src ──"
    # Use -Wall but NOT -Werror — we want warnings, not failures.
    if g++ "${CPP_FLAGS[@]}" -Wall -c "$src" -o "$obj" 2>&1; then
        echo "[PASS] $src -> $obj ($(wc -c <"$obj") bytes)"
        OBJECTS+=( "$obj" )
    else
        echo "[FAIL] Compilation failed for: $src"
        echo "       Flags: ${CPP_FLAGS[*]}"
        exit 1
    fi
done

# ─── Link ─────────────────────────────────────────────────────────
echo
echo "── Linking build/bantu ──"
if g++ "${CPP_FLAGS[@]}" \
        "${OBJECTS[@]}" \
        build/ios_stub.o \
        -o build/bantu \
        "${LINK_LIBS[@]}" 2>&1; then
    echo "[PASS] Linked build/bantu ($(wc -c <build/bantu) bytes)"
else
    echo "[FAIL] Link failed"
    echo "       Objects: ${OBJECTS[*]}"
    echo "       Libs:    ${LINK_LIBS[*]}"
    exit 1
fi

# ─── Verify the binary is executable ──────────────────────────────
echo
echo "── Verifying binary ──"
if [ ! -x build/bantu ]; then
    echo "[FAIL] build/bantu is not executable"
    exit 1
fi
echo "[PASS] build/bantu is executable"

# ─── Print ldd output (informational only) ────────────────────────
echo
echo "── ldd build/bantu ──"
ldd build/bantu 2>&1 || echo "(ldd not available or returned non-zero)"

# ─── Print GLIBC/GLIBCXX requirements (informational only) ────────
echo
echo "── Dynamic symbol requirements (informational only) ──"
if command -v objdump >/dev/null 2>&1; then
    echo "  GLIBC versions referenced:"
    objdump -T build/bantu 2>/dev/null \
        | grep -oE 'GLIBC_[0-9]+\.[0-9]+' \
        | sort -u | sed 's/^/    /' || true
    echo "  GLIBCXX versions referenced:"
    objdump -T build/bantu 2>/dev/null \
        | grep -oE 'GLIBCXX_[0-9]+\.[0-9]+\.[0-9]+' \
        | sort -u | sed 's/^/    /' || true
else
    echo "  (objdump not available — skipping symbol diagnostics)"
fi

echo
echo "════════════════════════════════════════════════════════════════"
echo "  ✓ Build complete: $(pwd)/build/bantu"
echo "════════════════════════════════════════════════════════════════"
