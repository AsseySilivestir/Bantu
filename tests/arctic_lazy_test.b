// ════════════════════════════════════════════════════════════════════════
//  arctic_lazy_test.b — LazyFrame + query optimizer (pure Bantu).
//  Verifies lazy .collect() == eager equivalent, and that the optimizer applies
//  predicate + projection pushdown.
//  Run:  build/bantu run tests/arctic_lazy_test.b
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
def frameStr($fr) {
    $s = "names=" + str($fr.columns());
    each ($n in $fr.columns()) { $s = $s + "|" + $n + "=" + str($fr.get($n).to_list()); }
    return $s;
}
// with_column derivation used below (named def = a first-class value in Bantu)
def deriveNet($d) { return $d.get("amount").mul(2); }

$df = arctic.dataframe({
    "name":   ["Ada", "Bob", "Cy", "Dee", "Eve"],
    "region": ["EU", "US", "EU", "US", "EU"],
    "age":    [36, 41, 29, 50, 33],
    "amount": [1200, 500, 2000, 700, 3000]
});

print("");
print("-- lazy == eager: filter + select + sort --");
$eager = $df.query("amount > 1000").select(["name", "amount"]).sort("amount", true);
$lazy  = $df.lazy().filter("amount > 1000").select(["name", "amount"]).sort("amount", true).collect();
eq(frameStr($lazy), frameStr($eager), "filter/select/sort pipeline matches eager");

print("");
print("-- lazy == eager: groupby.agg --");
$eg = $df.groupby("region").agg([["amount", "sum", "total"], ["amount", "mean", "avg"]]);
$lg = $df.lazy().groupby("region").agg([["amount", "sum", "total"], ["amount", "mean", "avg"]]).collect();
eq(frameStr($lg), frameStr($eg), "groupby.agg pipeline matches eager");

print("");
print("-- lazy == eager: with_column then filter on derived --");
$ew = $df.with_column("net", deriveNet($df)).query("net > 2000").select(["name", "net"]);
$lw = $df.lazy().with_column("net", deriveNet).filter("net > 2000").select(["name", "net"]).collect();
eq(frameStr($lw), frameStr($ew), "with_column + derived filter matches eager");

print("");
print("-- predicate pushdown: filter hoisted before sort --");
$plan = $df.lazy().sort("amount", true).filter("region == 'EU'").explain();
$fpos = indexOf($plan, "FILTER");
$spos = indexOf($plan, "SORT");
ok($fpos >= 0, "plan has a FILTER");
ok($spos >= 0, "plan has a SORT");
ok($fpos < $spos, "FILTER appears before SORT (hoisted)");

print("");
print("-- predicate pushdown: filter on derived col NOT hoisted --");
$plan2 = $df.lazy().with_column("net", deriveNet).filter("net > 2000").explain();
$wpos = indexOf($plan2, "WITH_COLUMN");
$fpos2 = indexOf($plan2, "FILTER");
ok($wpos < $fpos2, "filter on derived column stays after WITH_COLUMN");

print("");
print("-- projection pushdown into a csv scan --");
$csv = "name,region,age,amount\nAda,EU,36,1200\nBob,US,41,500\nCy,EU,29,2000\n";
writefile("/tmp/arctic_lazy.csv", $csv);
$scan = arctic.scan_csv("/tmp/arctic_lazy.csv", null)
              .filter("amount > 1000")
              .select(["region", "amount"]);
$exp = $scan.explain();
ok(contains($exp, "project="), "explain shows projection pushdown");
ok(contains($exp, "amount"), "projected set includes filter/select cols");
ok(!contains($exp, "project=[name"), "unused 'name' not first in projection");
$res = $scan.collect();
eq(str($res.columns()), "[region, amount]", "collect returns projected columns");
eq($res.height(), 2, "collect filtered to 2 rows");

print("");
print("-- projection pushdown skipped when with_column present --");
$scan2 = arctic.scan_csv("/tmp/arctic_lazy.csv", null)
               .with_column("net", deriveNet)
               .filter("net > 2000");
$exp2 = $scan2.explain();
ok(!contains($exp2, "project="), "no projection pushdown with a with_column");
$res2 = $scan2.collect();
eq($res2.height(), 2, "with_column pipeline still correct (Ada 2400, Cy 4000)");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
