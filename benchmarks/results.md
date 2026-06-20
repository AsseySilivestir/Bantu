# Bantu Benchmark Results

This file collects real-world benchmark numbers from contributors.

To add your own: run `./benchmarks/run.sh`, copy the `=== Summary ===` section below, and open a PR.

## Reference (maintainer)

- **Date:** 2026-06-20
- **OS:** Ubuntu 22.04 LTS (x86-64)
- **CPU:** generic x86-64 (no `-march=native`, binary is portable)
- **Bantu version:** v1.2.2
- **Build:** `cmake .. -DCMAKE_BUILD_TYPE=Release`

```
fib(28) recursive                 3 iters  1842 ms   614.0 ms/iter
1M-iteration arithmetic loop      5 iters   980 ms   196.0 ms/iter
list push 100k                    5 iters   712 ms   142.4 ms/iter
string concat 10k                 3 iters  1240 ms   413.3 ms/iter
dict set 100k                     3 iters   856 ms   285.3 ms/iter
```

## Comparison table

| Benchmark | Bantu v1.2.2 | Node.js 20 | Python 3.11 | Lua 5.4 |
|---|---|---|---|---|
| fib(28) | 614 ms | 38 ms | 285 ms | 280 ms |
| 1M arithmetic loop | 196 ms | 2.4 ms | 38 ms | 9.6 ms |
| list push 100k | 142 ms | 4.1 ms | 18 ms | 8.1 ms |
| string concat 10k | 413 ms | 1.9 ms | 9.2 ms | 5.4 ms |
| dict set 100k | 285 ms | 11 ms | 22 ms | 14 ms |

## Interpretation

Bantu is **not designed to be the fastest scripting language**. It's designed to be:

1. **The smallest full-stack toolchain** — one ~660 KB binary contains the interpreter, package manager, HTTP server, WebSocket relay, STUN server, SQLite + PostgreSQL + MySQL drivers, WebRTC peer engine, project scaffolding, and Windows installer generator.
2. **The easiest to install** — no runtime dependencies, no system Python, no Node, no virtual env. Drop the binary on PATH and `bantu init` works.
3. **Reasonably fast for I/O-bound web work** — the Sua HTTP server uses native sockets (not the interpreter) so request throughput is competitive with Node's `http` module on the same hardware.

The interpreter is ~10–100x slower than V8 / CPython on tight arithmetic. For web request handlers (which are I/O-bound: database, HTTP, file reads), this difference is rarely the bottleneck — and Bantu's binary size and install simplicity win.

The v1.3 roadmap includes a register-based VM and bytecode compiler targeting ~5–10x speedup on these micro-benchmarks.

## Contributor results

<!-- Append your results below. Format:
### @yourhandle — Date — CPU
\`\`\`
<bench output>
\`\`\`
-->

(empty — be the first!)
