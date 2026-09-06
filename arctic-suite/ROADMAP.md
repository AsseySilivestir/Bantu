# arctic Foundations — Roadmap

Native language + interpreter foundations that let a pandas/polars-style data-science library
(`arctic`) be written in **pure Bantu**. The *atoms* (a `Column` primitive, vectorized `col_*`
kernels, fast typed I/O) are native; everything above them — frames, the API, expressions — is Bantu.
This roadmap tracks ONLY the foundations; the `arctic` package itself is a separate later effort.

Status legend: `[ ]` todo · `[~]` in progress · `[x]` done (feature **and** stress tests green)

**Every phase is gated:** it is not done until its feature tests AND stress tests pass and the full
existing regression (`lang/scope/crypto/uuid/random/orm`) stays green. Results recorded in CHANGELOG.

---

## Phase 0 — Tracking docs
- [x] `arctic-suite/ROADMAP.md`, `CHANGELOG.md`, `DECISIONS.md` created

## Phase 1 — Language core: first-class `else if`
| Item | How | Feature test | Stress test | Status |
|---|---|---|---|---|
| `else if` parses natively | `parseIf`: if token after `else` is `IF`, parse a nested `if` as the else branch (no AST/evaluator change; backward compatible) | ✅ `tests/lang_elseif_test.b` 17/17 | ✅ 499-arm chain ×500 + deep nesting, ~2.3s; regression green | [x] |

## Phase 2 — Native-handle `Value` + `Column` primitive
| Item | How | Feature test | Stress test | Status |
|---|---|---|---|---|
| `NATIVE_HANDLE` Value type | `types.hpp`: `shared_ptr<void>` + tag; RAII frees; switch arms (toString/truthy/eq/type/json) | ✅ `type($c)=="column"` | ✅ (via leak test below) | [x] |
| `Column` primitive (f64/i64/bool/utf8 + null mask) | `dataframe_native.hpp`; builtins `col`, `col_len`, `col_dtype`, `col_get`, `col_to_list`, `col_slice`, `col_cast`, `col_is_null`, `col_null_count`, `col_fill_null` | ✅ `tests/arctic_column_test.b` 31/31 | ✅ 500k columns → RSS flat at 3.9 MB (no leak); 3M-elem column; empty/single boundaries; regression green | [x] |

## Phase 3 — Vectorized kernels (incl. conditional) — ✅ `tests/arctic_kernels_test.b` 44/44
All kernels done. Feature 44/44 + differential vs pure-Bantu 5/5. Scale (5M rows): sum 70ms,
mean 68ms, std 63ms, mul 111ms, gt 76ms, filter 201ms, median 190ms, argsort(1M) 73ms; typed
fast-path group_agg (1000 groups) 474ms, join(1M×1M) 782ms — all sub-second.
| Group | Builtins | Status |
|---|---|---|
| Arithmetic | `col_add/sub/mul/div/mod/pow/neg/abs` (i64-exact +,-,*; f64 otherwise) | [x] |
| Compare→mask / mask logic | `col_gt/ge/lt/le/eq/ne` (numeric + utf8 lexicographic), `col_and/or/not` | [x] |
| Conditional | `col_where(mask,a,b)`, `col_case([mask,val,...],default)` | [x] |
| Select | `col_filter`, `col_take`, `col_head`, `col_tail` | [x] |
| Aggregate | `col_sum/mean/min/max/std/var/median/count/nunique/any/all` (Welford; nth_element) | [x] |
| Order | `col_argsort(c, desc)` → indices (stable, nulls last) | [x] |
| Group/join | `col_group_ids`, `col_group_agg`, `col_join(how)` (typed single-key fast path) | [x] |

## Phase 4 — Fast typed I/O — ✅ `tests/arctic_io_test.b` 27/27
| Item | How | Status |
|---|---|---|
| `read_csv` / `write_csv` | RFC-4180 parser (quotes/escapes/CRLF), type inference (i64→f64→bool→utf8), builds columns directly; returns `{names, cols, shape}` | [x] |
| `read_sqlite(path, query)` | linked SQLite → typed columns (per-cell type) | [x] |
| JSON | deferred to the arctic package (pure Bantu via existing `json` → `col`) | [~] |

Stress: 1M-row / 14 MB CSV read ~1.5 s (stable), then `col_group_agg` ~130–200 ms, `col_sum` ~10 ms;
ragged/blank/unterminated-quote lines handled without crashing; write→read round-trip preserves
quoted commas and nulls; SQLite result → typed columns. **Future opt:** two-pass parser (infer, then
write straight into typed buffers) to skip string storage for numeric columns and get read under 1 s.

## Phase 5 — Consolidate & document — ✅
- [x] `docs/arctic-foundations.md` (every builtin: one-line doc + tiny example, newcomer-readable);
  end-to-end example verified to run
- [x] final full regression green; stress summary in CHANGELOG; lint clean on all new `.b`
- [x] **FOUNDATIONS COMPLETE** — the `col_*` atoms + first-class `else if` are ready; the `arctic`
  package can now be written in pure Bantu on top of them (next effort)

## Performance push (2026-09-07) — one combined effort ✅
| Phase | What | Tests | Status |
|---|---|---|---|
| A — fast CSV | two-pass reader (index spans → typed buffers), `columns=` projection, one-shot slurp | `arctic_csv2_test.b` 34/34 | [x] |
| B — datetime/date/categorical | logical overlays on i64 + `col_to_datetime/to_date/strftime/dt_*`, `col_to_categorical/categories/codes`; overlay-aware compare/sort/group | `arctic_datetime_test.b` 24/24, `arctic_categorical_test.b` 18/18 | [x] |
| C — lazy frame + optimizer | `LazyFrame`/`LazyGroupBy`, `scan_csv`/`scan_parquet`/`.lazy()`, predicate + projection pushdown, `explain()` | `arctic_lazy_test.b` 14/14 | [x] |
| C — interpreter bug fix | `this` no longer leaks from a bound method's caller (evalCall) — language-wide OOP correctness | full regression incl. orm | [x] |
| D — Parquet + Feather | native `dataframe_arrow.hpp` (opt-in `BANTU_ARROW=1`), `read/write_parquet`, `read/write_feather`, projection | `arctic_arrow_test.b` 19/19 | [x] |
| E — package + docs | Series datetime/cat helpers, `to_parquet/to_feather`, `scan_parquet`, `parse_dates`; docs; Windows build branch | `arctic/arctic_test.b` 48/48 | [x] |

Perf: 1M-row CSV read **894 ms** (< 1 s); 3M-row datetime parse 231 ms + components 233 ms; lazy
projection 903 ms vs 1173 ms eager; Parquet read **471 ms vs 1081 ms CSV** (245 ms projected),
file 11 MB vs 27 MB. Full regression green on both default and Arrow builds.

## Deferred (next efforts)
- Predicate/row-group pushdown into the Parquet reader (skip row-groups by min/max stats).
- Window functions, `pivot`, `unique`; timezone database (currently UTC-only); Arrow Flight;
  streaming/out-of-core execution.
