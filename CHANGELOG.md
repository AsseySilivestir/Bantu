# Changelog

All notable changes to the Bantu programming language are documented in this file. The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

> **Searchable tags:** every entry below is prefixed with `[feature]`, `[bug fix]`, or
> `[patch]` so you can grep the log, e.g. `grep '\[bug fix\]' CHANGELOG.md`.

## [Unreleased]

_Nothing yet._

---

## [1.4.0] — 2026-10-17

The **"Systems & Data" release**: a complete filesystem namespace, Python-style string
formatting, and the formal publication of the "Phase 6" body of work — a cycle collector,
refcounted class instances, a from-scratch raster backend that produces byte-identical PNGs
on Linux/macOS/Windows, `numba` (native n-d arrays), `arctic` (DataFrames), and `bplot`
(matplotlib-style charts). v1.4.0 promotes the long `## [Unreleased]` section below to a
numbered release and ships two new headline features on top of it.

### Added — NEW in v1.4.0

- **[feature] `sua.fs` — a complete filesystem namespace.** Twenty-six native builtins for
  path and directory work, complementing the v1.3.0 file I/O (`open`/`read`/`write`):

  ```bantu
  sua.fs.mkdir("/tmp/app/data")            // or mkdirp for recursive
  sua.fs.writefile("/tmp/app/data/cfg.json", "{...}")
  $files = sua.fs.readdir("/tmp/app/data") // list of names
  $info = sua.fs.stat("/tmp/app/data/cfg.json")
  print($info.type)                        // "file"
  print(str($info.size))                   // byte count
  sua.fs.copy_rec("./site", "./backup")    // recursive copy
  sua.fs.rmdir_rec("/tmp/app/data")        // rm -rf
  print(sua.fs.join("var", "log", "app.log"))
  print(sua.fs.abspath("../x.b"))
  ```

  Full method list: `exists`, `stat`, `mkdir`, `mkdirp`, `rmdir`, `remove`, `rmdir_rec`,
  `rename`, `copy`, `copy_rec`, `readdir`, `listdir`, `cwd`, `chdir`, `tempdir`,
  `abspath`, `basename`, `dirname`, `join`, `split`, `readfile`, `writefile`,
  `appendfile`, `readfile_bytes`, `writefile_bytes`, `chmod`. Cross-platform (POSIX +
  Windows). Covered by `tests/fs_test.b` (61 assertions, ALL GREEN).

- **[feature] `format()` — Python-style string formatting.** A new builtin with positional,
  auto-positional, and keyed placeholders, plus `{{`/`}}` escapes:

  ```bantu
  format("Hello, {}!", "World")                          // "Hello, World!"
  format("{0} + {0} = {1}", "1", "2")                    // "1 + 1 = 2"
  format("{name} is {age}", {"name": "Ada", "age": 36})  // "Ada is 36"
  format("literal {{braces}} here")                      // "literal {braces} here"
  ```

  Bad formats (unbalanced braces, missing keys, out-of-range indices) raise a clear
  `RUNTIME_ERROR`. Covered by `tests/format_test.b` (21 assertions, ALL GREEN).

### Added — Phase 6 body of work (formally released in v1.4.0)

The features below were developed on the `main` branch since v1.3.2 and are now formally
published as part of v1.4.0. They were previously documented under `## [Unreleased]`.

- **[feature] bplot's generators are Bantu, not Python.** `scripts/gen_circle_tables.b` and
  `scripts/gen_font_tables.b`, a TrueType reader, replace the Python scripts that wrote
  `raster_tables.hpp` and `raster_font.hpp`. Each regenerates its header byte for byte, and on all
  twelve DejaVu faces matplotlib ships the font reader's output matches the fontTools original.
  `tests/gen_tables_test.sh` holds them to the committed bytes. bplot's PNG corpus is also now
  **observed byte-identical in CI on Linux (GCC), Windows (MSVC) and macOS (arm64)**.
- **[feature] A cycle collector, so objects that refer to each other are freed too.** Reference
  counting frees a Bantu object the moment its last reference drops, but it cannot free a **cycle**
  — and three ordinary things made one, two of which the user never wrote:

  ```bantu
  $a.peer = $b; $b.peer = $a;          // a tree node and its parent
  $a.callback = $a.someMethod;         // a handler stored on its own object
  def outer() { def helper() { … } }   // a private helper inside a function
  ```

  The second is the interpreter's doing: binding a method builds a scope holding `this` and a
  function closing over it. The third is too: a nested `def` is stored into the very scope it
  captured, so **every call leaked its whole call frame**. Measured over 400,000 iterations each, the
  mutual pair reached **502 MB**, the stored method **558 MB**, and 20,000 nested-`def` calls held
  **20,001 scopes**. A `sua` handler doing any of these grew until the worker was OOM-killed, with no
  error and no log line — reachable by anyone who could send requests.

  Bantu now runs a cycle collector alongside reference counting, as CPython and PHP do. The same runs
  are **flat at 17–20 MB**, and ten times the run length costs under twice the memory. It infers its
  roots from reference counts rather than scanning a root set, so no C++ temporary can be missed; and
  it never frees anything itself — it clears a dead cycle's fields and lets reference counting do the
  freeing, so a logic error could not corrupt memory.

  **A program with no cycles never pays for it**: one million acyclic objects trigger **zero**
  collections, and the interpreter benchmark is **−0.85%** against the build before it existed,
  inside the ±2% gate. Three builtins make it observable:

  ```bantu
  gc_stats()         // {"live", "collections", "freed", "threshold", "enabled"}
  gc_collect()       // collect now; returns how many objects were freed
  gc_enable(false)   // stop the automatic one; returns the previous setting
  ```

  `BANTU_GC=0` disables automatic collection for the process; `gc_collect()` still works when it is
  off. Design, measurements and the rejected alternatives:
  [`docs/object-lifetime-architecture.md`](docs/object-lifetime-architecture.md). Tests:
  `tests/lang_gc_test.b` (55 assertions) and `tests/gc_stress.sh` (19 checks).

- **[feature] The same bplot figure is the same PNG file on every platform — gated in CI.**
  `tests/bplot_png_corpus_test.b` renders eight PNGs (every primitive, text in every anchor and
  rotation, far-off coordinates, whole figures at three dpi values) and checks each SHA-256 against a
  recorded literal; Linux and macOS run it with every suite, and the Windows job runs it explicitly.
  A 4000×3000 dashboard at 300 dpi renders in 2.2 s at a 220 MB peak, gated in
  `tests/bplot_stress.sh` — it took 51.6 s before B6f.

- **[patch] The rasteriser keeps an active edge table**, so its cost follows the crossings rather
  than every edge on every sub-scanline; and **bplot's heatmap encoder runs natively**
  (`bp_grid_paths`), byte-identical to the pure-Bantu path it replaces, which stays as the oracle.
  A 500×500 heatmap of real data went from 7 s to 0.2 s as SVG.

- **[feature] bplot writes PNG.** `savefig("chart.png", {"dpi": 150})` and `to_png(dpi)` draw any
  figure through the native raster backend — the same figure is the same file on Linux, macOS and
  Windows — and `samples/bplot/server.b` serves `/chart.png` beside `/chart.svg`. Text is measured by
  the backend doing the drawing, so a PNG's gutters fit its own font, and an SVG and a PNG rendered at
  once on two sua threads are each byte-identical to a lone render.

- **[bug fix] bplot pie slices were drawn as their mirror image.** Slices run counter-clockwise on
  screen, but each arc was emitted with SVG's clockwise sweep flag, so every slice bulged the wrong
  way about its chord — in the SVG as well as the PNG. Found the first time a pie was rasterised.

- **[bug fix] bplot twin axes drew their data in the wrong rectangle under `tight_layout`.** Each axes
  was measured for its own labels and the twin kept the box it measured, so a `twinx` line sat
  shifted by about a category and escaped the frame. Host and twin now share one rectangle with room
  for both sets of labels.

- **[feature] Text on the raster canvas, in an embedded font.** `bp_text` and `bp_text_width` draw
  and measure DejaVu Sans (210 glyphs: ASCII, Latin-1 and the symbols charts use) from integer outline
  tables, with no hinting and no system font, so a label is the same pixels on every platform. Anchors
  and rotation take the same arguments as the SVG backend's `text()`; invalid UTF-8 and missing
  characters draw as `U+FFFD`.

- **[bug fix] Raster coordinates far outside the canvas overflowed 64-bit arithmetic.** A point up to
  10⁸ units away reached ~2⁴⁰ in fixed point, and the clip crossing and a stroke's squared length
  multiplied two such values. Crossings now bisect when the exact product would not fit, and strokes are
  clipped to a guard box before any length is squared. Output for ordinary figures is unchanged.

