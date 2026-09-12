// ════════════════════════════════════════════════════════════════════════
//  arctic_window_test.b — window / set / string column kernels.
//  Run:  build/bantu run tests/arctic_window_test.b
// ════════════════════════════════════════════════════════════════════════

$R = {"pass": 0, "fail": 0};
def eq($got, $want, $name) {
    if ($got == $want) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); print("          got:  " + str($got)); print("          want: " + str($want)); }
}
def L($c) { return str(col_to_list($c)); }

print("");
print("-- cumulative --");
$n = col([1, 2, 3, 4], "i64");
eq(L(col_cumsum($n)),  "[1, 3, 6, 10]", "col_cumsum");
eq(L(col_cumprod($n)), "[1, 2, 6, 24]", "col_cumprod");
eq(L(col_cummax(col([3, 1, 4, 1], "i64"))), "[3, 3, 4, 4]", "col_cummax");
eq(L(col_cummin(col([3, 1, 4, 0], "i64"))), "[3, 1, 1, 0]", "col_cummin");
$withNull = col([1, null, 3], "i64");
eq(L(col_cumsum($withNull)), "[1, null, 4]", "cumsum skips nulls, keeps null in place");

print("");
print("-- shift --");
eq(L(col_shift($n, 1)),  "[null, 1, 2, 3]", "shift down 1");
eq(L(col_shift($n, -1)), "[2, 3, 4, null]", "shift up 1");
eq(L(col_shift($n, 0)),  "[1, 2, 3, 4]", "shift 0 is identity");
$dt = col_to_datetime(col(["2020-01-01 00:00:00", "2021-01-01 00:00:00"], "utf8"));
eq(col_dtype(col_shift($dt, 1)), "datetime", "shift keeps datetime overlay");

print("");
print("-- rank (ties share lowest) --");
eq(L(col_rank(col([10, 30, 20, 30], "i64"), false)), "[1, 3, 2, 3]", "ascending rank with tie");
eq(L(col_rank(col([10, 30, 20], "i64"), true)), "[3, 1, 2]", "descending rank");

print("");
print("-- quantile --");
$q = col([1, 2, 3, 4], "i64");
eq(col_quantile($q, 0), 1, "q=0 is min");
eq(col_quantile($q, 1), 4, "q=1 is max");
eq(col_quantile($q, 0.5), 2.5, "median by interpolation");

print("");
print("-- concat --");
eq(L(col_concat([col([1,2],"i64"), col([3],"i64")])), "[1, 2, 3]", "concat i64");
eq(L(col_concat([col([1,2],"i64"), col([3.5],"f64")])), "[1, 2, 3.5]", "concat widens to f64");
eq(L(col_concat([col(["a"],"utf8"), col([1],"i64")])), "[a, 1]", "concat widens to utf8");

print("");
print("-- unique mask --");
$k = col(["a", "b", "a", "c", "b"], "utf8");
eq(L(col_unique_mask($k)), "[true, true, false, true, false]", "first occurrence per key");
eq(L(col_filter($k, col_unique_mask($k))), "[a, b, c]", "filter by mask = unique values");

print("");
print("-- is_in --");
eq(L(col_is_in(col([1,2,3,4],"i64"), [2,4])), "[false, true, false, true]", "numeric is_in");
eq(L(col_is_in(col(["a","b","c"],"utf8"), ["a","c"])), "[true, false, true]", "text is_in");

print("");
print("-- round --");
eq(L(col_round(col([1.234, 5.678], "f64"), 1)), "[1.2, 5.7]", "round 1dp");
eq(L(col_round(col([1.4, 1.6], "f64"), 0)), "[1, 2]", "round 0dp");

print("");
print("-- string ops --");
$s = col(["  Hello ", "World", null], "utf8");
eq(L(col_strip($s)), "[Hello, World, null]", "strip");
eq(L(col_upper(col(["ab","cd"],"utf8"))), "[AB, CD]", "upper");
eq(L(col_lower(col(["AB","CD"],"utf8"))), "[ab, cd]", "lower");
eq(L(col_str_len(col(["abc","de"],"utf8"))), "[3, 2]", "str_len");
eq(L(col_contains(col(["apple","banana"],"utf8"), "an")), "[false, true]", "contains");
eq(L(col_starts_with(col(["apple","apricot"],"utf8"), "ap")), "[true, true]", "starts_with");
eq(L(col_ends_with(col(["apple","banana"],"utf8"), "na")), "[false, true]", "ends_with");
eq(L(col_replace(col(["a-b-c"],"utf8"), "-", "+")), "[a+b+c]", "replace all");
eq(L(col_substr(col(["abcdef"],"utf8"), 1, 3)), "[bcd]", "substr");
eq(L(col_substr(col(["abcdef"],"utf8"), -2, -1)), "[ef]", "substr negative start to end");
// categorical text ops work too
eq(L(col_upper(col_to_categorical(col(["eu","us"],"utf8")))), "[EU, US]", "string op on categorical");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
