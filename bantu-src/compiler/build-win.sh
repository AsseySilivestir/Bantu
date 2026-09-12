#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
#  build-win.sh — Cross-compile the Bantu interpreter to a Windows .exe
#
#  Output:  build/bantu.exe  (64-bit Windows executable)
#
#  Toolchain used:
#    llvm-mingw (clang-based MinGW-w64) — handles both Linux and Windows
#    source code paths in the Bantu interpreter.
#
#  Runtime dependencies (must be shipped next to bantu.exe):
#    - sqlite3.dll          (SQLite runtime)
#    - libcurl-x64.dll      (libcurl runtime + its bundled deps)
#    - libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll
#      (C++ runtime — shipped by llvm-mingw)
#
#  The script will print a list of every DLL the produced .exe needs at
#  the end, so you can copy them all into the dist folder.
# ─────────────────────────────────────────────────────────────────────

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ─── Locate llvm-mingw toolchain ────────────────────────────────────
MINGW_PREFIX="${MINGW_PREFIX:-/tmp/my-project/scripts/build-win/llvm-mingw-20230614-ucrt-ubuntu-20.04-x86_64}"
if [[ ! -x "$MINGW_PREFIX/bin/x86_64-w64-mingw32-clang++" ]]; then
    # Try system-installed mingw-w64
    if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
        CXX=x86_64-w64-mingw32-g++
        DLLTOOL=x86_64-w64-mingw32-dlltool
        OBJDUMP=x86_64-w64-mingw32-objdump
        EXTRA_INC=()
        EXTRA_LIB=()
        USE_LLVM=0
    else
        echo "✗ llvm-mingw not found at: $MINGW_PREFIX"
        echo "  Install:  sudo apt-get install -y mingw-w64"
        echo "  Or set MINGW_PREFIX to point at an llvm-mingw tree."
        exit 1
    fi
else
    CXX="$MINGW_PREFIX/bin/x86_64-w64-mingw32-clang++"
    DLLTOOL="$MINGW_PREFIX/bin/x86_64-w64-mingw32-dlltool"
    OBJDUMP="$MINGW_PREFIX/bin/x86_64-w64-mingw32-objdump"
    # llvm-mingw requires explicit header search paths because the
    # .cfg file that should set them up isn't shipped in this build.
    # Order matters: libc++ headers first, then win32 (mingw) headers,
    # then clang resource dir.
    EXTRA_INC=(
        -isystem "$MINGW_PREFIX/generic-w64-mingw32/include/c++/v1"
        -isystem "$MINGW_PREFIX/generic-w64-mingw32/include"
        -isystem "$MINGW_PREFIX/lib/clang/16/include"
        -nostdinc -nostdinc++
    )
    EXTRA_LIB=(
        -L "$MINGW_PREFIX/x86_64-w64-mingw32/lib"
        -lc++ -lwinpthread
    )
    USE_LLVM=1
fi

# ─── Locate Windows deps (headers + DLLs + import libs) ─────────────
DEPS_DIR="${DEPS_DIR:-$SCRIPT_DIR/../../win-deps}"
SQLITE_INC="$DEPS_DIR/sqlite-amalgamation-3530200"
CURL_INC="$DEPS_DIR/include"
LIB_DIR="$DEPS_DIR/lib"

for f in "$SQLITE_INC/sqlite3.h" "$CURL_INC/curl/curl.h" \
         "$LIB_DIR/libsqlite3.dll.a" "$LIB_DIR/libcurl-x64.dll.a"; do
    if [[ ! -f "$f" ]]; then
        echo "✗ Missing Windows dep: $f"
        echo "  Run:  cd ../../win-deps && ./fetch-deps.sh"
        exit 1
    fi
done