- **[feature] The rasteriser: polygons, strokes and SVG path data, all in integers.**
  `bp_fill_polygon`, `bp_stroke_polyline` (caps, joins, dashes) and `bp_fill_path` draw with sixteen
  sub-scanlines of anti-aliasing and exact coverage along x. Circles and arcs step around **embedded
  integer tables** rather than calling `cos`, `sin` or `atan2`, which differ in the last bit between
  platforms and would break byte-identical output. Coverage was checked against analytic areas: a
  rectangle, a triangle and a clipped shape come out exact — and that check found a real defect, in
  which every stroke had a hole at its caps and joins because its pieces wound in opposite directions.

- **[feature] A raster canvas and a PNG encoder, written from scratch.** `bp_canvas_new`,
  `bp_fill_rect`, `bp_png` and `bp_png_save` draw into an RGB canvas and encode it — including a
  deflate implementation (LZ77 with dynamic Huffman codes), CRC-32, Adler-32 and adaptive PNG row
  filtering. Nothing is linked: the system zlib does not promise identical output across versions,
  and bplot's PNGs must be byte-identical on Linux, macOS and Windows. Checked against decoders we
  did not write — Python's `zlib` and Pillow — and the same canvas rendered twice in separate
  processes is byte-identical. This is the second step of the raster backend
  ([`docs/bplot-raster-architecture.md`](docs/bplot-raster-architecture.md)); the rasteriser and text
  follow.

- **[bug fix] `read($f)` could not be called.** `read`, `await`, `private`, `public`, `calc`,
  `import` and `export` were lexed as keywords that no grammar rule ever accepted, so the documented
  `read()` builtin failed to parse in every position — `$rest = read($f);` included — and no function
  could take one of those names. They are ordinary identifiers now. Found when the tests for binary
  file modes called `read()`.

- **[bug fix] Binary files could not be written, and an unknown file mode opened the file for
  reading.** `open($path, "wb")` — and `"w+"`, `"r+"`, `"x"` — fell through to read mode, so every
  write failed silently; and no file builtin set `std::ios::binary`, so on Windows every `\n` in a
  binary file became `\r\n`. `open`, `readfile`, `writefile` and `appendfile` now take `"rb"`, `"wb"` and
  `"ab"`; an unknown mode raises, naming the ones that exist; and a failed write raises instead of
  reporting success. The defaults remain text. This is the first step of bplot's PNG backend; its
  design is [`docs/bplot-raster-architecture.md`](docs/bplot-raster-architecture.md).

- **[feature] bplot 1.1.0 — a user guide whose every example runs, a served chart, and a clean-install
  gate.** [`docs/bplot.md`](docs/bplot.md) covers every chart family, tables from arctic, styles,
  serving from sua and the SVG security rules, with measured numbers. Three new gates make "finished"
  checkable: `tests/run_doc_examples.sh` executes every example in `docs/bplot.md`, `docs/numba.md`
  and `docs/arctic.md`, each doc as one program in reading order; `tests/bplot_package_test.sh`
  publishes, runs `bantu add` in an empty directory and draws, including a DataFrame through arctic's
  lazy include; and `samples/bplot/server.b` serves a chart per request, which
  `tests/bplot_sua_test.sh` holds to a Content-Security-Policy, an escaped hostile title and forty
  concurrent requests that each receive only their own chart.

- **[bug fix] numba's documented examples had never been run.** Its roadmap recorded "every
  documented example executed by CI"; nothing executed them, and two of nine were broken — a
  reduction over an axis the array did not have, and a loop over arrays its block never defined.
  Both fixed, and the doc runner now executes them in CI.

- **[bug fix] `num()` read a prefix, swallowed overflow and ignored bools.** `num("12abc")` was
  `12`, `num("1e999")` was `0` and `num(true)` was `0` — plausible wrong numbers in exactly the code
  that parses input it did not write. The whole string must now be a decimal number (surrounding
  whitespace allowed), overflow is infinity, and a bool is 1 or 0. Unparsable input still reads `0`,
  so existing handlers keep working; `num($s, $default)` returns `$default` instead, so a caller can
  tell a real zero from no number at all.

- **[feature] bplot draws arctic and numba data directly, and fast.** `plot`, `scatter`, `hist`,
  `boxplot` and `violin` accept a Bantu list, a numba ndarray, an arctic column or a `Series`, and
  produce **byte-identical documents** from all four. Numeric data now stays native until it becomes
  pixels: one 1,000,000-row column drawn as a line went from **~20 s and 3.1 GB** to **under a
  second**, because a Bantu list of a million 190-byte values was being copied on every function
  call. A null is a gap, a datetime column makes a date axis on its own, and
  `$df.plot({"kind": "bar", "x": "month", "y": ["rain", "temp"]})` draws a whole table — arctic
  includes bplot lazily, only when you plot. See `docs/bplot-architecture.md` §13.

- **[bug fix] `include "name"` no longer binds an empty module when a folder of that name exists.**
  The resolver accepted a directory as a module and parsed it as an empty file, with no error. A
  module must now be a regular file, and a bare name naming a directory resolves inside it
  (`package.json` `main`, `<name>.b`, `index.b`).

- **[bug fix] `len()` of a dict, an ndarray or a column returned 0**, so a loop bounded by it never
  ran. A dict counts its entries, an ndarray reports its first axis (a 0-d array raises), a column
  its rows.

- **[bug fix] `contains(list, value)` returned false for every list.** It now tests membership with
  the same equality as `==`.

- **[bug fix] bplot date axes at hour and minute resolution showed no date.** A four-day axis read
  `00:00 12:00 00:00 …`. Labels now follow matplotlib's defaults, `MM-DD HH` and `DD HH:MM`.

- **[feature] `sort` by a key, not just a comparator.** A comparator is called O(n log n) times; a
  key is called n times, and the ordering then happens in C++ on the keys alone.

  ```bantu
  def byAge($r) { return $r["age"]; }
  sort($rows, {"key": byAge})                  // youngest first
  sort($rows, {"key": byAge, "desc": true})    // oldest first
  ```

  On 100,000 rows, with the cost of building the list subtracted from both: **13,331 ms with a
  comparator against 764 ms with a key — 17× faster**, identical result. Python replaced `cmp=` with
  `key=` in 3.0 for the same reason. The key must return the same type for every element, `NaN` keys
  sort last in both directions, and passing both `"key"` and `"cmp"` raises rather than silently
  ignoring one.

- **[feature] `sort(list)` and `reverse(list)`** — the language had `push`, `pop`, `insert`,
  `extend` and `slice` and **no way to order a list**, so every median, quantile, boxplot, ranking
  and "top N" in every Bantu program was an interpreted sort. Both return a new list; the argument is
  untouched.

  ```bantu
  sort([3, 1, 2])                    // [1, 2, 3]
  sort([5, 2, 9], "desc")            // [9, 5, 2]
  sort(["aaa", "b", "cc"], byLength) // a comparator: negative, zero or positive
  reverse([1, 2, 3])                 // [3, 2, 1]
  reverse("abc")                     // "cba"
  ```

  `NaN` sorts **last in both directions** — a comparator that answers false to every `NaN`
  comparison is not a strict weak ordering, and `std::sort` given one reads past the end of its
  range. A mixed list of numbers and strings **raises**: ordering them has no correct answer, and
  picking one silently is how a sort quietly produces garbage that still looks sorted. A
  user-supplied comparator gets a merge sort, which cannot leave its range however badly the
  comparator behaves. See [`docs/language-features.md`](docs/language-features.md).

- **[feature] bplot grew its chart types, its scales and its layout engine** — `hist` `boxplot`
  `violin` `errorbar` `fill_between` `step` `stem` `pie`; `log` and `symlog` scales; calendar-aware
  date axes; annotations and arrows; categorical axes; `subplots`, twin and shared axes,
  `tight_layout`; `imshow` `heatmap` `pcolormesh` `contour` with colormaps and colorbars; and style
  sheets. Tick locations, the symlog transform, quantiles, boxplot statistics and histogram bins are
  all checked against matplotlib's and numpy's own answers, with the reference values embedded as
  literals — **nothing at run time depends on Python**. Details in
  [`bplot-suite/CHANGELOG.md`](bplot-suite/CHANGELOG.md).

### Fixed

- **[bug fix] `+` on two lists concatenates them.** It read the numeric value of both lists and
  silently answered `0`. `[1, 2] + [3]` is now `[1, 2, 3]`, a new list with neither operand
  touched, as in Python. Only list `+` list changed. The branch sits in the existing non-number path,
  so number arithmetic is untouched. See `docs/language-features.md`; 13 assertions in
  `tests/lang_list_test.b`.
- **[bug fix] The Windows build compiles again.** Three branch hints on the interpreter's hot path used
  `__builtin_expect`, which MSVC does not have (C3861). They now go through `BANTU_UNLIKELY` in
  `platform_compat.hpp`, which is the same builtin on GCC and Clang and no hint on MSVC. The MSVC
  fallback for `mulOverflows` multiplied first and checked after — itself the signed overflow it
  guards against — and now checks before multiplying; it agrees with `__builtin_mul_overflow` on 676
  edge pairs under UBSan. CI's AST tag check globs `src/*.cpp` instead of a hand-kept list that had
  missed `raster_native.cpp`.
