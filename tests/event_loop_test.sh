#!/usr/bin/env bash
# Compiles and runs the event-loop backend unit test against every backend the
# platform provides. See docs/sua-architecture.md §3.3.
set -u
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(mktemp -d)/event_loop_test"
trap 'rm -rf "$(dirname "$OUT")"' EXIT
if ! g++ -std=c++17 -O1 -Wall -I "$SRC/bantu-src/compiler/src" \
        "$SRC/tests/event_loop_test.cpp" -o "$OUT" 2>&1; then
    echo "  FAIL  event_loop_test.cpp did not compile"; exit 1
fi
"$OUT"
