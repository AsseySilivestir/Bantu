// ════════════════════════════════════════════════════════════════════════
//  arctic_api_test.b — the completed arctic API: Series stats/text/window,
//  DataFrame set-ops/reshape/combine/stats, GroupBy extras, LazyFrame extras
//  (incl. optimizer barrier correctness) and JSON I/O.
//  Run:  build/bantu run tests/arctic_api_test.b
// ════════════════════════════════════════════════════════════════════════

include "../arctic/arctic.b" as arctic;

$R = {"pass": 0, "fail": 0};
def eq($got, $want, $name) {
    if ($got == $want) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); print("          got:  " + str($got)); print("          want: " + str($want)); }
}
def ok($cond, $name) {
    if ($cond) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); }
}
def dbl($x) { return $x * 2; }

$df = arctic.dataframe({
    "region": ["EU", "US", "EU", "US", "EU"],
    "year":   [2020, 2020, 2021, 2021, 2021],
    "amount": [10, 20, 30, 40, 50],
    "name":   ["ada", "bob", "cy", "dee", "eve"]
});

print("");
print("-- Series: set & membership --");
$s = arctic.series("v", [3, 1, 4, 1, 5], null);
eq(str($s.unique().to_list()), "[3, 1, 4, 5]", "unique keeps first occurrences");
eq(str($s.is_in([1, 5]).to_list()), "[false, true, false, true, true]", "is_in");
eq(str($s.between(2, 4).to_list()), "[true, false, true, false, false]", "between");
eq(str(arctic.series("x", [1, null, 3], null).drop_nulls().to_list()), "[1, 3]", "drop_nulls");
eq(str(arctic.series("x", [1, null, 3], null).is_not_null().to_list()), "[true, false, true]", "is_not_null");
$vc = $s.value_counts();
eq(str($vc.get("v").to_list()), "[1, 3, 4, 5]", "value_counts values (by count desc)");
eq(str($vc.get("count").to_list()), "[2, 1, 1, 1]", "value_counts counts");
eq($s.mode(), 1, "mode");

print("");
print("-- Series: window & cumulative --");
eq(str($s.cumsum().to_list()), "[3, 4, 8, 9, 14]", "cumsum");
eq(str($s.cummax().to_list()), "[3, 3, 4, 4, 5]", "cummax");
eq(str($s.shift(1).to_list()), "[null, 3, 1, 4, 1]", "shift");
eq(str($s.diff(null).to_list()), "[null, -2, 3, -3, 4]", "diff");
eq(str($s.rank(false).to_list()), "[3, 1, 4, 1, 5]", "rank (ties share lowest)");
eq($s.product(), 60, "product");
eq($s.first(), 3, "first");
eq($s.last(), 5, "last");
eq(str($s.reverse().to_list()), "[5, 1, 4, 1, 3]", "reverse");
eq(str($s.n_largest(2).to_list()), "[5, 4]", "n_largest");

print("");
print("-- Series: numeric shaping --");
eq($s.quantile(0.5), 3, "quantile median");
eq(str($s.clip(2, 4).to_list()), "[3, 2, 4, 2, 4]", "clip");
eq(str(arctic.series("f", [1.234, 5.678], null).round(1).to_list()), "[1.2, 5.7]", "round");
eq(str($s.map(dbl).to_list()), "[6, 2, 8, 2, 10]", "map applies a Bantu function");

print("");
print("-- Series: text --");
$t = arctic.series("t", ["  Ab ", "cd", "Ab"], null);
eq(str($t.strip().upper().to_list()), "[AB, CD, AB]", "strip + upper");
eq(str($t.strip().lower().contains("b").to_list()), "[true, false, true]", "contains");
eq(str($t.strip().starts_with("A").to_list()), "[true, false, true]", "starts_with");
eq(str($t.strip().str_len().to_list()), "[2, 2, 2]", "str_len");
eq(str(arctic.series("z", ["a-b"], null).replace("-", "+").to_list()), "[a+b]", "replace");
eq(str(arctic.series("z", ["abcdef"], null).substr(1, 3).to_list()), "[bcd]", "substr");

print("");
print("-- DataFrame: set ops & nulls --");
eq(str($df.sort(["region", "amount"], false).get("amount").to_list()), "[10, 30, 50, 20, 40]", "multi-column sort");
eq(str($df.unique(["region"]).get("region").to_list()), "[EU, US]", "unique on a subset");
eq(arctic.dataframe({"a": [1, null, 3]}).drop_nulls(null).height(), 2, "drop_nulls");
eq(str(arctic.dataframe({"a": [1, null, 3]}).fill_null(0, null).get("a").to_list()), "[1, 0, 3]", "fill_null");
eq($df.null_count()["amount"], 0, "null_count per column");
ok(!$df.is_empty(), "is_empty false on data");