- **[bug fix] CI and release workflows run on supported runners and actions.** `actions/checkout@v4`,
  `upload-artifact@v4`, `download-artifact@v4` and `action-gh-release@v2` target the deprecated
  Node 20; they are now v7, v7, v8 and v3, all Node 24. The release job's `macos-13` Intel runner is
  retired, so a version tag could not build the x64 macOS package; it is `macos-15-intel` now, and
  `macos-14` (deprecated) is `macos-15`. `ubuntu-latest` is pinned to `ubuntu-24.04`, so the move to
  Ubuntu 26 (from 2026-10-19) happens as a deliberate change, not mid-release.
- **[bug fix] `return`, `break` and `continue` no longer cost a C++ exception.** They were thrown
  and caught, and a throw walks the stack with the unwinder: **8.9 µs per `return`** against 1.2 µs
  for the whole rest of a call, and the same again for every `continue`. They are now a pending signal
  that each statement list checks between statements, and that loops and calls consume. A call that
  returns costs what one that does not costs: 100,000 returning calls **891 → 145 ms**, 100,000
  continued iterations **887 → 71 ms**, `benchmarks/bench.b` **35.7 → 6.7 s**. No syntax or semantics
  change — `tests/lang_control_flow_test.b` (35 assertions, every construct) passes before and after,
  and so does every suite. Found by bplot's 4000×3000 stress figure, whose profile was dominated by the
  unwinder. Design: [`docs/control-flow-architecture.md`](docs/control-flow-architecture.md).

- **[bug fix] Every object a Bantu program created leaked.** `new ClassName()` allocated an instance
  that **nothing ever deleted**, so instances accumulated for the life of the process — measured at
  ~300 bytes each, and a program building 20,000 plotting figures reached **372 MB** of resident
  memory and was still climbing. A `sua` handler creating objects per request grew without bound
  until the worker was killed. Instances are refcounted now, like every other Bantu value: 20,000
  instances is **flat at 4.9 MB**.

  Refcounting does not collect **reference cycles** — two objects that point at each other keep each
  other alive, as in Swift or any other refcounted runtime without a cycle collector. If you build a
  graph with back-references, break the cycle when you are done or hold the back-reference as an
  index. This is documented in [`docs/language-features.md`](docs/language-features.md).

- **[bug fix] `&&` and `||` now short-circuit.** They evaluated the right-hand side
  unconditionally, so the universal guard idiom

  ```bantu
  if ($i < len($a) && $a[$i] == 9) { ... }    // died with "Index out of bounds"
  if ($d != null && $d["k"] == 1) { ... }     // indexed a null
  ```

  failed on exactly the boundary it was written to prevent — and any side effect on the right ran
  even when the left had already decided the answer. The result is still a boolean, so nothing that
  already worked changes value. A/B'd on a 1M-iteration arithmetic loop containing no logical
  operators at all, best-of-5 in both orderings: **540/541 ms before against 535/533 ms after**.


- **[feature] bplot — data visualisation, in the shape of matplotlib** — 100% pure Bantu, emitting
  SVG. A chart is three lines:

  ```bantu
  include "bplot" as plt;
  plt.plot([1, 2, 3], [2, 4, 9]);
  plt.savefig("chart.svg");
  ```

  `plot` `scatter` `bar` `barh`; `title` `xlabel` `ylabel` `legend` `grid` `xlim` `ylim`; `savefig`
  `show` `to_svg` `clf` `help`. Underneath is a Figure/Axes/Artist model over a swappable backend —
  matplotlib's own layering, which is what let it change output targets for twenty years without
  breaking its API — and the objects appear nowhere in the quickstart. `x` and `y` accept a Bantu
  list, a numba ndarray or an arctic column.

  **Ticking matches matplotlib's `MaxNLocator`**, verified against its own choices on twelve ranges
  including negatives, `1e-9` and `1e9`. **NaN breaks a line into segments** rather than emitting
  `points="NaN,12 …"`, which every browser renders as nothing at all with no error anywhere. **Path
  simplification** bounds the output by the canvas rather than the data: a 100,000-point line is one
  polyline of 26 KB against 1.4 MB unsimplified, a 54× reduction, rendered in 1.1 s.

  **Security.** SVG is not an image format the way PNG is — it is XML that browsers execute, and
  `sua` already serves `image/svg+xml`, so a chart title taken from a request parameter is a stored
  XSS. Every text node and attribute value is escaped at the point of emission, with **no opt-out and
  no raw-SVG hatch**; control bytes are stripped rather than escaped, because they are not
  representable in XML 1.0 at all and one of them turns the chart into a blank page; and colours are
  validated against a shape rather than interpolated into an attribute. That makes injection
  *through bplot* impossible, but it cannot make SVG stop being executable — if you serve charts
  built from untrusted data, serve them from a separate origin or under a strict CSP.

### Changed

- **[perf] The interpreter is 4–6× faster** — and not for the reason anyone expected. A profile of a
  20-million-iteration arithmetic loop put **`dynamic_cast` at 79.6 % of all interpreter time**:
  `evalNode` dispatched by trying each of 38 node types in turn, in the order they had been added, so
  every function call paid for nineteen failed hierarchy searches before reaching its own arm. `Value`'s
  192-byte size, the allocator and the environment's string hashing — the costs previously blamed for
  Bantu's speed — came to 20 % between them.

  Dispatch is now a tag on the node and a `switch`: O(1), and the same cost for the 39th node type as
  for the 1st.

  | | before | after | |
  |---|---|---|---|
  | 1M arithmetic while loop | 2,574 ms | **448 ms** | **5.7×** |
  | 1M comparison loop | 2,436 ms | **539 ms** | **4.5×** |
  | 500k unary negation | 1,171 ms | **200 ms** | **5.9×** |
  | 200k list index read | 575 ms | **101 ms** | **5.7×** |
  | 100k list index write | 213 ms | **41 ms** | **5.2×** |
  | 50k dict set | 178 ms | **45 ms** | **4.0×** |
  | `fib(24)` recursive | 1,910 ms | **1,535 ms** | 1.24× |

  Recursion gains least because a call is dominated by building an environment and copying arguments,
  not by dispatch. Measured on an i7-9750H, best-of across both A/B orderings. The full profile, the
  alternatives considered, and how a wrong tag is made a *compile* error rather than undefined
  behaviour are in [`docs/interpreter-performance.md`](docs/interpreter-performance.md).

- **[perf] `$s = $s + $part` and `$s += $part` are linear** — they were O(n²), because every `+` built
  a fresh string holding a copy of everything accumulated so far. Assembling 1.16 MB out of 40,000
  pieces moved ~23 GB and took **6,752 ms**; it now takes **23 ms**, and 200,000 appends take 112 ms.

  When the result of `x + y` is assigned straight back to `x`, `x`'s old value is dead the moment the
  assignment lands, so it is appended to rather than copied — the same fix CPython has had since 2.4.
  `join()` is still the right idiom when the pieces are already a list; the difference is that
  reaching for the obvious `+=` no longer falls off a cliff.

  This covers fields and elements too — `$fig.parts += $x`, `$a[$i] += $x`, `$this.buf = $this.buf +
  $x` — which matters because accumulating through a field is what object-oriented code actually
  does. Left out, it would have been **64× slower than the identical code accumulating into a local**
  (1,413 ms against 22 ms for 40,000 appends); it now matches at 18 ms.

  Semantics are unchanged, and `tests/lang_perf_test.b` is mostly about proving it: an assignment
  inside a function still creates a local rather than mutating a global, `$t = $s` still keeps its own
  copy, `$s = $s + $s` still reads the old value, `const` still raises, and numbers, lists, dicts and
  native handles still take the ordinary operator path. A field target is held to a stricter rule
  than a local — no operand may run code at all — because a callee holding the same dict can replace
  the string being appended to, where Bantu's function-local assignment makes that impossible for a
  plain variable.

### Added

- **[feature] Scalar maths** — the language had `abs ceil cos floor log max min pow round sin sqrt
  tan random` and nothing else. It now also has the constants `PI TAU E INF NAN` and the functions
  `exp expm1 log1p log2 log10 cbrt asin acos atan atan2 hypot sinh cosh tanh asinh acosh atanh trunc
  sign fmod copysign degrees radians clamp isnan isinf isfinite`. Domain behaviour is IEEE 754's —
  `acos(2)` is `NaN` rather than an error, `log(0)` is `-inf`, NaN propagates — and a non-number
  argument raises a catchable error naming the argument and its type.

  `isnan`, `isinf` and `isfinite` close a real gap: these values were always *producible* — `log(0)`
  is `-inf`, `sqrt(-1)` is `nan`, `pow(10, 400)` is `inf` — and there was no way to test for one.

- **[feature] `join(list [, sep])`** — the inverse of `split()`, which has existed since v1.0 while
  its counterpart never did. That mattered more than it sounds: without `join`, the only way to build
  a string in a loop was `$s = $s + $part`, and that is **O(n²)** — every `+` copies the whole
  accumulated string. Forty thousand appends took **6,752 ms**; collecting the same parts with `push`
  and joining once takes **147 ms + 7 ms**, and is linear rather than quadratic.

