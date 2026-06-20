#!/usr/bin/env bash
# benchmarks/run.sh — convenience wrapper for the Bantu benchmark suite.
# Runs both the built-in C++ benchmarks (`bantu bench`) and the pure-Bantu
# benchmarks (`bantu run bench.b`) and concatenates the output into
# benchmarks/results.txt.
set -e
cd "$(dirname "$0")/.."

OUT="benchmarks/results.txt"
{
    echo "=== Bantu v1.2.2 Benchmark Results ==="
    echo "Date:     $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "Host:     $(uname -srm)"
    echo "CPU:      $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs || echo 'n/a')"
    echo ""
    echo "--- bantu bench (built-in) ---"
    bantu bench
    echo ""
    echo "--- bantu run benchmarks/bench.b (pure Bantu) ---"
    bantu run benchmarks/bench.b
} | tee "$OUT"

echo ""
echo "Results saved to $OUT"