print("");
print("-- DataFrame: shaping & access --");
eq(str($df.slice(1, 2).get("amount").to_list()), "[20, 30]", "slice");
eq(str($df.reverse().get("amount").to_list()), "[50, 40, 30, 20, 10]", "reverse");
eq($df.row(0)["region"], "EU", "row() as a dict");
eq(len($df.iter_rows()), 5, "iter_rows");
eq(str($df.n_largest(2, "amount").get("amount").to_list()), "[50, 40]", "n_largest by column");
eq($df.with_columns([["dbl", $df.get("amount").mul(2)]]).get("dbl").get(0), 20, "with_columns");
eq($df.cast({"amount": "f64"}).get("amount").dtype(), "f64", "cast");
eq($df.sample(3).height(), 3, "sample returns n rows");

print("");
print("-- DataFrame: stats --");
eq($df.quantile(0.5)["amount"], 30, "quantile per column");
ok($df.corr("year", "amount") > 0.8, "corr is strongly positive");
eq(str($df.value_counts("region").get("region").to_list()), "[EU, US]", "frame value_counts");

print("");
print("-- DataFrame: reshape --");
$p = $df.pivot("region", "year", "amount", "sum");
eq(str($p.columns()), "[region, 2020, 2021]", "pivot builds a column per key");
eq($p.get("2021").get(0), 80, "pivot aggregates (EU 2021 = 30+50)");
eq($p.get("2020").get(1), 20, "pivot US 2020 = 20");
$m = $df.select(["region", "amount", "year"]).melt(["region"], ["amount", "year"]);
eq($m.height(), 10, "melt = rows x value columns");
eq(str($m.columns()), "[region, variable, value]", "melt column layout");

print("");
print("-- DataFrame: combining --");
$a = arctic.dataframe({"k": ["x"], "v": [1]});
$b = arctic.dataframe({"k": ["y"], "v": [2]});
eq(str($a.concat($b).get("k").to_list()), "[x, y]", "concat stacks rows");
eq(str($a.hstack(arctic.dataframe({"w": [9]})).columns()), "[k, v, w]", "hstack adds columns");

print("");
print("-- GroupBy extras --");
eq($df.groupby("region").std("amount").height(), 2, "groupby std");
eq(str($df.groupby("region").size().get("count").to_list()), "[3, 2]", "groupby size");
eq($df.groupby("region").median("amount").get("amount").get(0), 30, "groupby median (EU)");
eq($df.groupby("region").nunique("year").get("year").get(0), 2, "groupby nunique");
ok(len($df.groupby("region").stats("amount").columns()) == 7, "groupby stats bundle");

print("");
print("-- LazyFrame extras --");
// (column order comes from the source dict's iteration order, so assert on
// membership rather than a fixed order)
$ld = $df.lazy().drop(["name"]).collect();
ok(!$ld.has("name"), "lazy drop removed the column");
eq($ld.width(), 3, "lazy drop leaves 3 columns");
$lr = $df.lazy().rename({"amount": "amt"}).collect();
ok($lr.has("amt"), "lazy rename added the new name");
ok(!$lr.has("amount"), "lazy rename removed the old name");
eq(str($df.lazy().unique(["region"]).collect().get("region").to_list()), "[EU, US]", "lazy unique");
eq($df.lazy().limit(2).collect().height(), 2, "lazy limit");
eq(str($df.lazy().reverse().collect().get("amount").to_list()), "[50, 40, 30, 20, 10]", "lazy reverse");
$tax = arctic.dataframe({"region": ["EU", "US"], "tax": [0.2, 0.1]});
ok($df.lazy().join($tax, "region", "left").collect().has("tax"), "lazy join");
eq($df.lazy().join($tax.lazy(), "region", "left").collect().height(), 5, "lazy join with a lazy right side");

print("");
print("-- optimizer: filters are not hoisted past a barrier --");
// head() then filter must NOT be reordered (that would change the result)
$eagerHF = $df.head(2).query("amount > 15");
$lazyHF  = $df.lazy().head(2).filter("amount > 15").collect();
eq(str($lazyHF.get("amount").to_list()), str($eagerHF.get("amount").to_list()), "head-then-filter matches eager");
$planHF = $df.lazy().head(2).filter("amount > 15").explain();
ok(indexOf($planHF, "HEAD") < indexOf($planHF, "FILTER"), "FILTER stays after HEAD");
// but with no barrier it still hoists ahead of the sort
$planSF = $df.lazy().sort("amount", true).filter("region == 'EU'").explain();
ok(indexOf($planSF, "FILTER") < indexOf($planSF, "SORT"), "FILTER hoisted above SORT");

print("");
print("-- JSON I/O --");
$txt = $df.to_json(null);
ok(contains($txt, "\"region\""), "to_json emits quoted keys");
$df.to_json("/tmp/arctic_api.json");
$back = arctic.read_json("/tmp/arctic_api.json");
eq($back.height(), 5, "read_json rows");
eq($back.get("amount").sum(), 150, "read_json values round-trip");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
