# arctic — DataFrames for Bantu

The power of pandas + polars, made simple. `arctic` is written in **pure Bantu** on the native
column primitives (see [arctic-foundations.md](arctic-foundations.md)), so it is fast on millions of
rows while staying easy to read and use.

```
include "./arctic.b" as arctic;

$df  = arctic.read_csv("sales.csv");
print($df.head().show());

$big = $df.query("amount > 1000 and region == 'EU'")
          .select(["region", "amount"])
          .sort("amount", true);

$by  = $df.groupby("region").agg([["amount", "sum", "total"], ["amount", "mean", "avg"]]);
print($by.show());
```

Two ways to work, both easy:
- **Plain-English filters:** `$df.query("amount > 1000 and region == 'EU'")`
- **Composable expressions:** `$df.get("amount").gt(1000)` → a Series you combine and reuse

Needs an interpreter with the native `col` primitives (`has_native("col")`).

---

## Loading & creating
| Call | Returns |
|---|---|
| `arctic.read_csv(path, options?)` | DataFrame (types inferred). `options` = `{"delim": ",", "header": true, "columns": [...], "parse_dates": [...], "engine": "slow"}` |
| `arctic.read_sqlite(path, query)` | DataFrame from a SQL query |
| `arctic.read_parquet(path, options?)` | DataFrame from Parquet (`{"columns": [...]}` to project). Needs an Arrow build |
| `arctic.read_feather(path, options?)` | DataFrame from Arrow IPC/Feather. Needs an Arrow build |
| `arctic.scan_csv(path, options?)` | a **LazyFrame** (deferred; projection pushed into the scan) |
| `arctic.scan_parquet(path)` | a **LazyFrame** over Parquet (projection pushed to the reader) |
| `arctic.read_json(pathOrText)` | DataFrame from JSON — a list of row objects, or `{column: list}` |
| `arctic.dataframe({name: list, ...}, dtypes?)` | DataFrame from Bantu lists (dtype inferred, or forced via `dtypes`) |
| `arctic.series(name, list, dtype?)` | a single Series |

`read_csv` reads a million rows in **under a second** (native two-pass parser). `columns` reads only
those columns; `parse_dates` converts the named columns to datetime; `engine:"slow"` selects the
reference parser (for debugging).

## DataFrame
| Method | Does |
|---|---|
| `.shape()` / `.height()` / `.width()` | `[rows, cols]` / row count / col count |
| `.columns()` / `.has(name)` | column names / membership |
| `.get(name)` | a column as a **Series** |
| `.select([names])` / `.drop([names])` | keep / remove columns |
| `.rename({old: new})` | rename columns |
| `.with_column(name, seriesOrValue)` | add or replace a column |
| `.filter(mask)` | keep rows where a boolean Series is true |
| `.query(text)` | filter with the string DSL (below) |
| `.sort(name, descending)` | sort all columns by one |
| `.head(n)` / `.tail(n)` | first / last n rows (default 5) |
| `.groupby(keys)` | a GroupBy (keys = name or list of names) |
| `.join(other, on, how)` | join on a shared key; `how` ∈ inner/left/right/outer |
| `.describe()` | count/mean/std/min/median/max per numeric column |
| `.sort([a, b], desc)` | sort by several columns (stable, left-most wins) |
| `.unique(subset?)` / `.drop_duplicates(subset?)` | keep the first row of each distinct key |
| `.drop_nulls(subset?)` / `.fill_null(v, subset?)` | remove / replace missing values |
| `.null_count()` | nulls per column, as a dict |
| `.slice(offset, len)` / `.reverse()` / `.is_empty()` | row shaping |
| `.n_largest(n, col)` / `.n_smallest(n, col)` / `.sample(n)` | top-n and random rows |
| `.with_columns([[name, series], ...])` | add/replace several columns at once |
| `.cast({name: dtype})` | convert column types |
| `.row(i)` / `.iter_rows()` | one row / all rows as dicts |
| `.value_counts(col)` / `.quantile(q)` / `.corr(a, b)` | frequency table, per-column quantiles, correlation |
| `.pivot(index, columns, values, op?)` | long → wide (default op `"sum"`) |
| `.melt(idVars, valueVars?)` | wide → long, producing `variable` / `value` |
| `.concat(other)` / `.vstack(other)` / `.hstack(other)` | stack rows / place columns side by side |
| `.to_csv(path)` | write to CSV |
| `.to_json(path?)` | write JSON (a list of row objects); returns the text when `path` is null |
| `.to_parquet(path)` / `.to_feather(path)` | write Parquet / Arrow-IPC (needs an Arrow build) |
| `.lazy()` | start a lazy pipeline over this frame |
| `.show(n?)` | a pretty ASCII table (string) |

Every transform returns a **new** DataFrame (immutable) — safe to chain.

```
$clean = $df.drop_nulls(["amount"]).unique(["id"]).fill_null(0, null);
$top   = $df.n_largest(10, "amount");
$wide  = $sales.pivot("region", "year", "amount", "sum");
$long  = $wide.melt(["region"]);
$all   = $jan.concat($feb);
```

## LazyFrame — deferred, optimized pipelines
`scan_csv`/`scan_parquet`/`.lazy()` return a **LazyFrame**. You build the pipeline with the same
verbs; nothing runs until `.collect()`. Before running, the optimizer applies:
- **projection pushdown** — only the columns the pipeline actually needs are read off disk;
- **predicate pushdown** — filters on base columns are hoisted ahead of sorts/derivations.

