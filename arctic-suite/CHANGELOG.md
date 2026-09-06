# arctic Foundations — Changelog

Initiative-scoped, dated log of the native foundations. Searchable tags: `[feature]` / `[bug fix]` /
`[patch]`. The top-level `CHANGELOG.md` gets summarized entries when the foundations ship; this log
tracks granular per-phase progress, including feature- and stress-test results.

## [Unreleased]

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