# ─── Prep build dir ─────────────────────────────────────────────────
mkdir -p build
rm -f build/*.o build/bantu.exe

echo "════════════════════════════════════════════════════════════════"
echo "  Bantu interpreter — Windows cross-compile"
echo "  toolchain: $CXX"
echo "  deps dir:  $DEPS_DIR"
echo "════════════════════════════════════════════════════════════════"

# ─── Compile stubs (ios_base_library_initv shim) ────────────────────
echo
echo "── Compiling stubs/ios_base_library_initv.c ──"
if $CXX "${EXTRA_INC[@]}" -xc++ -O2 -c stubs/ios_base_library_initv.c -o build/ios_stub.o 2>&1; then
    echo "[PASS] ios_stub.o"
else
    echo "[FAIL] ios_stub.o"
    exit 1
fi

# ─── Compile each .cpp ──────────────────────────────────────────────
CPP_FLAGS=(
    -std=c++17
    -O2
    -mtune=generic
    -pthread
    "${EXTRA_INC[@]}"
    -I src
    -I "$SQLITE_INC"
    -I "$CURL_INC"
    -D_CRT_SECURE_NO_WARNINGS
)

# ── Optional: Apache Arrow (Parquet + Feather/IPC), OFF by default ────────
# Enable with:  BANTU_ARROW=1 ARROW_HOME=/path/to/arrow bash build-win.sh
# Kept opt-in so the default binary gains NO new dependency. Arrow 14+ headers
# need C++20 (std::span/popcount); the last -std wins. Untested on Windows here —
# provide a mingw-built Arrow via ARROW_HOME. The default build is unaffected.
ARROW_LINK=()
if [ "${BANTU_ARROW:-0}" = "1" ]; then
    if [ -z "${ARROW_HOME:-}" ] || [ ! -f "$ARROW_HOME/include/arrow/api.h" ]; then
        echo "[FAIL] BANTU_ARROW=1 but ARROW_HOME not set to an Arrow install (need include/arrow/api.h)"
        exit 1
    fi
    echo "  Apache Arrow: $ARROW_HOME (Parquet + Feather)"
    CPP_FLAGS+=( -std=c++20 -DBANTU_ARROW -I "$ARROW_HOME/include" )
    ARROW_LINK=( -L "$ARROW_HOME/lib" -larrow -lparquet )
fi

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

OBJECTS=()
for src in "${SOURCES[@]}"; do
    obj="build/$(basename "${src%.cpp}").o"
    echo
    echo "── Compiling $src ──"
    if $CXX "${CPP_FLAGS[@]}" -Wall -c "$src" -o "$obj" 2>&1; then
        echo "[PASS] $src -> $obj ($(wc -c <"$obj") bytes)"
        OBJECTS+=( "$obj" )
    else
        echo "[FAIL] Compilation failed for: $src"
        exit 1
    fi
done

# ─── Link ────────────────────────────────────────────────────────────
echo
echo "── Linking build/bantu.exe ──"
# -L "$LIB_DIR" finds the .dll.a import libs for sqlite3.dll and libcurl-x64.dll
# -lws2_32  → Windows Sockets (Winsock2), always static (system lib)
# -lbcrypt  → CNG (BCryptGenRandom), the Windows CSPRNG used by bantuCsprng
# -lc++ -lwinpthread → llvm-mingw C++ runtime + threading
if $CXX "${CPP_FLAGS[@]}" \
        "${OBJECTS[@]}" \
        build/ios_stub.o \
        -o build/bantu.exe \
        "${EXTRA_LIB[@]}" \
        "${ARROW_LINK[@]}" \
        -L "$LIB_DIR" \
        -l:libsqlite3.dll.a \
        -l:libcurl-x64.dll.a \
        -lws2_32 \
        -lbcrypt 2>&1; then
    echo "[PASS] Linked build/bantu.exe ($(wc -c <build/bantu.exe) bytes)"
else
    echo "[FAIL] Link failed"
    exit 1
fi

# ─── Verify the .exe ─────────────────────────────────────────────────
echo
echo "── file build/bantu.exe ──"
file build/bantu.exe 2>&1 || true

echo
echo "── DLLs the .exe depends on at runtime ──"
if command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1; then
    OBJDUMP=x86_64-w64-mingw32-objdump
elif [[ -x "$MINGW_PREFIX/bin/x86_64-w64-mingw32-objdump" ]]; then
    OBJDUMP="$MINGW_PREFIX/bin/x86_64-w64-mingw32-objdump"
elif command -v objdump >/dev/null 2>&1; then
    OBJDUMP=objdump
else
    OBJDUMP=""
fi
if [[ -n "$OBJDUMP" ]]; then
    $OBJDUMP -p build/bantu.exe 2>/dev/null | grep -i "DLL Name" | sed 's/^[[:space:]]*/  /'
fi

echo
echo "════════════════════════════════════════════════════════════════"
echo "  ✓ Build complete: $(pwd)/build/bantu.exe"
echo "════════════════════════════════════════════════════════════════"
