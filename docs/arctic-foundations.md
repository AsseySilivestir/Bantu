# arctic foundations — native columns for Bantu

These are the low-level building blocks (the *atoms*) that make fast, pandas/polars-style data work
possible in Bantu. They are simple on purpose: if you can use a spreadsheet column, you can use
these. The full `arctic` package (DataFrames, method chaining, a `query()` language) will be written
in pure Bantu **on top of** these — but they are usable directly today.

Everything here ships in the interpreter (no install, no dependency). Check availability with
`has_native("col")`.

---

## The idea in 30 seconds
A **column** is a typed list of values stored efficiently in native memory — millions of numbers cost
a fraction of what a normal Bantu list would, and operations run on the whole column at once (fast),
instead of one element at a time (slow).

```
$c = col([1, 2, 3, 4], "f64");     // make a column
$big = col_filter($c, col_gt($c, 2));   // keep values > 2  ->  [3, 4]
print(col_mean($c));                // 2.5
```

A **DataFrame** is just an ordinary Bantu dict of columns — no special type needed:
```
$df = read_csv("sales.csv");        // { "names": [...], "cols": {name: column}, "shape": [rows, cols] }
print($df.shape);                   // e.g. [12000, 4]
print(col_sum($df.cols.amount));    // total of the "amount" column
```

## Types (dtypes)
`"f64"` decimals · `"i64"` whole numbers (exact) · `"bool"` true/false · `"utf8"` text.
Any element can be **null** (Bantu `null`); nulls are tracked per element and skipped by
aggregations.

## Columns are immutable
Every operation returns a **new** column and never changes its input — so there are no surprises
about shared state. Memory is reclaimed automatically when a column is no longer used.

---

## Building & inspecting a column
| Call | Does |
|---|---|
| `col(list, dtype)` | build a column from a Bantu list (`dtype` ∈ f64/i64/bool/utf8) |
| `col_len(c)` | number of elements |
| `col_dtype(c)` | the dtype string |
| `col_get(c, i)` | element `i` (or `null` if that element is null) |
| `col_to_list(c)` | convert back to a Bantu list |
| `col_slice(c, start, len)` | a contiguous sub-range |
| `col_cast(c, dtype)` | convert to another dtype |
| `col_is_null(c)` | bool column, `true` where an element is null |
| `col_null_count(c)` | how many nulls |
| `col_fill_null(c, value)` | replace nulls with `value` |

```
$c = col([10, null, 30], "i64");
print(col_null_count($c));           // 1
print(col_to_list(col_fill_null($c, 0)));   // [10, 0, 30]
```

## Arithmetic (a column, or a column and a number)
`col_add col_sub col_mul col_div col_mod col_pow` — and `col_neg`, `col_abs`.
```
$price = col([100, 250, 400], "f64");
$withTax = col_mul($price, 1.1);     // multiply every value by 1.1
```
`i64 + i64` stays exact `i64`; division and any decimal give `f64`. A null anywhere gives a null.

## Comparisons → a true/false mask
`col_gt col_ge col_lt col_le col_eq col_ne` (numbers, or text compared alphabetically).
```
$old = col_gt($age, 18);             // [false, true, ...]
```

## Combining masks
`col_and(a, b)`, `col_or(a, b)`, `col_not(a)`.

## Choosing values by condition (like if / else-if / else)
```
$tier = col_where(col_gt($amount, 1000), "big", "small");   // if / else per row

$grade = col_case([                                          // if / else-if / else per row
    col_ge($score, 90), "A",
    col_ge($score, 80), "B",
    col_ge($score, 70), "C"
], "F");                                                    // the final "else"
```

## Selecting rows
| Call | Does |
|---|---|
| `col_filter(c, mask)` | keep elements where the mask is true |
| `col_take(c, indexCol)` | pick elements by an index column |
| `col_head(c, n)` / `col_tail(c, n)` | first / last `n` (default 5) |

## Summaries (return one value; nulls skipped)
`col_sum col_mean col_min col_max col_std col_var col_median col_count col_nunique col_any col_all`.
```
print(col_mean($amount));
print(col_median($amount));
print(col_nunique($region));         // how many distinct regions
```
`col_std`/`col_var` are the sample versions (ddof = 1, same as pandas).

## Sorting
`col_argsort(c, descending)` returns an **index column**; apply it with `col_take` — that way you can
reorder several columns the same way:
```
$order = col_argsort($amount, true);          // indices, highest first
$topAmounts = col_take($amount, $order);
$topNames   = col_take($name, $order);         // same order
```

## Grouping
```
$ids = col_group_ids($region);                 // a group number per row

$g = col_group_agg($region, $amount, "sum");   // sum amount within each region
// $g.keys[0] -> the distinct regions, $g.values -> the sum per region, $g.ngroups -> count
```
`op` ∈ `sum mean min max std var median count nunique any all`. Group by several columns by passing a
list: `col_group_agg([$region, $year], $amount, "mean")`.

## Joining
`col_join(leftKeys, rightKeys, how)` returns index columns you apply with `col_take`. `how` ∈
`inner left right outer`; unmatched rows on an outer side give null indices.
```
$j = col_join($orders.cols.customer_id, $customers.cols.id, "left");
$orderName = col_take($customers.cols.name, $j.right_idx);   // customer name per order
```

## Reading & writing data
| Call | Does |
|---|---|
| `read_csv(path, options?)` | parse a CSV into `{names, cols, shape}`; infers each column's type. `options` = `{"delim": ",", "header": true}` |
| `write_csv(frame, path)` | write a `{names, cols}` frame to CSV |
| `write_csv(names, cols, path)` | write specific columns (in `names` order) |
| `read_sqlite(path, query)` | run a SQL query and get typed columns back |

```
$df = read_csv("sales.csv");
$byRegion = col_group_agg($df.cols.region, $df.cols.amount, "sum");
write_csv(["region", "total"], {"region": $byRegion.keys[0], "total": $byRegion.values}, "out.csv");
```

## A small end-to-end example
```
$df = read_csv("sales.csv");                       // load
$big = col_filter($df.cols.amount, col_gt($df.cols.amount, 1000));   // rows over 1000
print("count over 1000: " + str(col_len($big)));
$g = col_group_agg($df.cols.region, $df.cols.amount, "mean");        // average per region
$k = $g.keys[0]; $v = $g.values;
$i = 0;
while ($i < $g.ngroups) {
    print(col_get($k, $i) + ": " + str(col_get($v, $i)));
    $i = $i + 1;
}
```

## Notes
- **Performance.** Column operations are vectorized in native C++: on a few million rows, arithmetic,
  comparisons, filters and aggregations run in well under a second; grouping and joins use hash
  tables. Reading a 1M-row CSV takes ~1.5 s. This is what makes real data work practical in Bantu
  (a pure-Bantu loop over the same data would be thousands of times slower).
- **Exactness.** `i64` columns hold true 64-bit integers internally; when you read a very large
  integer back into a Bantu number it becomes a float64 (exact up to 2^53), so keep large-integer
  work inside columns where possible.
- **`else if`.** The language now supports `if (a) {} else if (b) {} else {}` directly — the
  everyday version of `col_case`.
