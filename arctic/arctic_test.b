// ════════════════════════════════════════════════════════════════════════
//  arctic_test.b — the arctic DataFrame library (pure Bantu on col_* atoms).
//  Run:  bantu run arctic/arctic_test.b
// ════════════════════════════════════════════════════════════════════════

include "./arctic.b" as arctic;

$R = {"pass": 0, "fail": 0};
def eq($got, $want, $name) {
    if ($got == $want) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); print("          got:  " + str($got)); print("          want: " + str($want)); }
}
def ok($cond, $name) {
    if ($cond) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); }
}

// Build a frame from lists (explicit column order via select where needed).
$df = arctic.dataframe({
    "name":   ["Ada", "Bob", "Cy", "Dee", "Eve"],
    "region": ["EU", "US", "EU", "US", "EU"],
    "age":    [36, 41, 29, 50, 33],
    "amount": [1200.0, 500.0, 2000.0, 700.0, 3000.0]
});

print("");
print("-- construction & shape --");
eq($df.height(), 5, "5 rows");
eq($df.width(), 4, "4 cols");
ok($df.has("amount"), "has amount");
ok(!$df.has("nope"), "does not have nope");

print("");
print("-- Series ops --");
$amt = $df.get("amount");
eq($amt.sum(), 7400, "amount sum");
eq($amt.max(), 3000, "amount max");
eq($amt.mean(), 1480, "amount mean");
$net = $amt.mul(1.1);
eq($net.get(0), 1320, "series scalar multiply");
$mask = $amt.gt(1000);
eq(str($mask.to_list()), "[true, false, true, false, true]", "series comparison -> mask");

print("");
print("-- select / drop / rename --");
$sel = $df.select(["name", "amount"]);
eq(str($sel.columns()), "[name, amount]", "select keeps order");
$dropped = $df.drop(["age"]);
ok(!$dropped.has("age"), "drop removed age");
ok($dropped.has("name"), "drop kept name");
$ren = $df.rename({"amount": "total"});
ok($ren.has("total"), "rename to total");
ok(!$ren.has("amount"), "old name gone");

print("");
print("-- with_column (Series & derived) --");
$df2 = $df.with_column("double", $df.get("amount").mul(2));   // exact
eq($df2.get("double").get(4), 6000, "with_column derived value");
eq($df2.width(), 5, "with_column added a column");

print("");
print("-- filter (Series mask) --");
$big = $df.filter($df.get("amount").gt(1000));
eq($big.height(), 3, "filter keeps 3 rows");
eq(str($big.get("name").to_list()), "[Ada, Cy, Eve]", "filter selects right rows");

print("");
print("-- query() DSL --");
$q1 = $df.query("amount > 1000");
eq($q1.height(), 3, "query single predicate");
$q2 = $df.query("amount > 1000 and region == 'EU'");
eq($q2.height(), 3, "query and (EU big: Ada, Cy, Eve)");
$q3 = $df.query("region == 'US' or age < 30");
eq(str($q3.get("name").to_list()), "[Bob, Cy, Dee]", "query or");
$q4 = $df.query("age >= 41");
eq($q4.height(), 2, "query >=");

print("");
print("-- sort --");
$s = $df.sort("amount", true);
eq(str($s.get("name").to_list()), "[Eve, Cy, Ada, Dee, Bob]", "sort desc by amount");
$s2 = $df.sort("age", false);
eq($s2.get("age").get(0), 29, "sort asc by age");

print("");
print("-- groupby / agg --");
$g = $df.groupby("region").agg([["amount", "sum", "total"], ["amount", "mean", "avg"]]);
eq($g.height(), 2, "two regions");
// EU total = 1200+2000+3000 = 6200 ; US = 500+700 = 1200
$euRow = $g.filter($g.get("region").eq("EU"));
eq($euRow.get("total").get(0), 6200, "EU total");
$usRow = $g.filter($g.get("region").eq("US"));
eq($usRow.get("total").get(0), 1200, "US total");
$gc = $df.groupby("region").count("name");
eq($gc.get("name").sum(), 5, "group counts sum to 5");

print("");
print("-- join --");
$prices = arctic.dataframe({"region": ["EU", "US"], "tax": [0.2, 0.1]});
$j = $df.join($prices, "region", "left");
ok($j.has("tax"), "join brought in tax");
eq($j.height(), 5, "left join keeps all left rows");
eq($j.get("tax").get(0), 0.2, "EU tax joined");

print("");
print("-- describe --");
$d = $df.describe();
ok($d.has("age"), "describe has numeric age");
ok($d.has("amount"), "describe has numeric amount");
ok(!$d.has("name"), "describe skips text name");
eq($d.get("amount").get(0), 5, "describe count row = 5");

print("");
print("-- show() renders without error --");
$table = $df.head(3).show();
ok(len($table) > 0, "show produced text");
ok(contains($table, "name"), "show includes a header");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