```
$out = arctic.scan_csv("sales.csv")
             .filter("amount > 1000 and region == 'EU'")
             .groupby("region").agg([["amount", "sum", "total"]])
             .collect();               // reads only region + amount

print(arctic.scan_csv("sales.csv").filter("amount > 1000").select(["region","amount"]).explain());
```
Verbs: `filter`/`query`, `select`, `drop`, `rename`, `with_column(name, fn)`, `sort`, `head`/`tail`/
`limit`, `slice`, `reverse`, `unique`, `drop_nulls`, `fill_null`, `join`, `groupby(keys).agg(specs)`,
then `collect()`; `explain()` prints the optimized plan.

**Filters are only hoisted when it is safe.** Ops that change row counts (`head`/`tail`/`slice`),
pick first-wins rows (`unique`), rewrite values (`fill_null`) or change the column namespace
(`rename`/`join`/`with_column`/`groupby`) act as barriers — a filter written after one of them stays
there, so `.head(2).filter(...)` means exactly what it says.

## Datetime, date & categorical
```
$ts = $df.get("created").to_datetime();     // parses ISO-8601 → datetime
$df2 = $df.with_column("year", $df.get("created").to_datetime().year());
$region = $df.get("region").to_categorical(); // compact + fast group/join
```
Series helpers: `to_datetime` · `to_date` · `year month day hour minute second weekday` (weekday
0=Sunday) · `strftime(fmt)` (`%Y %y %m %d %H %M %S %j %%`) · `to_categorical` · `categories` ·
`codes`. Datetimes compare against ISO strings directly (`col created after '2020-01-01'` via
`$df.get("created").to_datetime().gt("2020-01-01")`); categoricals compare as their text.

## Series
Arithmetic and comparisons accept another Series **or** a scalar, and return a Series.

| Group | Methods |
|---|---|
| arithmetic | `add sub mul div mod pow neg abs` |
| compare | `gt ge lt le eq ne` · `and_ or_ not_` (trailing `_`: `and/or/not/any` are reserved words) |
| summaries | `sum mean min max std var median count nunique any_ all_ product first last quantile` |
| nulls | `is_null is_not_null null_count fill_null drop_nulls` |
| set / membership | `unique value_counts mode is_in between` |
| window / cumulative | `cumsum cumprod cummax cummin shift diff pct_change rank` |
| shaping | `clip round cast sort argsort take filter head tail slice reverse n_largest n_smallest` |
| text | `upper lower strip str_len contains starts_with ends_with replace substr` |
| datetime / categorical | `to_datetime to_date year month day hour minute second weekday strftime to_categorical categories codes` |
| escape hatch | `map(fn)` / `apply(fn)` — runs a Bantu function per element (interpreted, so not for the hot path) |
| misc | `to_list get len dtype rename alias` |

```
$s.value_counts();                 // DataFrame of [value, count], most frequent first
$s.is_in(["EU", "US"]);            // boolean Series
$s.cumsum();  $s.shift(1);  $s.rank(false);
$s.clip(0, 100).round(2);
$df.get("email").lower().ends_with("@example.com");
```

```
$amount = $df.get("amount");
$net    = $amount.mul(1.1);          // a new Series
$mask   = $amount.gt(1000);          // boolean Series
$rich   = $df.filter($mask);
```

## GroupBy
```
$g = $df.groupby("region").agg([
    ["amount", "sum",  "total"],     // [column, op, outputName]
    ["amount", "mean", "avg"]
]);
// convenience: $df.groupby("region").sum("amount")
```
`op` ∈ `sum mean min max std var median count nunique any all`. Group by several columns with a list:
`$df.groupby(["region", "year"])`.

Convenience methods mirror the ops — `sum mean min max count std var median nunique any_ all_` — plus
`.size()` (rows per group) and `.stats(col)` (count/sum/mean/min/max/std in one go).

## The `query()` filter language
`predicate (and | or predicate)*`, evaluated left to right.
- predicate: `column OP value`
- OP: `==  !=  >  >=  <  <=`
- value: a number, `'text'` or `"text"`, `true`, `false`, or `null` (`col == null` / `col != null`)

```
$df.query("age >= 18 and region == 'EU'");
$df.query("score > 90 or flagged == true");
$df.query("email != null");
```
(Left-to-right; there is no `and`/`or` precedence and no parentheses yet — chain `.filter()` calls
for complex logic.)

## Notes
- **Immutable & chainable:** operations never modify their input.
- **Column order** from `arctic.dataframe({...})` follows dict iteration; use `.select([...])` to fix
  an order. `read_csv`/`read_sqlite` preserve source order.
- **Floating point:** decimals are IEEE-754; compare with a tolerance rather than `==` where needed.
- **Performance:** loads a 1M-row CSV in **under a second** (native two-pass parser) and does grouped
  aggregations on millions of rows in well under a second, because the heavy lifting runs in the
  native `col_*` kernels. Parquet reads a 1M-row file in ~0.5s (~half with column projection) and
  stores it in ~40% of the CSV size.
- **Arrow builds:** `read_parquet`/`write_parquet`/`read_feather`/`write_feather` and
  `scan_parquet` need an interpreter built with `BANTU_ARROW=1`; they error clearly otherwise. Check
  with `has_native("arrow")`.
