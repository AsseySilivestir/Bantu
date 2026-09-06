// ════════════════════════════════════════════════════════════════════════
//  arctic_arrow_test.b — Parquet + Feather/Arrow-IPC round-trip.
//  Requires a BANTU_ARROW build; self-skips (ALL GREEN, 0 checks) otherwise.
//  Run:  build/bantu run tests/arctic_arrow_test.b
// ════════════════════════════════════════════════════════════════════════

$R = {"pass": 0, "fail": 0};
def eq($got, $want, $name) {
    if ($got == $want) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); print("          got:  " + str($got)); print("          want: " + str($want)); }
}
def ok($cond, $name) {
    if ($cond) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); }
}

if (!has_native("arrow")) {
    print("  (skipped: interpreter built without BANTU_ARROW)");
    print("========================================");
    print("  PASS: 0   FAIL: 0");
    print("========================================");
    print("  RESULT: ALL GREEN");
    return null;
}

// Build a frame with every dtype + nulls.
$names = ["i", "f", "s", "b", "dt", "da", "cat"];
$cols = {
    "i":   col([1, 2, null, 4], "i64"),
    "f":   col([1.5, 2.5, 3.5, null], "f64"),
    "s":   col(["ab", "cd", null, "ef"], "utf8"),
    "b":   col([true, false, true, null], "bool"),
    "dt":  col_to_datetime(col(["2020-01-15 10:30:00", "2021-06-01 00:00:00", null, "2022-12-31 23:59:59"], "utf8")),
    "da":  col_to_date(col(["2019-07-04", "2020-02-29", "2021-01-01", null], "utf8")),
    "cat": col_to_categorical(col(["EU", "US", "EU", "AS"], "utf8"))
};

$pq = "/tmp/arctic_test.parquet";
$ft = "/tmp/arctic_test.feather";

print("");
print("-- Parquet round-trip (all dtypes + nulls) --");
write_parquet($names, $cols, $pq);
$p = read_parquet($pq);
eq(str($p.names), "[i, f, s, b, dt, da, cat]", "parquet preserves column order");
eq(col_dtype($p.cols["i"]), "i64", "i64 dtype preserved");
eq(col_dtype($p.cols["f"]), "f64", "f64 dtype preserved");
eq(col_dtype($p.cols["b"]), "bool", "bool dtype preserved");
eq(col_dtype($p.cols["dt"]), "datetime", "datetime dtype preserved");
eq(col_dtype($p.cols["da"]), "date", "date dtype preserved");
eq(str(col_to_list($p.cols["i"])), "[1, 2, null, 4]", "i64 values + null");
eq(str(col_to_list($p.cols["f"])), "[1.5, 2.5, 3.5, null]", "f64 values + null");
eq(str(col_to_list($p.cols["s"])), "[ab, cd, null, ef]", "utf8 values + null");
eq(col_get($p.cols["dt"], 0), "2020-01-15 10:30:00", "datetime round-trips");
eq(col_get($p.cols["da"], 1), "2020-02-29", "date round-trips (leap day)");
eq(str(col_to_list($p.cols["cat"])), "[EU, US, EU, AS]", "categorical values round-trip (as text)");
eq(col_null_count($p.cols["b"]), 1, "bool null preserved");

print("");
print("-- Parquet projection pushdown (columns=) --");
$proj = read_parquet($pq, {"columns": ["cat", "i"]});
eq(str($proj.names), "[cat, i]", "only requested columns, in order");
eq(col_get($proj.cols["i"], 0), 1, "projected value correct");

print("");
print("-- Feather round-trip --");
write_feather($names, $cols, $ft);
$fr = read_feather($ft);
eq(col_dtype($fr.cols["dt"]), "datetime", "feather datetime preserved");
eq(str(col_to_list($fr.cols["f"])), "[1.5, 2.5, 3.5, null]", "feather f64 + null");
eq(col_get($fr.cols["da"], 0), "2019-07-04", "feather date round-trips");
$fproj = read_feather($ft, {"columns": ["s"]});
eq(str($fproj.names), "[s]", "feather projection");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
