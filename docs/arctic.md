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
| `arctic.read_csv(path, options?)` | DataFrame (types inferred). `options` = `{"delim": ",", "header": true}` |
| `arctic.read_sqlite(path, query)` | DataFrame from a SQL query |
| `arctic.dataframe({name: list, ...}, dtypes?)` | DataFrame from Bantu lists (dtype inferred, or forced via `dtypes`) |
| `arctic.series(name, list, dtype?)` | a single Series |

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
| `.to_csv(path)` | write to CSV |
| `.show(n?)` | a pretty ASCII table (string) |

Every transform returns a **new** DataFrame (immutable) — safe to chain.

## Series
Arithmetic and comparisons accept another Series **or** a scalar, and return a Series:
`add sub mul div mod pow neg abs` · `gt ge lt le eq ne` · `and_ or_ not_` (trailing `_` because
`and/or/not/any` are reserved words) · summaries `sum mean min max std var median count nunique
any_ all_` · `is_null fill_null cast sort argsort take filter head tail to_list get len dtype
alias`.

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
- **Performance:** loads a 1M-row CSV in ~1.5s and does grouped aggregations on millions of rows in
  well under a second, because the heavy lifting runs in the native `col_*` kernels.
