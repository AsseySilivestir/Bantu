// ============================================================================
// bench.b — Bantu v1.2.2 Benchmark Suite (pure Bantu version)
//
// Run with:   bantu run bench.b
// Or use:     bantu bench      (built-in C++ version, slightly faster)
//
// Each benchmark reports ops/sec and total wall time. The results are
// also collected into a $results dict that the harness prints at the end.
// ============================================================================

print("=== Bantu v1.2.2 Benchmark Suite (pure-Bantu) ===");
print("");

$results = [];

def bench($name, $iters, $body) {
    $t0 = sua.clock();
    $i = 0;
    while ($i < $iters) {
        $body();
        $i += 1;
    }
    $t1 = sua.clock();
    $elapsedMs = ($t1 - $t0) * 1000.0;
    $perOpUs = ($elapsedMs * 1000.0) / $iters;
    $opsPerSec = $iters / ($elapsedMs / 1000.0);

    print($name + "  " + $iters + " iters in " + $elapsedMs + " ms  ("
          + $perOpUs + " us/op, " + $opsPerSec + " ops/s)");

    $results.push({
        "name": $name,
        "iters": $iters,
        "elapsedMs": $elapsedMs,
        "perOpUs": $perOpUs,
        "opsPerSec": $opsPerSec
    });
}

// ─── 1. Recursive Fibonacci (warm-up + recursion benchmark) ──────
def fib($n) {
    if ($n < 2) { return $n; }
    return fib($n - 1) + fib($n - 2);
}
bench("fib(28) recursive", 3, def() { fib(28); });

// ─── 2. Tight arithmetic loop ────────────────────────────────────
bench("1M-iteration arithmetic loop", 5, def() {
    $sum = 0;
    $i = 0;
    while ($i < 1000000) {
        $sum += $i;
        $i += 1;
    }
});

// ─── 3. List push ─────────────────────────────────────────────────
bench("list push 100k", 5, def() {
    $l = [];
    $i = 0;
    while ($i < 100000) {
        $l.push($i);
        $i += 1;
    }
});

// ─── 4. String concatenation ─────────────────────────────────────
bench("string concat 10k", 3, def() {
    $s = "";
    $i = 0;
    while ($i < 10000) {
        $s = $s + "x";
        $i += 1;
    }
});

// ─── 5. Dict set/get ─────────────────────────────────────────────
bench("dict set 100k", 3, def() {
    $d = {};
    $i = 0;
    while ($i < 100000) {
        $d["k" + $i] = $i;
        $i += 1;
    }
});

// ─── 6. HTTP echo (local) — only if server is running ────────────
// bench("http GET /api/health", 50, def() {
//     $r = sua.http.get("http://localhost:3000/api/health");
// });

// ─── Summary ──────────────────────────────────────────────────────
print("");
print("=== Summary ===");
print("Bantu v1.2.2");
each ($r in $results) {
    print("  " + $r.name + ": " + $r.opsPerSec + " ops/s");
}
print("");
print("Done. " + $results.size() + " benchmarks completed.");