### Fixed

- **[bug fix] `benchmarks/bench.b` called `sua.clock()`, which does not exist** — so it raised on its
  first benchmark, `benchmarks/run.sh` has been failing, and the numbers published in
  `benchmarks/results.md` and `benchmarks/README.md` came from a script that no longer ran. It now
  uses the global `clock()`, which already returns milliseconds — the `* 1000.0` it applied was wrong
  as well.

- **[bug fix] `max(1, 2, 9)` answered 2** — `max` and `min` read only their first two arguments and
  silently ignored the rest, in every shipped build. Both are now variadic, and a single list
  argument is reduced over it, so `max($points)` takes the range of a 100,000-element list in 13 ms
  instead of a 100,000-iteration loop. The two-argument form is unchanged. NaN propagates
  (`max(1, NAN, 3)` is `NaN`), matching NumPy's `max` as against its separate `nanmax`, because a
  primitive should not silently discard a value it was handed.

- **[bug fix] `$a[$i]` copied the entire list to read one element** — evaluation returns values by
  value and a list owns its elements inline, so reading one element out of a 20,000-element list
  deep-copied all 20,000, each a ~190-byte struct carrying a string, a vector, a `std::function` and
  three `shared_ptr`s. **Every loop over a list in every Bantu program was quadratic.** Indexing now
  borrows a pointer to the live container instead.

  | | before | after |
  |---|---|---|
  | 10,000 reads | 1,919 ms | **26 ms** |
  | 20,000 reads | 8,093 ms | **52 ms** |
  | 100,000 reads | ~3.4 min (extrapolated) | **282 ms** |

  Nothing about list semantics changed — `$b = $a;` is still a copy — and a read of a missing dict
  key still does not create it. Dicts and class instances never had this problem: they hold a
  `shared_ptr`, which is also why they have reference semantics and lists do not.

- **[bug fix] `str(1e21)` answered `"-9223372036854775808"`** — any integral double was cast to
  `long long`, and converting a floating-point value outside the destination integer range is
  undefined behaviour; it saturates to `INT64_MIN` on x86-64 and ARM64. Reachable from
  `pow(10, 21)`, and from any numba reduction whose total exceeds 2⁶³. It now prints `1e+21`, and
  `num(str($x))` round-trips at every magnitude.

### Added

- **[feature] arctic frames and series convert to numba arrays** — `$series.to_ndarray()` hands the
  column's own memory to numba without copying it, and `$frame.to_ndarray()` builds a matrix ready
  for `solve` or `lstsq`, optionally choosing and ordering the columns. Going the other way,
  `arctic.from_columns({...})` and `arctic.from_column(name, col)` build a frame or a series from
  native columns directly, so data coming back from numba does not have to be turned into a Bantu
  list first. `Series` also gained 21 maths methods — `.sqrt()`, `.exp()`, `.log()` and the rest —
  which work without numba and keep nulls as nulls.

- **[feature] arctic columns and numba arrays convert between each other** — `nd_from_column($c)`
  hands a column's data to numba **without copying it**, so a million-row column becomes an array in
  a millisecond; `nd_to_column($a)` copies back; and `nd_from_frame([$c1, $c2])` turns a set of
  columns into a matrix ready for `nd_solve` or `nd_lstsq`. A column with nulls is refused rather
  than quietly turned into NaN — arctic distinguishes "no value" from "not a number" and an array
  cannot — and converting NaN back into null is something you ask for explicitly.

- **[feature] arctic gained 21 maths functions** — `col_sqrt`, `col_exp`, `col_log`, `col_sin` and
  the rest. Nulls pass through as nulls. A domain error such as `col_sqrt(-1)` gives NaN and stays
  non-null, because the value was there; the function simply has no real answer for it.

- **[feature] numba is installable** — `bantu add numba`, then `include "numba" as np;`.
  `numba/numba.b` is the public API in plain Bantu, with sensible defaults the raw builtins cannot
  have (`np.arange(0, 10)` rather than `nd_arange(0, 10, null)`), plus composed helpers built from
  the same pieces: `polyfit`, `polyval`, `interp`, `gradient`, `cov`, `corrcoef`, `meshgrid`,
  `moving_average` and `trapz`. `np.help()` lists the API and `np.info($a)` describes an array.
  Documented in `docs/numba.md`, with four runnable programs in `samples/numba/`.

- **[feature] Samples are executed by CI** — `tests/run_samples.sh` runs every program under
  `samples/` and every package smoke test, and fails the build if one errors. Documentation rots
  silently: an example that stopped working still looks right in the docs, and the first person to
  find out is a new user in their first five minutes.

- **[feature] numba does linear algebra** — `matmul`, `solve`, `inv`, `det`, `cholesky`, `qr`,
  `lstsq`, `eigh`, `svd`, `pinv`, `norm`, `matrix_rank` and `cond`, all written from scratch with no
  external library. A 500×500 system solves in 23 ms to a residual of 1e-14; a 1000×1000 matrix
  multiply runs at 7.8 GFLOP/s.

  Degenerate input raises rather than returning something that looks like an answer: a singular
  matrix points you at `lstsq` or `pinv`, a matrix that is not positive definite says so and suggests
  `solve`, and `eigh` on a non-symmetric matrix names the element that broke the symmetry instead of
  quietly symmetrising it and answering a different question.

  Not included, deliberately: eigenvalues of a general non-symmetric matrix. Doing it properly needs
  complex arithmetic the language does not have, and a fragile version is worse than none — `eigh`
  already covers covariance matrices, PCA and graph Laplacians.

- **[feature] Arrays work with the ordinary operators** — `$a + $b`, `2 * $a`, `-$a`, `$a > 0.5`,
  `$m[1]`, `$m[1][2] = 99` and `$vals[$vals > 20] = 0` now mean what they look like on a numba array.
  Every one of these paths was previously dead: `$array + 1` read a number field that is always 0 for
  a handle and silently produced `1`, indexing returned null, and index assignment threw. Methods are
  available too — `$x.multiply($x).add($x).sum()` — bound to the same functions the `nd_*` builtins
  use, so the two spellings cannot drift apart.

  Two deliberate choices. `$m[1]` on a 2-D array is a **view**, so writing to it changes the
  original — the opposite of a nested list, and worth knowing. And `==` stays an identity comparison
  rather than becoming element-wise, so `if ($a == $b)` keeps meaning what it says; `nd_array_equal`
  and `nd_allclose` are the element-wise forms.

  Nothing else moved: strings still concatenate, `==` on lists and dicts is still structural, `&&`
  and `||` are unchanged, and the interpreter's hot paths measured within the noise floor.

- **[feature] numba summarises data** — 20 reductions (`sum`, `mean`, `std`, `var`, `min`, `max`,
  `median`, `quantile`, `argmin`, `argmax`, `any`, `all`, the `nan*` forms and more), each taking an
  axis — one, several, or all — and an optional `keepdims` that leaves the reduced axis as length 1
  so the result broadcasts straight back against the input. Plus running totals (`cumsum`,
  `cumprod`, `cummax`, `cummin`, `diff`), sorting (`sort`, `argsort`, `searchsorted`, `unique`,
  `bincount`, `histogram`) and indexing (`take`, `put`, `compress`, `nonzero`).

  Summing is accurate, not merely fast: adding ten million copies of `0.1` comes out with a relative
  error of 2e-16, where an ordinary running total would drift by about 2e-12. Empty input returns the
  operation's identity where there is one — nothing sums to 0 and nothing multiplies to 1 — while
  `min` of nothing raises, because any answer would be a wrong one.

- **[feature] numba can do arithmetic** — element-wise operations across ~55 functions: arithmetic,
  29 transcendentals, comparisons, boolean logic, `nd_where`, `nd_clip`, `nd_isclose` and
  `nd_allclose`. Shapes broadcast the way NumPy's do, so a `(2,3)` array and a `(3,)` row combine
  without copying anything, and a mismatch names the axis and both extents rather than saying only
  "shape mismatch". Scalars and ordinary Bantu lists work as operands directly — `nd_add($a, 2)`.
  Every function takes an optional destination, which is what lets a loop over large arrays run in
  constant memory instead of allocating a new 80 MB result each iteration.

- **[feature] numba's allocation limit is enforceable by whoever runs the process** —
  `BANTU_ND_MAX_BYTES` sets a hard ceiling on the memory numba may hold, read once at startup.
  `nd_max_bytes(n)` can lower a program's own limit but never raise it past that, so an operator
  running untrusted Bantu can cap it and a script cannot opt out. The limit now bounds **total live
  bytes**, not one allocation at a time, because a loop is how a request handler actually exhausts a
  server. `nd_live_bytes()` reports what is currently held. A malformed env value keeps the default
  rather than silently disabling the limit.

- **[feature] `nd_shares_memory(a, b)`** — whether two arrays address any of the same memory, the
  question you need before writing into one while reading the other. Conservative in the same way as
  NumPy's `may_share_memory`: overlapping extents count as sharing even where the stride patterns
  would never actually collide.

