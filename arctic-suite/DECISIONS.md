# arctic Foundations — Design Decisions

Rationale for the choices behind the native foundations, so they aren't re-litigated. Each entry:
**decision · why · alternatives rejected · implication.**

---

### A1 — Native atoms + pure-Bantu library (not a native dataframe engine)
**Decision:** Add native *primitives* (a `Column` type, `col_*` kernels, typed I/O); write `arctic`
(frames, API, expressions) in pure Bantu on top.
**Why:** mirrors the crypto suite — keeps the library auditable and genuinely "written in Bantu"
while the heavy vectorized compute is native and fast. It's also what the user asked: fix what's
missing so we can write it in Bantu.
**Rejected:** a full C++ dataframe engine (not "in Bantu," larger trusted surface); pure Bantu
(~KB/s — a toy).
**Implication:** the native surface is small and general-purpose; arctic is portable Bantu code.

### A2 — A frame is NOT native; it's a pure-Bantu structure
**Decision:** Represent a DataFrame as `{"names": [...ordered...], "cols": {name: column}}` in Bantu.
**Why:** a frame is just named columns; keeping it in Bantu maximizes what's written in Bantu and
keeps the native layer to columns only. `ObjectMap` is an `unordered_map`, so column order is carried
in the `names` list.
**Rejected:** a native Frame type (more native surface for no compute benefit).

### A3 — Automatic lifetime via a `NATIVE_HANDLE` Value variant (shared_ptr)
**Decision:** Add `Type::NATIVE_HANDLE` to `Value` holding `shared_ptr<void>` + a `handleType` tag;
columns/masks/index-vectors are these handles.
**Why:** a query chain creates thousands of intermediate columns; the existing native-resource
pattern (file ids in a static table, `evaluator.hpp:2806`) never frees them — a leak. A `shared_ptr`
inside the Value frees the buffer by RAII when the last Bantu reference drops: no `free()`, no leak.
**Rejected:** id-in-static-table (leaks); a full GC (huge, out of scope).
**Implication:** one contained core-`Value` change (new opaque type existing code never produces);
switch sites get simple default arms. This is the enabling foundation for any native object.

### A4 — dtypes f64 / i64 / bool / utf8; i64 stored true 64-bit
**Decision:** Columns store real typed buffers; i64 is a true 64-bit integer natively, only converted
to a Bantu number (float64) when materialized to Bantu (with the documented 2^53 caveat).
**Why:** exactness is preserved inside the engine (sums, ids, joins) even though Bantu numbers are
float64. **Rejected:** storing everything as double (loses integer exactness for large ids/counts).

### A5 — `col_` naming (user-approved)
**Decision:** All column atoms use a lowercase `col_` `verb_noun` prefix; `col(...)` constructs.
**Why:** self-documenting (a column is spreadsheet-intuitive), no clashes with existing builtins
(`max`/`min`/`sum`/`len`), easy to grep. **Rejected:** `arr_` (less intuitive to non-programmers);
no-prefix short verbs (collisions/ambiguity).

### A6 — First-class `else if` via the parser (not an AST/evaluator refactor)
**Decision:** In `parseIf`, after `else`, if the next token is `IF`, parse a nested `if` as the else
branch. `else if` becomes real syntax.
**Why:** today `else if` doesn't parse (parseBlock requires `{` right after `else`), forcing nested
`else { if }`. The parser tweak is minimal, backward-compatible, and needs no AST/evaluator change
(`evalIf` already recurses `elseBody`). **Rejected (for now):** a multi-branch `IfNode` with an arm
list — more churn for no user-visible or perf gain; kept optional.
**Implication:** cleaner Bantu everywhere; the columnar mirror is `col_where`/`col_case`.

