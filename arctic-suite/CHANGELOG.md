# arctic Foundations — Changelog

Initiative-scoped, dated log of the native foundations. Searchable tags: `[feature]` / `[bug fix]` /
`[patch]`. The top-level `CHANGELOG.md` gets summarized entries when the foundations ship; this log
tracks granular per-phase progress, including feature- and stress-test results.

## [Unreleased]

### 2026-09-07 — arctic 1.2.0: API completion (window · set · text · reshape · combine · JSON)
Closes the remaining gaps against pandas/polars so the package is feature-complete for everyday
data work. Native atoms first (for speed), then the pure-Bantu API on top.

- **[feature] New native atoms** (`dataframe_native.hpp` + builtins): `col_cumsum/cumprod/cummax/
  cummin`, `col_shift`, `col_rank` (min-method ties), `col_quantile` (linear interpolation),
  `col_reverse`, `col_concat` (dtype-widening), `col_unique_mask` (first-occurrence mask),
  `col_is_in`, `col_round`, `col_full` (constant column), and text: `col_upper/lower/strip/str_len`,
  `col_contains/starts_with/ends_with`, `col_replace`, `col_substr`. Text atoms accept categoricals.
- **[feature] Series**: `unique value_counts mode is_in between drop_nulls is_not_null`,
  `cumsum cumprod cummax cummin shift diff pct_change rank`, `quantile clip round product first last
  n_largest n_smallest reverse slice`, text `upper lower strip str_len contains starts_with ends_with
  replace substr`, and `map/apply` for arbitrary Bantu functions.
- **[feature] DataFrame**: multi-column `sort([a,b], desc)`, `unique/drop_duplicates`, `drop_nulls`,
  `fill_null`, `null_count`, `slice`, `reverse`, `is_empty`, `n_largest/n_smallest`, `sample`,
  `with_columns`, `cast`, `row`/`iter_rows`, `value_counts`, `quantile`, `corr`, **`pivot`** and
  **`melt`**, **`concat`/`vstack`/`hstack`**, and `to_json`.
- **[feature] GroupBy**: `std var median nunique any_ all_`, `size()` and `stats(col)`.
- **[feature] LazyFrame**: `drop rename unique drop_nulls reverse slice fill_null limit join`
  (accepts an eager or lazy right side), all wired through the optimizer and `explain()`.
- **[feature] JSON I/O**: `arctic.read_json(pathOrText)` and `DataFrame.to_json(path?)`.
- **[bug fix] Optimizer correctness — filters are no longer hoisted past a barrier.** Previously a
  filter was hoisted to the front unconditionally, so `.head(2).filter(...)` was reordered into
  `.filter(...).head(2)` — a different result. Ops that change row counts (`head/tail/slice`), pick
  first-wins rows (`unique`), rewrite values (`fill_null`) or change the namespace (`rename/join/
  with_column/groupby`) are now barriers. Projection pushdown is likewise disabled when a plan
  contains ops whose column needs can't be determined statically.
- **[bug fix] Host `json.stringify`/`json.parse` were placeholders** — stringify returned
  `Value::toString()` (invalid JSON: unquoted keys) and parse echoed its input, so JSON could not
  round-trip. Both now delegate to the real serializer/parser already used by the HTTP layer, and no
  longer print `[JSON] …` noise on every call.
- **[patch]** `pivot` and `sample` use direct dict lookups instead of `keyIn()` (which scans every
  key); `pivot` was quadratic in the group count before this.
- **Tests:** `tests/arctic_window_test.b` 34/34 (native atoms), `tests/arctic_api_test.b` 72/72
  (full API incl. barrier correctness and JSON round-trip). **Stress (1M rows):** cumsum 10 ms,
  shift 29 ms, concat 39 ms, upper 67 ms, is_in 67 ms, unique 163 ms, quantile 158 ms, rank 499 ms;
  package-level value_counts 144 ms, melt 1M→2M 258 ms, groupby stats 778 ms, multi-sort 1.29 s,
  realistic pivot (5×12) 251 ms. Full regression green on default and Arrow builds.

