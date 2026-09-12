// ════════════════════════════════════════════════════════════════════════
//  arctic_categorical_test.b — native categorical (dictionary-encoded) dtype.
//  Run:  build/bantu run tests/arctic_categorical_test.b
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

$regions = col(["EU", "US", "EU", "AS", "US", "EU"], "utf8");
$cat = col_to_categorical($regions);

print("");
print("-- construction --");
eq(col_dtype($cat), "cat", "dtype is cat");
eq(col_len($cat), 6, "length preserved");
eq(col_get($cat, 0), "EU", "renders category text");
eq(col_get($cat, 3), "AS", "renders category text 2");

print("");
print("-- categories & codes --");
eq(str(col_to_list(col_categories($cat))), "[EU, US, AS]", "categories in first-seen order");
eq(str(col_to_list(col_codes($cat))), "[0, 1, 0, 2, 1, 0]", "dictionary codes");
eq(col_nunique($cat), 3, "nunique = distinct categories");

print("");
print("-- comparison resolves to category text --");
$mask = col_eq($cat, "US");
eq(str(col_to_list($mask)), "[false, true, false, false, true, false]", "col_eq(cat, 'US')");
$ne = col_ne($cat, "EU");
eq(str(col_to_list($ne)), "[false, true, false, true, true, false]", "col_ne(cat, 'EU')");

print("");
print("-- filter/take keep categorical rendering --");
$kept = col_filter($cat, col_eq($cat, "EU"));
eq(col_dtype($kept), "cat", "filtered stays categorical");
eq(col_len($kept), 3, "three EU rows");
eq(col_get($kept, 0), "EU", "filtered renders text");

print("");
print("-- groupby on a categorical key --");
$vals = col([10, 20, 30, 40, 50, 60], "i64");
$g = col_group_agg($cat, $vals, "sum");
// group keys render as category strings; EU=10+30+60=100, US=20+50=70, AS=40
$keys = $g.keys[0];
eq(col_dtype($keys), "cat", "group key stays categorical");
eq(str(col_to_list($keys)), "[EU, US, AS]", "group keys render as text");
eq(str(col_to_list($g.values)), "[100, 70, 40]", "grouped sums by category");

print("");
print("-- nulls preserved through encoding --");
$withNull = col(["A", null, "B", "A"], "utf8");
$cn = col_to_categorical($withNull);
eq(col_null_count($cn), 1, "null stays null");
eq(str(col_to_list(col_categories($cn))), "[A, B]", "null not a category");
ok(col_get($cn, 1) == null, "null cell is null");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