### A8 — Best-in-class data structures & algorithms for the kernels (user directive)
**Decision:** Kernels operate directly on the contiguous typed buffers (no per-element `Value`
boxing) with the right algorithm for each: single-pass O(n) arithmetic/compare/filter/where;
Welford's method for numerically-stable variance/std (sample, ddof=1, to match pandas);
`std::nth_element` O(n) median; `unordered_map` hash groupby O(n) (typed single-key fast paths +
combined-key path for multi-key); hash-join building the table on the **smaller** side O(n+m);
`std::sort` on an index array for argsort (nulls sorted last). Integer-exact path for i64⊕i64
add/sub/mul (result i64); division/pow and any f64 operand → f64.
**Why:** the user asked for a fast, performant design; columnar contiguous access + correct
asymptotics is what makes it competitive with pandas/polars. **Implication:** kernels are the
performance-critical code and carry the scale stress tests.

### A7 — Feature + stress tests gate every phase
**Decision:** No phase advances until feature tests AND stress tests pass and full regression is
green; results logged in CHANGELOG.
**Why:** stress testing is what surfaced the function-local scoping bug in the crypto work; scale +
memory-churn + fuzz + malformed inputs catch what unit tests miss. **Implication:** slower per phase,
but the foundations are trustworthy before `arctic` is built on them.

---

## Performance push (2026-09-07)

### B1 — Two-pass CSV: index spans, then materialize
**Decision:** The fast reader scans once to record per-field byte spans (packed `Field{off,len,flag}`,
CSR by row), then builds each column directly — numbers parsed from the span (`from_chars` for i64, a
reused buffer + `strtod` for f64), only utf8 cells allocate. **Why:** the old reader's per-field
`std::string` allocation dominated; eliminating it (plus a one-shot file slurp) took 1M rows from
~1.5 s to **0.89 s**. Quoted/`""`/`\r` fields are "dirty" and reprocessed lazily so output is
byte-identical to the reference parser (differential-tested).

### B2 — datetime/date/categorical as LOGICAL OVERLAYS, not new DType values
**Decision:** Add a `Logical{NONE,DATE,DATETIME,CAT}` tag + `cats` dictionary to `Column`; physical
storage stays `i64` (epoch ms / days / dict codes). **Why:** adding enum values would force new arms
in every `switch(dtype)` across the kernels — high risk on a production interpreter. Overlays keep
every numeric kernel (arith/compare/agg/sort/group/join) working unchanged; only `elemToValue`, a few
overlay-aware spots (compare vs ISO string / category text), and the new builtins consult the tag.
Value-preserving ops carry the overlay via `carryMeta`. **Implication:** `col_min/max` on a datetime
returns raw epoch ms (documented); use `col_strftime` to render.

### B3 — Fixed a language-wide OOP `this`-binding bug (interpreter)
**Decision:** In `evalCall`, stop overwriting a **bound method's** `this` with the caller's; inherit
the caller's `this` only when the callee's own closure has no instance `this`. **Why:** the old code
made `a.method()` run any nested `b.other()` with `a` as `this` — any object calling another object's
method from inside a method was broken. The LazyFrame executor needs cross-instance calls; the fix is
minimal and backward-compatible (free functions still get dynamic `this`). Verified against
inheritance/`super`/orm; full regression green. (Pre-existing list-field index-mutation quirk is
unrelated and left as-is.)

### B4 — Lazy execution routed through free functions
**Decision:** `LazyFrame.collect()` and `LazyGroupBy.agg()` delegate the actual DataFrame-method calls
to free functions (`_runLazy`, `_lazyAppend`). **Why:** even with B3, keeping cross-instance calls in
free functions is the clean seam; the optimizer does predicate + projection pushdown, then reuses the
eager ops so there is exactly one implementation of each operation.

### B5 — Parquet/Arrow native + feature-gated (BANTU_ARROW), C++20 for that build only
**Decision:** Parquet/Feather live in `dataframe_arrow.hpp` under `#ifdef BANTU_ARROW`, linked opt-in
(mirrors `BANTU_SODIUM`); that build is bumped to C++20 (Arrow 14+ headers need `std::span`), the
default stays C++17 with no new dependency. **Why:** Parquet (Thrift + encodings + codecs) and Arrow
IPC (flatbuffers) are not sensibly implementable in an interpreter, and the dependency is heavy — so
it must be strictly opt-in. arctic feature-detects via `has_native("arrow")`. Categoricals are written
as their category text (round-trip values; re-encode with `col_to_categorical`).
