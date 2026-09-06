#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
#  build-mac.sh — Build the Bantu interpreter binary (macOS)
# ─────────────────────────────────────────────────────────────────────

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ═══════════════════════════════════════════════════════════════════════
#  Macros
# ═══════════════════════════════════════════════════════════════════════

section()  { echo; echo "── $* ──"; }
hr()       { echo "════════════════════════════════════════════════════════════════"; }
pass()     { echo "[PASS] $*"; }
fail()     { echo "[FAIL] $*"; }

die() {
    fail "$1"
    shift
    [ $# -gt 0 ] && printf '       %s\n' "$@"
    exit 1
}

compile_one() {
    local src="$1" obj="$2"; shift 2
    section "Compiling $src"
    if g++ "${CPP_FLAGS[@]}" -Wall -c "$src" -o "$obj" "$@" 2>&1; then
        pass "$src -> $obj ($(wc -c <"$obj") bytes)"
        return 0
    else
        die "Compilation failed for: $src"
    fi
}

check_header() {
    local hdr="$1"
    if printf '#include <%s>\nint main(){return 0;}\n' "$hdr" \
        | g++ -std=c++17 -x c++ - -o /tmp/hdrtest 2>/dev/null; then
        pass "<$hdr> found"
    else
        rm -f /tmp/hdrtest
        die "<$hdr> not found" "Run: brew install sqlite curl"
    fi
}

# ═══════════════════════════════════════════════════════════════════════
#  Configuration — macOS
# ═══════════════════════════════════════════════════════════════════════

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

# Detect Homebrew prefix
if [ -d /opt/homebrew ]; then
    BREW_PREFIX="/opt/homebrew"
else
    BREW_PREFIX="/usr/local"
fi

CPP_FLAGS=(
    -std=c++17
    -O2
    -I src
    -I"$BREW_PREFIX/include"
    -DBANTU_FFI          # enable the FFI builtins (loadlib/func)
)

LINK_LIBS=(
    -L"$BREW_PREFIX/lib"
    -lsqlite3
    -lcurl
    -lffi                # libffi — foreign function interface
    -ldl                 # dlopen/dlsym
)

# ═══════════════════════════════════════════════════════════════════════
#  Build
# ═══════════════════════════════════════════════════════════════════════

hr
echo "  Bantu interpreter build (macOS)"
echo "  script dir: $SCRIPT_DIR"
echo "  brew prefix: $BREW_PREFIX"
echo "  g++ version: $(g++ --version  | head -1)"
hr

section "Checking build tools"
MISSING=()
for tool in g++ gcc; do
    command -v "$tool" >/dev/null 2>&1 || MISSING+=("$tool")
done
[ ${#MISSING[@]} -eq 0 ] || die "Missing tools: ${MISSING[*]} — Run: brew install gcc"
pass "All required build tools present."

section "Checking headers"
for hdr in sqlite3.h curl/curl.h; do
    check_header "$hdr"
done
rm -f /tmp/hdrtest

section "Cleaning previous build"
rm -rf build; mkdir -p build

OBJECTS=()
for src in "${SOURCES[@]}"; do
    obj="build/$(basename "${src%.cpp}").o"
    compile_one "$src" "$obj"
    OBJECTS+=( "$obj" )
done

section "Linking build/bantu"
if g++ "${CPP_FLAGS[@]}" \
        "${OBJECTS[@]}" \
        -o build/bantu \
        "${LINK_LIBS[@]}" 2>&1; then
    pass "Linked build/bantu ($(wc -c <build/bantu) bytes)"
else
    die "Link failed" \
        "Objects: ${OBJECTS[*]}" \
        "Libs:    ${LINK_LIBS[*]}"
fi

section "Verifying binary"
[ -x build/bantu ] || die "build/bantu is not executable"
pass "build/bantu is executable"

section "otool -L build/bantu"
otool -L build/bantu 2>&1 || true

echo
hr
echo "  ✓ Build complete: $(pwd)/build/bantu"
hr