- **[feature] `include "<package>"` resolves installed packages** — `bantu add <pkg>` installs into
  `./bantu_modules/<pkg>/`, but the module resolver had no rule for that directory, so an installed
  package could only be reached by writing out its full path
  (`include "./bantu_modules/numba/numba.b" as np;`). A **bare** name now searches
  `bantu_modules/<name>/` beside the importing file and under the working directory, honouring the
  package's `package.json` `"main"` and falling back to `<name>.b`, `index.b`, `main.b`. Explicit
  relative paths are unaffected, so `./x.b` still means exactly what it says.

- **[feature] `print()` shows what a native handle contains** — a `NATIVE_HANDLE` stringified to
  `"<column>"`, which named the type and nothing else, so the only way to look at an arctic column
  was to materialize it into a Bantu list first. `Value::toString` now consults a registry of
  renderers keyed by handle tag, and each native layer registers one for the type it owns.
  Summarization follows NumPy's convention — every element up to 1000, then three from each end —
  so printing a five-million-row column is O(1):
  `print($c)` → `[1, 2, 3]  (len=3, dtype=f64)`.

- **[feature] Progressive Web Apps in `sua` (`sua.pwa`)** — any Bantu web app becomes installable and
  offline-capable from one config call. Modelled on Python's **django-pwa**; the research and design
  notes are in [docs/pwa-research.md](docs/pwa-research.md).
  `sua.pwa.configure({...})` takes one flat dict (the `PWA_APP_*` settings minus the prefix) and
  auto-registers **`/manifest.json`**, `/manifest.webmanifest`, **`/serviceworker.js`**,
  **`/offline`** and **`/pwa.js`**. The worker is served from the ROOT so its scope covers the whole
  origin — the single most common PWA bug. The generated worker precaches assets, serves navigations
  network-first with a cache and offline-page fallback, cleans up old cache versions, and handles
  `push`/`notificationclick`. `sua.pwa.meta()` renders the `<head>` block (django-pwa's
  `{% progressive_web_app_meta %}`); with `auto_inject` on it is patched into served HTML
  automatically, so an existing app becomes installable without touching a template.
  `window.BantuPWA` (from `/pwa.js`) exposes registration, `beforeinstallprompt` capture,
  `promptInstall()` and push subscription. Tests: `tests/sua_pwa_test.b` (101) and
  `tests/sua_pwa_http_test.sh` (32, boots a real server and curls it).
- **[feature] Web Push notifications (`sua.push`)** — real RFC 8291 (`aes128gcm`) payload encryption
  and RFC 8292 (VAPID) signing, which django-pwa does *not* provide (that is `django-webpush`).
  `sua.push.keys(path)` generates the VAPID keypair once and reuses it; `configure()` registers the
  subscribe endpoint; `send()`/`send_all()` deliver, and `send_all` **prunes subscriptions the push
  service reports as gone** (404/410). Subscriptions live in their own sqlite store and can be
  grouped by `tag`. Payloads are capped at 3993 octets and rejected before any network call.
- **[feature] P-256 and AES-128-GCM (`p256.hpp`, `aes_gcm.hpp`)** — self-contained, no new
  dependency, compiled unconditionally. Needed because Web Push mandates P-256, which libsodium does
  not provide; see **DECISIONS D14** for why this is a documented exception to "never hand-roll
  crypto" and the mitigations that make it acceptable (complete exception-free point formulas,
  no secret-dependent branches or memory indices, deterministic RFC 6979 nonces, computed rather
  than transcribed constants, validated public-key import). A known-answer selftest
  (FIPS 197, NIST GCM, RFC 5869, RFC 6979 §A.2.5, RFC 5903 — 52 checks) runs once on first use and
  **fails closed**: on any failure `has_native("webpush")` is false and every entry point returns
  `null`. Atoms are exposed as `webpush_*` plus `b64url_encode`/`b64url_decode`.
  Tests: `tests/webpush_test.b` (53).
- **[feature] `sua.http.request(opts)`** — the general HTTP client form, with arbitrary request
  headers and binary-safe bodies (string, byte-list, or object auto-serialised to JSON).
  The convenience helpers could not set an `Authorization` header at all.
- **[feature] `bantu init --pwa <name>`** — scaffolds an installable app: server with PWA and push
  wired up, offline page, placeholder icons, and a README explaining installability and key handling.
- **[feature] `file_exists(path)`** — `readfile()`/`open()` raise on a missing file, so there was no
  way to write a "create it if absent" flow in Bantu.
- **[feature] `docs/sua.md`** — the first reference for the sua framework: routing, `$req`/`$res`,
  static files, PWA, push, the HTTP client, and an honest list of known limitations.
- **[feature] ChatBantu is now a PWA** — installable, offline-capable, and pushes notifications while
  closed. Every existing route is untouched; `notify()` additionally sends a Web Push tagged to the
  recipient, and degrades to in-app-only when push is unavailable.

### Fixed

- **[bug fix] Least squares on tall data killed the process** — fitting 200,000 rows by 3 columns
  tried to allocate an intermediate matrix of 200,000 by 200,000, which is 320 GB, and the process
  was killed outright. Tall data is the normal case for a regression. The same fit now takes 9 ms.

- **[bug fix] numba's linear algebra could allocate past its own memory limit** — the working
  storage for `solve`, `qr` and the rest allocated directly rather than through the accounting that
  bounds every other numba allocation, so the limit that exists to stop the process being killed
  could be reached around. It now raises, naming the matrix size it wanted and the limit it broke.

- **[bug fix] "Cannot call non-function value" now says what went wrong** — it names the thing you
  tried to call and what it actually holds, and mentions the usual cause: Bantu keeps variables and
  functions in one namespace, so `$len = 3` replaces the `len()` function for the rest of the scope
  and every later `len(...)` fails.

- **[bug fix] Keywords could not be used as property names** — `$d.number`, `$d.string`,
  `$d.delete` and `$a.any()` all failed with "Expected property name after '.'", because the parser
  kept a hand-written list of which keywords were allowed after a dot and it was incomplete. Any
  word now works there, which is safe because a property name can only follow a dot. Latent for
  anyone whose dict happened to use one of those keys.

- **[bug fix] `samples/blogsite` called a database method that does not exist** — `sua.sqlite.open`
  is the API; the sample said `connect`. Broken for long enough that the shipped release reproduces
  it.

- **[bug fix] An array combined with null or a dict silently produced 0** — falling through to
  numeric addition of two non-numbers. It now raises and says what arrived.

- **[bug fix] Storing NaN or infinity in an integer array corrupted it silently** — writing NaN into
  an `i64` numba array stored `-9223372036854775808`, and writing infinity stored `0`, which is worse
  because it looks like a real answer. Neither has an integer representation, and nothing said so. It
  now raises and names the fix. Note that Bantu has a single number type, so `nd([1.0, 2.0])` creates
  an **integer** array — `1.0` and `1` are the same value — which is why this was reachable from
  ordinary-looking code. Ask for `"f64"` explicitly when an array needs to hold NaN.

- **[bug fix] Bantu could not read the numbers it prints** — `str(0.000012345678)` produces
  `"1.23457e-05"`, but writing that back into a program failed with `Undefined variable: e`, because
  the lexer stopped at the `e` and read the rest as an identifier. Scientific notation now lexes
  (`1e3`, `1E3`, `2.5e2`, `1e-3`, `1e+3`), so anything `str()` emits can be read back. The JSON
  parser accepted exponents all along, so the two halves of the language had disagreed about what a
  number is. A variable named `e` still works, and `2.5.round()` still parses — the exponent is only
  consumed when digits actually follow.

- **[bug fix] numba array slicing accepted arguments that were not numbers** — `nd_slice` read the
  numeric field of whatever `Value` it was given, which is `0` for a list, a string or null, so
  `nd_slice($m, [[[1,2], null, null], null])` was silently treated as index `0` instead of being
  rejected. Very large bounds were then converted in a way that is undefined outside the integer
  range, so a start of `1e300` quietly produced an empty array rather than an error. Every argument
  is now type-checked and bounded, and the message names the axis and which part was wrong.

- **[bug fix] A slice step could corrupt an array's strides through signed overflow** — on an array
  whose axis stride was 8, a step near `-2^62` overflowed `stride * step` and wrapped it to **0**,
  producing an array with a zero stride that was still marked writable. That is the exact state
  broadcast views are made read-only to prevent, and signed overflow is undefined behaviour rather
  than merely a wrong number. Both products are now checked.

- **[bug fix] numba's random stream and memory limit were shared across sua requests** — sua runs
  each connection's handler on its own thread, and both were process-wide. `nd_seed()` in one
  request therefore changed the random numbers every other in-flight request received — the same
  problem numba carries its own generator to avoid — and concurrent updates to the limit were a data
  race. The generator is now per-thread and the counters are atomic. Each thread seeds its own
  stream.

- **[bug fix] numba error messages repeated the builtin's name** — errors read
  `nd_slice: nd_slice: step cannot be zero on axis 0`, because the wrapper prefixed the name onto
  messages that already carried it.

