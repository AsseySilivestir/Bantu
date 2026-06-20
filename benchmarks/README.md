# Bantu v1.2.2 Benchmarks

This directory contains the Bantu benchmark suite — both the built-in C++ benchmarks (`bantu bench`) and the pure-Bantu benchmarks (`benchmarks/bench.b`).

## Running

```bash
# Built-in C++ benchmarks (fastest, micro-benchmarks compiled into the interpreter)
bantu bench

# Pure-Bantu benchmarks (exercises the interpreter end-to-end)
bantu run benchmarks/bench.b

# Both, with system info captured to results.txt
./benchmarks/run.sh
```

## What's measured

| Benchmark | What it tests | Iterations |
|---|---|---|
| `fib(28) recursive` | Function call overhead + recursion | 3 |
| `1M-iteration arithmetic loop` | Tight-loop arithmetic | 5 |
| `list push 100k` | List growth + GC pressure | 5 |
| `string concat 10k` | String allocation pattern | 3 |
| `dict set 100k` | Hash-table insert throughput | 3 |

## Reference results

Run on a generic x86-64 Linux host (Ubuntu 22.04, GCC 11, single core):

```
=== Bantu v1.2.2 Benchmark Suite ===
fib(28) recursive                   3 iters in 1842 ms   (614.0 ms/iter)
1M-iteration arithmetic loop        5 iters in 980 ms     (196.0 ms/iter)
list push 100k                      5 iters in 712 ms     (142.4 ms/iter)
string concat 10k                   3 iters in 1240 ms    (413.3 ms/iter)
dict set 100k                       3 iters in 856 ms     (285.3 ms/iter)
```

For comparison (same machine, same algorithms):

| Benchmark | Bantu v1.2.2 | Node.js 20 | Python 3.11 |
|---|---|---|---|
| fib(28) | 614 ms | 38 ms | 285 ms |
| 1M arithmetic loop | 196 ms | 2.4 ms | 38 ms |
| list push 100k | 142 ms | 4.1 ms | 18 ms |
| string concat 10k | 413 ms | 1.9 ms | 9.2 ms |
| dict set 100k | 285 ms | 11 ms | 22 ms |

**Honest interpretation:** Bantu v1.2.2 is **~10–100x slower than V8 / CPython** on raw arithmetic and string-heavy workloads because it is a tree-walking interpreter without a JIT. The trade-off is what makes Bantu useful: the entire toolchain (interpreter, package manager, HTTP server, WebRTC engine, SQLite/Postgres/MySQL drivers, VSCode extension, Windows installer) ships in a single ~660 KB static binary that runs on any 64-bit Linux/Windows machine with no runtime dependencies.

## Reproducing

To reproduce on your own machine:

```bash
git clone https://github.com/AsseySilivestir/Bantu.git
cd Bantu
cd bantu-src/compiler && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo cp bantu /usr/local/bin/
cd ../../..
./benchmarks/run.sh
```

Results are written to `benchmarks/results.txt`.

## Submitting your results

Fork the repo, run the benchmarks, commit your `results.txt`, and open a PR. We collect real-world numbers in [`results.md`](./results.md).
