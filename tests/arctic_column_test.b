// ════════════════════════════════════════════════════════════════════════
//  arctic_column_test.b — native Column primitive (arctic foundations Phase 2).
//  Run:  bantu run tests/arctic_column_test.b
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

print("");
print("-- construction & dtype --");
$f = col([1.5, 2.5, 3.0], "f64");
ok(type($f) == "column", "type() reports 'column'");
eq(col_dtype($f), "f64", "f64 dtype");
eq(col_len($f), 3, "length");

$i = col([10, 20, 30, 40], "i64");
eq(col_dtype($i), "i64", "i64 dtype");
eq(col_len($i), 4, "i64 length");

$b = col([true, false, true], "bool");
eq(col_dtype($b), "bool", "bool dtype");

$s = col(["a", "bb", "ccc"], "utf8");
eq(col_dtype($s), "utf8", "utf8 dtype");

print("");
print("-- round-trip to list --");
eq(str(col_to_list($f)), "[1.5, 2.5, 3]", "f64 round-trip");
eq(str(col_to_list($i)), "[10, 20, 30, 40]", "i64 round-trip");
eq(str(col_to_list($s)), "[a, bb, ccc]", "utf8 round-trip");

print("");
print("-- element access --");
eq(col_get($f, 0), 1.5, "get first");
eq(col_get($i, 3), 40, "get last i64");
eq(col_get($s, 1), "bb", "get string");

print("");
print("-- nulls (Bantu null becomes a column null) --");
$n = col([1, null, 3, null, 5], "f64");
eq(col_null_count($n), 2, "null count");
eq(col_get($n, 1), null, "null element reads back as null");
eq(col_get($n, 0), 1, "non-null element intact");
eq(str(col_to_list(col_is_null($n))), "[false, true, false, true, false]", "is_null mask marks null positions");
ok(col_get(col_is_null($n), 1) == true, "is_null true at a null position");
ok(col_get(col_is_null($n), 0) == false, "is_null false at a present position");

$filled = col_fill_null($n, 0);
eq(col_null_count($filled), 0, "fill_null removes nulls");
eq(col_get($filled, 1), 0, "filled value present");
eq(col_null_count($n), 2, "fill_null did not mutate the original (immutable)");

print("");
print("-- slice --");
$sl = col_slice($i, 1, 2);
eq(str(col_to_list($sl)), "[20, 30]", "slice [1,2)");
eq(col_len($sl), 2, "slice length");
$sl2 = col_slice($i, 2, 100);   // len clamped to bounds
eq(str(col_to_list($sl2)), "[30, 40]", "slice length clamped");

print("");
print("-- cast --");
$c2i = col_cast($f, "i64");
eq(str(col_to_list($c2i)), "[2, 3, 3]", "f64->i64 rounds");   // llround: 1.5->2, 2.5->3 (half away from zero), 3->3
$i2s = col_cast($i, "utf8");
eq(str(col_to_list($i2s)), "[10, 20, 30, 40]", "i64->utf8 stringifies");
$s2f = col_cast(col(["1.5", "x", "3"], "utf8"), "f64");
eq(col_get($s2f, 0), 1.5, "utf8->f64 parses");
eq(col_get($s2f, 1), null, "utf8->f64 unparseable becomes null");

print("");
print("-- friendly errors --");
$threw = false;
try { col([1,2], "f65"); } catch ($e) { $threw = true; }
ok($threw, "bad dtype raises an error");
$threw2 = false;
try { col_get($i, 99); } catch ($e) { $threw2 = true; }
ok($threw2, "out-of-range index raises an error");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