### 2026-09-07 — Follow-up: in-place list-field mutation fix (interpreter)
- **[bug fix]** Mutating a **list field of a class instance in place** now persists:
  `this.list[i] = x`, `this.list[len(this.list)] = x` (append), `this.list.push(x)` / `.pop()`,
  2-D `this.grid[i][j] = x`, chained `add()` returning `this`, and grow-by-index. Root cause:
  `resolveLValue` didn't resolve a member base that is a **class instance** (only dicts, which
  happened to alias through a shared_ptr), and `evalIndexAssign` mutated a *copy* of the field and
  only wrote it back for plain-variable targets — so list-field writes were silently lost. Fix:
  `resolveLValue` returns a pointer to the instance's actual stored property; `evalIndexAssign`
  mutates through `resolveLValue`. Dict-field assignment and local-list assignment unchanged.
  Regression guard: `tests/lang_oop_test.b` (15/15, also covers the cross-instance `this` fix). Full
  regression green on default and Arrow builds. (This retires the "arctic sidesteps it with immutable
  rebuilds" caveat — the rebuilds still work, just no longer required.)

### 2026-09-07 — Performance push (combined effort): fast CSV · datetime/categorical · lazy · Arrow/Parquet

#### Phase A — two-pass fast CSV reader — ✅ `tests/arctic_csv2_test.b` 34/34
- **[feature]** New `readCsvFast` (dataframe_native.hpp): a single indexing scan records per-field
  byte spans (packed 12-byte `Field{off,len,flag}`, CSR by row — handles ragged rows for free) with
  **no per-field `std::string`**; a second pass infers each column's dtype and fills the typed buffer
  directly. Numbers parse straight from the span (`std::from_chars` for i64; a reused null-terminated
  buffer + `strtod` for f64); only genuine `utf8` cells allocate. Quotes/`""`/embedded newlines and
  `\r` are handled by lazily reprocessing just the "dirty" fields, so output is **byte-identical to
  the reference parser** on well-formed CSV (differential test).
- **[feature]** `read_csv(path, {columns:[...]})` — **projection pushdown**: unused columns are never
  materialized (the scan still runs, but inference/allocation is skipped). Also `{engine:"slow"}`
  selects the reference parser (kept for the differential test / fallback).
- **[patch]** `read_csv` now slurps the file in one sized `read()` instead of the char-by-char
  `istreambuf_iterator` form (that alone was ~600 ms of the old time).
- **Tests:** feature 34/34 (types/inference, quotes/escapes/embedded delim+newline, missing→null vs
  quoted-empty→"", header on/off, custom delim, ragged rows, projection, CRLF, empty/unterminated).
  **Stress:** 1M-row / 27 MB CSV read **894 ms (< 1 s target met)** — down from 1.5 s reference;
  indexing scan alone 306 ms (was 940 ms); projected 2-of-5-column read 698 ms. Full regression green
  (lang, scope, else-if, column, kernels, io, csv2, hash, crypto, uuid, random, orm, arctic pkg).

#### Phase B — datetime / date / categorical dtypes — ✅ `arctic_datetime_test.b` 24/24, `arctic_categorical_test.b` 18/18
- **[decision]** Implemented as **logical overlays on the physical i64 buffer** (a `Logical` tag +
  `cats` dictionary on `Column`), not new `DType` enum values. This keeps every existing numeric
  kernel (arith/compare/agg/sort/group/join) working unchanged — the right call for a production
  interpreter — while `elemToValue` and a handful of new builtins consult the overlay. DATETIME =
  epoch ms (UTC), DATE = days since epoch, CAT = dictionary codes.
- **[feature]** New builtins: `col_to_datetime`, `col_to_date`, `col_strftime` (%Y %y %m %d %H %M %S
  %j %%), `col_dt_year/month/day/hour/minute/second/weekday`, `col_to_categorical`, `col_categories`,
  `col_codes`. `col_dtype` now reports `date`/`datetime`/`cat`. Calendar math uses Hinnant's
  locale-free civil-date algorithms (no `struct tm`, correct across the range). ISO-8601 parser
  handles date-only and `YYYY-MM-DD[ T]HH:MM:SS[.fff][Z]`; unparseable cells → null.
- **[feature]** Comparisons "just work": a datetime/date column vs an ISO **string** parses the
  string to the same epoch unit; a **categorical** column compares as its category text. Value-
  preserving ops (`slice/filter/take/head/tail`, group-key reconstruction) carry the overlay via
  `carryMeta`, so filtered/grouped datetime & categorical columns still render correctly.
- **[patch]** `col_argsort` gained a typed key path (pre-materialized double keys / string keys) —
  one-branch hot comparator instead of per-compare dtype+null dispatch. Speeds `DataFrame.sort`
  across all dtypes.
- **Tests:** feature 24 + 18 (round-trip, components hand-verified incl. weekday, strftime, ISO
  comparison, chronological sort, leap day, null propagation, categories/codes, cat groupby with
  text-rendered keys, null-through-encoding). **Stress (3M rows):** `col_to_datetime` 231 ms,
  components 233 ms, sort 1.53 s (down from 2.5 s after the typed key path); categorical groupby
  correctness verified (grouped sums == full sum, 8 groups); 200k-column build/drop churn kept RSS
  flat (shared_ptr RAII frees). Full regression green.

#### Phase C — LazyFrame + query optimizer (pure Bantu) — ✅ `tests/arctic_lazy_test.b` 14/14
- **[feature]** `LazyFrame`/`LazyGroupBy` classes + `arctic.scan_csv(path)` and `DataFrame.lazy()`.
  A pipeline (`filter`/`query`, `select`, `with_column`, `sort`, `head`/`tail`, `groupby().agg()`) is
  recorded and nothing runs until `.collect()`. `.explain()` prints the optimized plan. All execution
  reuses the eager DataFrame ops.
- **[feature]** Optimizer, two rewrites: **predicate pushdown** hoists filters that reference only
  base columns ahead of sorts/derivations (filters on a `with_column` output correctly stay put);
  **projection pushdown** computes the minimal column set the pipeline needs and pushes it into the
  scan via `read_csv(columns=)`, so unused columns are never parsed (skipped when a `with_column`
  could touch any column).
- **[bug fix] (interpreter, OOP `this` binding)** Found and fixed a latent, language-wide bug:
  `evalCall` overwrote a **bound method's** `this` with the *caller's* `this` on every call, so any
  object calling another object's method from inside a method ran with the wrong receiver
  (`a.method()` where the body calls `b.other()` saw `a`, not `b`). The existing suites never hit it
  (they only call methods on `this` or free functions); the LazyFrame executor did. Fix: inherit the
  caller's `this` only when the callee's closure has no real instance `this` of its own — free
  functions still get dynamic `this` (backward compatible), bound methods keep their receiver.
  Verified with inheritance/`super`/cross-instance tests; **full regression green incl. orm (OOP)**.
- **Tests:** feature 14/14 (lazy == eager for filter/select/sort, groupby.agg, with_column+derived
  filter; predicate & projection pushdown via `explain`; scan projection returns only needed cols).
  **Stress (1M rows):** lazy `scan_csv → filter → groupby.agg` **903 ms vs 1173 ms eager**
  (projection reads 2 of 5 columns), identical grouped totals. Full regression green.

#### Phase D — Parquet + Feather/Arrow-IPC (native, feature-gated) — ✅ `tests/arctic_arrow_test.b` 19/19
- **[feature]** New `bantu-src/compiler/src/dataframe_arrow.hpp` (entirely under `#ifdef BANTU_ARROW`):
  converts arctic `Column` ↔ `arrow::Array` (layouts map ~1:1) for all dtypes incl. datetime
  (timestamp[ms,UTC]), date (date32), categorical (written as category text), null bitmaps both ways.
- **[feature]** Builtins (compiled only with Arrow): `read_parquet(path,{columns?})`,
  `write_parquet(frame|names,cols,path,{compression?})` (snappy/zstd/gzip/none),
  `read_feather(path,{columns?})`, `write_feather(...)`. `read_parquet`/`read_feather` honor
  `columns` (projection pushed to the reader). `has_native("arrow")` is true only in this build, so
  the arctic package feature-detects and errors clearly on a default binary.
- **[build]** `build-mac.sh` gains an opt-in `BANTU_ARROW=1` branch (mirrors `BANTU_SODIUM`): links
  libarrow/libparquet via pkg-config/brew and bumps that build to **C++20** (Arrow 14+ headers need
  `std::span`/`popcount`; last `-std` wins so the **default build stays C++17 with no new dep**).
- **Tests:** feature 19/19 (round-trip all dtypes + nulls for Parquet & Feather; datetime/date/leap
  day; categorical→text; column projection). **Stress (1M rows):** write parquet 734 ms; read full
  **471 ms vs 1081 ms CSV** (2.3×); projected 2-of-5-col read **245 ms**; file **11 MB vs 27 MB CSV**
  (snappy); parquet sum == csv sum. Default build compiles unchanged with `has_native("arrow")`=false;
  full regression green on both builds.
- **[deferred]** Predicate/row-group pushdown into the Parquet reader (skip row-groups by min/max
  stats) — the reader currently does projection; the lazy optimizer already computes predicates, so
  this is a wiring follow-up. Values are always correct (filter applied after read).

### 2026-08-28 — arctic package (pure Bantu) + two interpreter fixes
- **[feature]** `arctic/arctic.b` — the DataFrame library, written in **pure Bantu** on the `col_*`
  atoms: `Series` and `DataFrame` classes with method chaining, a `GroupBy` with `agg`, a
  plain-English `query()` filter DSL (tokenizer + evaluator in Bantu), `join` (inner/left/right/
  outer), `sort`, `select/drop/rename/with_column`, `describe`, `to_csv`, and `show()` pretty-print.
  Readers `read_csv`/`read_sqlite`/`dataframe`/`series`. Docs: `docs/arctic.md`. Package published to
  the local registry. Tests: `arctic/arctic_test.b` 37/37; end-to-end on a 1M-row CSV
  (load ~1.3 s, grouped agg + sort sub-second).
- **[bug fix]** `col_group_agg` segfaulted when the value column was `utf8` (e.g. `groupby.count`
  on a text column) — it read the value as numeric, indexing an empty buffer. Now aggregates each
  group via a gathered sub-column through the generic `aggOp`, correct for every dtype/op
  (count/nunique/any/all on utf8; all ops on numeric). group_agg 5M/1000-groups ~540 ms.
- **[patch]** The informational `[class] Defined:` log now respects quiet mode (`-q`), like
  `[INCLUDE]`, so libraries that define classes don't spam stdout on include.

### 2026-08-28 — Phase 5: consolidate & document (FOUNDATIONS COMPLETE)
- **[feature]** `docs/arctic-foundations.md` — a newcomer-readable reference for every `col_*` builtin
  with tiny examples and an end-to-end walkthrough (verified to run). The `else if` note is included.
- Final full regression green across all suites; lint clean on the new `.b` tests. The native
  foundations (first-class `else if` + `col` primitive + kernels + typed I/O) are ready; the `arctic`
  package can now be written in pure Bantu on top of them.

### 2026-08-28 — Phase 4: fast typed I/O
- **[feature]** `read_csv(path, options?)` — native RFC-4180 CSV parser (quotes, `""` escapes,
  delimiters/newlines in quotes, CRLF) with type inference (i64 → f64 → bool → utf8) that builds
  typed columns directly (no giant Bantu list). Returns `{names, cols, shape}`; blank lines skipped;
  ragged rows → nulls. `write_csv(frame, path)` / `write_csv(names, cols, path)` with proper
  escaping. `read_sqlite(path, query)` runs a query through the linked SQLite and infers typed
  columns per cell.
- **Tests:** feature `tests/arctic_io_test.b` 27/27 (type inference, quoted commas, nulls,
  round-trip, SQLite). Stress: 1M-row / 14 MB CSV read ~1.5 s, then group_agg ~130–200 ms, sum ~10 ms;
  malformed/ragged/blank lines handled without crashing. Full regression green.
- **[patch]** Faster inference (short-circuits once a narrower dtype is ruled out) and no per-column
  string copy. Noted future optimization: a two-pass parser to get 1M-row read under 1 s.

### 2026-08-28 — Phase 3: vectorized column kernels
- **[feature]** Full `col_*` kernel set (`dataframe_native.hpp` + `evaluator.hpp`): arithmetic
  (`col_add/sub/mul/div/mod/pow/neg/abs`, i64-exact for +,-,*), comparisons → boolean mask
  (`col_gt/ge/lt/le/eq/ne`, numeric + utf8 lexicographic), mask logic (`col_and/or/not`), the
  vectorized conditional `col_where(mask,a,b)` and `col_case([mask,val,...],default)` (if/elif/else
  over a column), selection (`col_filter/take/head/tail`), aggregations
  (`col_sum/mean/min/max/std/var/median/count/nunique/any/all` — Welford variance ddof=1,
  nth_element median, int-exact i64 sum), `col_argsort` (stable, nulls last), and hash group/join
  (`col_group_ids`, `col_group_agg`, `col_join`). Every operand accepts a column OR a scalar. Best
  DS&A per A8: contiguous single-pass loops, unordered_map grouping, hash-join on the smaller side,
  and typed single-key fast paths (no per-row string). `has_native("col")` now true.
- **Tests:** feature `tests/arctic_kernels_test.b` 44/44; differential vs a naive pure-Bantu
  reference 5/5. Stress on 5M rows: sum 70ms / mean 68ms / std 63ms / mul 111ms / gt 76ms /
  filter 201ms / median 190ms; argsort(1M) 73ms; group_agg(1000 groups over 5M) 474ms (was 1230ms
  before the typed fast path); join(1M×1M) 782ms (was 1793ms) — all sub-second. Full regression green.

### 2026-08-28 — Phase 2: native-handle Value + Column primitive
- **[feature]** New `Value` type `NATIVE_HANDLE` (`types.hpp`): an opaque native object held by a
  `std::shared_ptr<void>` with a string type tag (reusing `stringVal`). C++ RAII frees it when the
  last Bantu reference drops — automatic lifetime, no manual free, no leak. Switch arms added
  (toString `"<tag>"`, truthy = live handle, equality = identity, `type()` → tag, JSON → null).
- **[feature]** Native `Column` primitive (`dataframe_native.hpp`) — typed contiguous buffer
  (f64/i64/bool/utf8) + per-element null mask; i64 stored true 64-bit. Builtins: `col(list,dtype)`,
  `col_len`, `col_dtype`, `col_get`, `col_to_list`, `col_slice`, `col_cast`, `col_is_null`,
  `col_null_count`, `col_fill_null`. Friendly errors (bad dtype, out-of-range). Immutable ops.
- **Tests:** feature `tests/arctic_column_test.b` 31/31. Stress: **500k columns created/dropped in a
  loop → max RSS 3.9 MB (flat, no leak)** proving the shared_ptr handle frees; 3M-element column;
  empty/single boundaries. Full regression green (incl. lang/scope/else-if/crypto/uuid/random/orm).

### 2026-08-28 — Phase 1: first-class `else if`
- **[feature]** `if (a) {} else if (b) {} else {}` now parses directly. Previously `else if` did not
  parse (`parseBlock` requires `{` right after `else`), forcing `else { if (b) {} }`. Fix in
  `parser.hpp` `parseIf`: when `if` follows `else`, parse it as a nested if that becomes the else
  branch (recurses for any chain depth). Backward compatible; no AST/evaluator change (`evalIf`
  already recurses `elseBody`).
- **Tests:** feature `tests/lang_elseif_test.b` 17/17 (arm selection, no side effects on untaken
  arms, no-else fallthrough, loops/functions, boundaries). Stress: generated 499-arm chain called
  500× in a loop + deep nesting, all correct in ~2.3s. Full regression green
  (lang 28/scope 8/hash 25/crypto 31/uuid 17/random 33/orm 61).

### 2026-08-28 — Phase 0
- **[patch]** Created `arctic-suite/ROADMAP.md`, `DECISIONS.md`, `CHANGELOG.md`. Seeded the roadmap
  with phases 1–5 (first-class `else if`; native-handle `Value` + `Column`; vectorized `col_*`
  kernels incl. `col_where`/`col_case`; fast typed I/O; consolidate & document). Each phase gated by
  feature + stress tests + full regression.