- **[patch] Appending to a list was O(n²); it is now O(1)** — two independent causes, both
  pre-existing and both reproducible on the shipped release binary.
  `push` returned the mutated list, and a Bantu list is a `std::vector<Value>` with value semantics
  where each `Value` is ~190 bytes carrying a string, a vector, a `std::function` and three
  `shared_ptr`s — so every append deep-copied the whole list. **20,000 pushes took 9,616 ms; they
  now take 70 ms.** 100,000 went from roughly four minutes to 271 ms — `bantu bench`'s own
  "list push 100k" could not finish in ten minutes on the release binary and now runs at
  250 ms/iter. Separately, `len($var)` copied its argument, which made the common
  `$out[len($out)] = $v` idiom quadratic: **7,027 ms → 58 ms** at 20,000 items. That idiom is used
  throughout `hash.b` and `crypto.b`, whose pure-Bantu paths were therefore quadratic in input
  length.
  Semantics are preserved: `$x = push($x, v)` still returns the list (the parser now records that a
  call's result is discarded, so only the throwaway case skips the copy), `$l.push(x)` returns the
  new length as in JavaScript, and every `len()` answer is unchanged including a user-defined `len`
  shadowing the builtin. Hot paths unmoved — 1M-iteration arithmetic loop 2,336 → 2,186 ms,
  `fib(24)` 1,906 → 1,941 ms, 50k dict set 171 → 178 ms. Gate:
  [`tests/lang_list_test.b`](tests/lang_list_test.b).

- **[bug fix] Including the same module twice bound nothing** — the cycle guard returned before the
  alias was defined, so if two of your files both did `include "arctic" as arctic;`, whichever
  loaded second was left with an undefined `arctic` and the only clue was a line on stderr. Any
  application with more than one module hit this. Modules are now cached by canonical path and every
  later include binds that same namespace object, so one module means one namespace — as in Node. A
  genuine circular include is reported by name and skipped. `sua.include()` had the same defect in a
  different shape (a second call returned `{"_cached": true}` instead of the module) and now shares
  the cache. Gate: [`tests/lang_module_test.b`](tests/lang_module_test.b).

- **[bug fix] Outbound HTTP truncated binary bodies at the first NUL byte** — `CURLOPT_POSTFIELDS`
  was set without `CURLOPT_POSTFIELDSIZE`, so libcurl called `strlen()` on the buffer. An
  `aes128gcm` push body starts with 16 random octets, so roughly two in five would have been
  silently truncated. Bodies are now length-explicit.
- **[bug fix] TLS certificates were never verified** — `CURLOPT_SSL_VERIFYPEER` was hard-coded to 0,
  making every outbound HTTPS request unauthenticated. Verification is now **on by default**, with an
  explicit per-request `"insecure": true` escape hatch (**DECISIONS D15**).
- **[bug fix] `sua.http.*` logged every request URL to stdout** — unconditionally, which would have
  written subscriber push endpoints into application output. The trace is now opt-in and goes to
  stderr.
- **[bug fix] Static file MIME types** — the table covered nine extensions and lacked
  `.webmanifest` (required for a manifest served as a file), fonts, WebP/AVIF, WASM, and media.
  Now ~28 types. HTML and manifests are also served `no-cache` so a stale shell cannot pin itself.

- **[feature] Native digest accelerators (hash performance)** — byte-identical C++ fast paths for
  MD5/SHA-1/SHA-224/SHA-256/HMAC-SHA256 (`crypto_native.hpp`), which the `hash` module transparently
  delegates to via a new `has_native(name)` feature check, falling back to the pure-Bantu reference
  when absent. Throughput goes from ~3 KB/s to **hundreds of MB/s**; `hash_file` now reads+digests a
  file entirely in C++ (a 5 MB file hashes in ~0.15 s, matching `shasum`). A differential test
  (`tests/crypto_differential_test.b`, 69 assertions) asserts native == pure at every block boundary.
- **[feature] SHA-512 / SHA-384** — `hash.sha512`/`hash.sha384` (+`_hex`/`_bytes`). Native-only
  (64-bit words can't be represented exactly by Bantu's float64); return `null` on a build without
  the accelerator. Validated against the official vectors.
- **[feature] Authenticated encryption + password hashing (libsodium, opt-in)** — `crypto.encrypt`/
  `decrypt` (XChaCha20-Poly1305-IETF, random per-message nonce, constant-time tag; `null` on
  tamper/wrong-key) and `crypto.hash_password`/`verify_password` (argon2id). Backed by libsodium via
  `crypto_sodium.hpp`; **compiled only with `BANTU_SODIUM=1`** (statically linked), so the default
  build gains no runtime dependency. Guard with `crypto.encryption_available()`. Covered by
  `crypto/crypto_encrypt_test.b` (skips cleanly when not built in).
- **[feature] `eprint(...)` builtin** — writes to stderr (diagnostics/warnings without corrupting
  stdout).

### Changed

- **[feature] Transitive dependency resolution** — `bantu add <pkg>` now installs the package's
  declared `dependencies` recursively (cycle-safe), so e.g. `bantu add crypto` auto-pulls `hash`.
  `crypto`/`uuid` manifests declare their `hash` dependency. No-dependency packages behave as before.
- **[patch] MD5/SHA-1 misuse guard** — `md5(...)`/`sha1(...)` full digests now emit a one-time
  stderr notice steering to SHA-256/HMAC (silence with `hash.allow_insecure(true)`); the `*_bytes`
  cores used by UUID v3/v5 stay silent.

### Fixed

- **[bug fix] CSPRNG portability + fail-closed** — `randbytes`/all secret material now sources OS
  entropy on Windows (`BCryptGenRandom`) and Linux (`getrandom(2)` → `/dev/urandom`) as well as
  Apple/BSD, and **fails closed** (errors rather than ever falling back to a predictable PRNG).

- **[bug fix] Function-local variable scoping** — a plain assignment `$x = v` inside a function used
  to walk the whole scope chain and mutate a caller's or a global variable of the same name. A
  callee reusing a caller's loop counter (e.g. `$i`) would reset it, causing infinite loops. Plain
  assignment is now **function-local** (Python-style): it resolves only up to the enclosing function
  boundary and otherwise defines a local; reads still fall through to enclosing scopes so functions
  can read globals. Module top-level `$vars` now correctly stay in the module (and are exposed as
  namespace members) instead of leaking into global. Implemented via an `Environment::assign()` with
  a `functionScope` boundary flag on function-call envs, module envs, and the global root. OOP
  dispatch/inheritance untouched. Guarded by `tests/scope_test.b`; no regressions (lang/classes,
  ORM, Sua, random all green). This is what let the hash/HMAC/UUID functions be called in loops.

### Added

- **[feature] Native crypto primitives** — additive builtins in the interpreter enabling
  production-grade cryptography written in pure Bantu: 32-bit bitwise/modular ops
  (`band/bor/bxor/bnot/shl/shr/rotl/rotr/add32/mul32`), byte/hex conversion (`bytes/frombytes/ord/
  tohex/fromhex`, with `chr` extended 0–127 → 0–255), an OS CSPRNG (`randbytes`), and constant-time
  `ct_equal`. Bytes are represented as a Bantu list of 0–255. Covered by
  `tests/crypto_primitives_test.b` (40 assertions).
- **[feature] `hash` package** — MD5, SHA-1, SHA-224, SHA-256 and HMAC-SHA256, written in pure
  Bantu on the primitives above and **bit-exact to the RFC/NIST test vectors** (cross-checked vs
  `shasum`/`md5`/`openssl`), plus non-crypto `djb2`/`fnv1a` and `hash_file`. MD5/SHA-1 documented as
  non-secure (checksums/UUID-namespace only). `hash/hash_test.b` (25 vectors). Docs `docs/hash.md`.
- **[feature] `crypto` package** — OS-CSPRNG secure random (`random_bytes`, `token_hex`,
  `token_urlsafe`, unbiased `random_int`), HMAC-SHA256 with constant-time `verify_hmac`,
  HKDF-SHA256 (RFC 5869), and base64/base64url/hex (RFC 4648). AES and password-KDFs are
  intentionally out of scope (a future vetted native layer). `crypto/crypto_test.b` (31 tests).
  Docs `docs/crypto.md`.
- **[feature] `uuid` package** — RFC 4122 / RFC 9562 `uuid4` (CSPRNG), `uuid7` (time-ordered),
  `uuid3`/`uuid5` (name-based, bit-exact to RFC vectors), namespaces, and
  `parse/format/is_valid/version_of`. `uuid/uuid_test.b` (17 tests). Docs `docs/uuid.md`.
- **[feature] `random` standard-library package** — a Python-style random-number module written
  entirely in Bantu (`random/random.b`), with a **seedable** pure-Bantu generator so
  `random.seed(n)` reproduces sequences exactly (Bantu's built-in `random()` is a
  Mersenne-Twister that cannot be seeded). API: `seed`/`getstate`/`setstate`, `random`, `uniform`,
  `randint`, `randbelow`, `randrange`, `randrangeStep`, `randbool`, `choice`, `choices`,
  `choicesUniform`, `sample`, `shuffle` (returns a copy — Bantu lists are pass-by-value),
  `gauss`/`normalvariate`, `expovariate`, `triangular`, and `getrandbits`. Engine is a 32-bit LCG
  whose arithmetic stays exact in doubles; documented as non-cryptographic. Include with
  `include "./random.b" as random;`. Covered by `random/random_test.b`; docs at `docs/random.md`.

## [1.3.2] — 2026-09-06

WebSocket + voice release: real-time bidirectional communication via
`sua.ws` namespace with binary frame support for voice/audio data.
Also includes a collaborative IDE demo with chat, voice, and live code
editing.

### Added

- **[feature] WebSocket support (`sua.ws` namespace)** — Bantu now has
  true real-time bidirectional communication via WebSockets (RFC 6455).
  No more HTTP long-polling — this is Socket.IO-speed (sub-50ms latency).
  New builtins: `sua.ws.on`, `sua.ws.send`, `sua.ws.broadcast`,
  `sua.ws.clients`, `sua.ws.send_binary`, `sua.ws.broadcast_binary`,
  `sua.ws.send_to`.
- **[feature] Binary WebSocket frames** — voice/audio data can be
  transmitted as binary frames (opcode 0x02). The on(message) handler
  receives `{data, bytes, binary, client, json}` where `bytes` is
  a list of 0-255 integers and `binary` is true.
- **[feature] Collaborative IDE** — chat + voice + real-time code
  editing demo using CodeMirror, all powered by Bantu WebSocket.
- **[feature] SHA-1 + Base64** — inline implementations for the
  RFC 6455 WebSocket handshake.
- **[feature] Wildcard /* route matching** — enables SPA fallback
  for `bantu-auto-frontend`.

### Changed

- Bumped version constant in `main.cpp`: `1.3.1` → `1.3.2`.

## [1.3.1] — 2026-09-06

Installer + tooling release: real Windows installer with brand icon, live linting in
VS Code that shows red squiggles on syntax errors as you type, and a new `sua.udp`
namespace for native UDP networking. **No language semantics changed** — every v1.3.0
program runs unchanged.

### Added

- **[feature] Native UDP networking (`sua.udp` namespace)** — Bantu can now open raw UDP
  sockets and speak UDP-only protocols directly. Seven new builtins:
  `sua.udp.socket`, `sua.udp.bind`, `sua.udp.send_to`, `sua.udp.recvfrom`,
  `sua.udp.send` (one-shot), `sua.udp.close`, `sua.udp.getsockname`. Verified against
  real DNS (8.8.8.8), STUN (stun.l.google.com), and self-echo. Enables STUN/TURN servers,
  DNS clients, IoT relays, real-time games, all in pure Bantu.
- **[feature] Windows installer with brand icon** — the NSIS installer now embeds the
  official Bantu icon (multi-resolution .ico, 16/32/48/64/128/256 px). Shows in the
  installer wizard, Add/Remove Programs, file associations, and Start Menu shortcuts.
- **[feature] Live linting in VS Code** — the Bantu VS Code extension's
  `diagnosticsProvider.ts` runs `bantu lint --json` on the current buffer every
  300 ms (debounced). Syntax errors show as red squiggles as you type, not only on save.
  The interpreter's compile gate refuses to `run`/`build` files that still contain
  errors, so the editor and the toolchain agree.
- **[feature] VS Code extension icon** — replaced the placeholder blue-B with the
  official Bantu icon at 256×256 px (extension-icon.png) and 64×64 px (file icons).

### Changed

- Bumped version constant in `main.cpp`: `1.3.0` → `1.3.1`.
- VS Code extension package version: `1.3.0` → `1.3.1`.

### Compatibility

- v1.3.1 is a drop-in replacement for v1.3.0. No language changes, no breaking API
  changes. All v1.3.0 programs run unchanged.

## [1.3.0] — 2026-07-10

Core-language correctness release: the features that were advertised via keywords but silently
failed now work, the parser can no longer spin on a bad token, and several requested capabilities
(dict iteration, file I/O, FFI, parameterized SQL) land. **Object-oriented features were not
touched.**

### Fixed

- **[bug fix] Compound assignment** — `+=`, `-=`, `*=`, `/=` now apply the base arithmetic
  operator (the parser had passed the compound token `PLUS_EQUALS` straight into the evaluator,
  which rejected it as "Unknown operator"). This also removes an infinite loop where a `for`
  counter using `i += 1` never advanced. (`parser.hpp` `parseAssignment`)
- **[bug fix] `break` / `continue`** — were inert no-op nodes; they now compile to real AST nodes
  and throw the loop-control signals that `while`/`for`/`each` already caught.
- **[bug fix] `try` / `catch`** — runtime errors are now actually catchable. `ErrorHandler` throws
  a `BantuError` instead of merely printing, so `catch ($e)` receives a structured
  `{message, type, line}` (or the thrown value for `throw`).
- **[bug fix] Deterministic parser recovery** — a syntax error is reported once and the parser
  synchronizes to the next statement instead of re-reading the stuck token forever. `bantu run`
  now prints all diagnostics and refuses to execute (compile gate).
- **[bug fix] `$`-prefixed reserved words** — `$db`, `$create`, `$list`, `$switch`, … are valid
  variable names again; the `$` sigil forces the following word to be an identifier.
- **[bug fix] Bare `return;`** — returning with no value (or at the end of a block) yields null
  instead of a parse error.
- **[bug fix] `push()`, `$l.push()` and `$l.pop()` now mutate** — all three captured the list by
  value and were silent no-ops. They are resolved against the real storage now. `push()` returns
  the mutated list, so the older `$l = push($l, x)` idiom works correctly too.
- **[bug fix] ORM binds values instead of escaping them** — `orm/orm.b` now emits placeholders and
  collects bound parameters (SQLite `?`, PostgreSQL `$1…$n`), retiring string interpolation as the
  injection boundary. `_escape()` remains only for identifiers/DDL defaults and dialects without a
  parameter path. Covered by injection tests in `orm/orm_test.b` (61 assertions).
- **[patch] `sua.mysql.*` documented as a simulation** — `HAS_MYSQL` is defined by CMake but never
  referenced by the evaluator, so MySQL returns canned rows regardless of build flags, and
  `drivers/mysql_driver.hpp` / `drivers/postgres_driver.hpp` are included by nothing. The README's
  "real with `-DBANTU_MYSQL=ON`" claim is corrected; see `docs/v1.3.0-status.md`.

### Added

- **[feature] `switch` / `case` / `default`** — `switch ($x) { case 1 { … } default { … } }`,
  braces required, no fallthrough (first match wins).
- **[feature] `throw`** — `throw <value>;` raises any value; caught by `try/catch`.
- **[feature] `const` is truly constant** — reassigning a `const` binding is now an error (like
  Java `final`), enforced at runtime and flagged by the linter. The referenced object may still
  be mutated.
- **[feature] Anonymous functions** — `def($a, $b) { … }` is a first-class value (usable as a
  dict entry, argument, etc.).
- **[feature] Python-style `for … in …`** — `for $x in $list { }` and
  `for $key, $value in $dict.items() { }`; `each` also accepts a second variable.
- **[feature] Dict iteration & methods** — `$d.items()`, `$d.keys()`, `$d.values()`,
  `$d.size()`, plus `keys()`/`values()`/`entries()` builtins; `.size()` on lists/strings.
- **[feature] In-place list mutators** — `append(l, x)`, `pop(l)`, `insert(l, i, x)`,
  `remove(l, i)`, `extend(l, l2)`.
- **[feature] Python-style file I/O** — `open(path, mode)` (`"r"`/`"w"`/`"a"`),
  `read`, `readline`, `readlines`, `write`, `close`, plus one-shot `readfile`, `writefile`,
  `appendfile`.
- **[feature] FFI via libffi** — `loadlib("libm.dylib")` + `func(lib, "sqrt", "double",
  ["double"])` returns a callable that invokes the C symbol. Types: `int`, `double`, `string`,
  `pointer`, `void`. Built in on Linux/macOS (`-DBANTU_FFI -lffi -ldl`).
- **[feature] Parameterized SQL** — `sua.sqlite.exec/query(sql, [params])` binds `?` placeholders
  via prepared statements, and `sua.postgres.exec/query(sql, [params])` binds `$1…$n` via
  `PQexecParams` (both injection-safe). The ORM now uses this path for every value.
- **[feature] Linter + compile gate** — `bantu lint <file> [--json]` reports syntax errors and
  const/type issues (error = red, warning = yellow). `run`/`build` refuse to execute on errors
  (`--no-lint` opts out). The VS Code extension shows these live as you type.

### Notes

- **[patch]** Deferred to future releases: a bytecode VM (performance), `async`/`await`, and
  native binary compilation.
- **[patch]** Windows FFI is stubbed for now (the builtins raise a clear "not available" error);
  Linux/macOS ship it enabled.

## [1.2.2] — 2026-06-20

### Fixed

- **Module path canonicalization** — `include` now resolves through `realpath()` (POSIX) / `GetFullPathName` (Windows) so the same file reached via different relative paths (`./pkg/x.b`, `../pkg/x.b`, `pkg/x.b`) collapses to a single canonical key. This prevents the cycle guard from accidentally executing a module twice when it is reached via two different paths in the same project.
- **Cycle-guard diagnostic** — Previously, a circular include was silently skipped with no message. It now prints `[INCLUDE] Skipping already-loaded module: <path>` so users can see what was elided. The depth limit also surfaces a clear `[INCLUDE ERROR] Maximum include depth (64) exceeded` if a generated or pathological include chain escapes the cycle guard.
- **Depth limit** — A new `kMaxIncludeDepth = 64` guard prevents stack-exhaustion crashes on pathological include chains (e.g. self-generating scripts).
- **Error attribution** — `Module not found` errors now include the importing file path so the user knows which file made the bad `include` call:
  ```
  [INCLUDE ERROR] Module not found: ./missing.b (imported from /app/routes.b)
  ```
- **`[INCLUDE]` log channel** — Errors now go to `stderr` with the `[INCLUDE ERROR]` tag (was previously mixed into `stdout` as `[INCLUDE]`), so users can filter them separately from informational messages.

### Added

- **`--quiet` / `-q` global flag** — Suppresses informational `[INCLUDE] Loaded …` and `[Executed in … us]` lines. Useful for production server logs and benchmark harnesses. Errors still print to `stderr`.
  ```bash
  bantu --quiet run server.b
  bantu -q run server.b
  bantu run server.b --quiet   # also accepted
  ```
- **`$BANTU_PATH` module search path** — When `include` cannot find a module via the standard resolution order (absolute → importing-file dir → cwd → with `.b` appended), it now falls back to each directory listed in the `BANTU_PATH` environment variable (POSIX `:`-separated, Windows `;`-separated). Lets users install shared module libraries outside their project tree:
  ```bash
  export BANTU_PATH=/opt/bantu/lib:~/bantu-modules
  bantu run app.b   # can `include "auth.b";` from either dir
  ```
- **`bantu installer --platform android`** — Cross-platform desktop installer generator now also produces a complete Android Studio project (Gradle, Kotlin, WebView-based launcher) ready to build into a sideloadable APK. Bundles `.b` sources in `app/src/main/assets/bantu/`, looks for a pre-built arm64 `libbantu.so` in `./android/`, `~/.bantu/android/arm64-v8a/`, or `$BANTU_ANDROID_ARM64`, and ships a `build-apk.sh` wrapper plus `BUILD-ANDROID.md` with full NDK cross-compile instructions. The resulting APK runs fully offline on Android 7.0+ phones with no Bantu installed on the device.

### Changed

- Bumped version constant in `main.cpp`: `1.2.1` → `1.2.2`.
- VSCode extension package version: `1.2.1` → `1.2.2` (re-packaged as `bantu-vscode-1.2.2.vsix`).
- Sample app `bantu.json` versions bumped to `1.2.2`.
- Benchmark script header updated to `Bantu v1.2.2 Benchmark Suite`.

### Compatibility

- v1.2.2 is a drop-in replacement for v1.2.1. No language changes, no breaking API changes. All v1.2.1 programs run unchanged.
- Optional real-driver glue (`-DBANTU_POSTGRES`, `-DBANTU_MYSQL`, `-DBANTU_WEBRTC`) unchanged.

## [1.2.1] — 2026-06-20

### Added

- **`include` keyword** — language-level module imports with two forms:
  - `include "./routes.b";` — direct: symbols flow into the importer's scope
  - `include "./ctrl.b" as ctrl;` — namespaced: symbols bound under an alias
  - Path resolution: relative to importing file → relative to cwd → with `.b` appended
  - Idempotent: a module is executed only once per execution
  - Cycle guard: circular includes are detected and broken silently
- **`sua.include(path)` runtime function** — load a module dynamically, returns it as a dict (does not pollute scope)
- **`sua.webrtc.*` namespace** (v1.2.2) — explicit WebRTC peer / data-channel API:
  - `sua.webrtc.peer(id)`
  - `sua.webrtc.createOffer(peerId)` / `createAnswer(peerId)`
  - `sua.webrtc.addIceCandidate(peerId, candidate)`
  - `sua.webrtc.dataChannel(label)`
  - `sua.webrtc.send(channel, msg)`
  - `sua.webrtc.close(peerId)`
- **`bantu build-windows` command** — generates an NSIS `.exe` installer from any Bantu project
  - Bundles interpreter, source files, manifest, launcher, Start Menu shortcuts, uninstaller
  - `--name <Name>` and `--version <x.y.z>` flags
  - No admin rights required (uses `RequestExecutionLevel user`)
- **`bantu bench` command** — built-in micro-benchmark suite
- **VSCode extension** (`vscode-extension/`) — first-class editor support:
  - TextMate grammar for syntax highlighting
  - Context-aware autocomplete (keywords, types, `sua.*` namespaces & methods, `$variable` hints)
  - Hover hints for every keyword and `sua.*` method
  - Go-to-Symbol (`Ctrl+Shift+O`) for `def`, `class`, `include`, top-level `$var`
  - 20+ snippets (def, class, if, each, include, sua.server, etc.)
  - **Blue-B file icon** for `*.b` files (light + dark variants)
  - Commands: Run File (F5), Initialize Project, Initialize Sua Web Project, Build Windows Installer, Run Benchmarks
- **Three sample apps** in `samples/`:
  - `blogsite/` — modular blog backend (Sua + SQLite, uses `include` keyword across 5 files)
  - `webrtc-chat/` — WebRTC signaling server + browser chat UI
  - `pg-dashboard/` — analytics dashboard backed by PostgreSQL
- **Optional real-driver glue** (`drivers/`) for compile-time linking:
  - `postgres_driver.hpp` (libpq, `-DBANTU_POSTGRES=ON`)
  - `mysql_driver.hpp` (mysqlclient, `-DBANTU_MYSQL=ON`)
  - `webrtc_engine.hpp` (libdatachannel, `-DBANTU_WEBRTC=ON`)
- **Module resolver** (`bantu-src/compiler/src/module_resolver.hpp`) — handles path resolution + lex/parse of included files
- **30-page official PDF guide** at `docs/Bantu-Programming-Language-v1.2.2.pdf`

### Changed

- Bumped version: `1.2.0` → `1.2.1`
- `CMakeLists.txt` now declares `project(bantu VERSION 1.2.2)` and adds three CMake options (`BANTU_POSTGRES`, `BANTU_MYSQL`, `BANTU_WEBRTC`) with `find_path` / `find_library` detection
- `runCode()` in `main.cpp` now calls `evaluator.setEntryPoint(filename)` so that `include` statements resolve relative to the file being run
- `sua.webrtc.peer()` returns a `platform` field indicating whether libdatachannel or the stub is in use

### Documentation

- New 30-page PDF: `docs/Bantu-Programming-Language-v1.2.2.pdf`
- Updated `README.md` to reflect v1.2.2 features
- New `CHANGELOG.md` (this file)
- New `benchmarks/README.md` and `benchmarks/results.md`
- New `vscode-extension/README.md`
- New `samples/blogsite/README.md`, `samples/webrtc-chat/README.md`, `samples/pg-dashboard/README.md`

## [1.2.0] — Earlier

- PATH integration (`bantu setup`, `bantu setup --system`, `bantu setup --seed`)
- Offline package manager (`bantu install / add / remove / update / list / search / publish`)
- `bantu init` and `bantu init --web` scaffolders
- `bantu doctor` diagnostics
- `bantu.json` project manifest
- Local package registry at `~/.bantu/registry/`

## [1.1.0] — Earlier

- Sua HTTP framework (`sua.server.get/post/put/delete/patch/head/options/use/static/listen`)
- Sua HTTP client (`sua.http.get/post/put/delete/patch/head`) backed by libcurl
- Sua JSON helpers (`sua.json.parse/stringify`)
- Sua SQLite driver (`sua.sqlite.connect/exec/query/close`)
- Sua PostgreSQL stub (`sua.postgres.connect/query/close`)
- Sua MySQL stub (`sua.mysql.connect/query/close`)
- Real-time primitives: `sua.channel/signal/stun/broadcast/relay/stream/connect`
- Classes with `extends`, `super`, `public`/`private` visibility
- `try`/`catch`, `switch`/`case`, `each` loop
- Type annotations: `number`, `string`, `bool`, `list`, `dict`, `any`, `func`

## [1.0.0] — Initial Release

- Lexer, parser, tree-walking evaluator
- Variables (`$name = value`), numbers, strings, booleans, lists, dicts
- Control flow: `if`/`else`, `while`, `for`, `break`, `continue`, `return`
- Functions (`def`), closures, higher-order functions
- Built-in `print`, `read`, `clock`, `sleep`, `len`, `keys`, `values`
- REPL
- `bantu run <file.b>` and `bantu build <file.b>` commands
