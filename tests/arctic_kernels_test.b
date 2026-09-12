// ════════════════════════════════════════════════════════════════════════
//  arctic_kernels_test.b — vectorized column kernels (arctic Phase 3).
//  Run:  bantu run tests/arctic_kernels_test.b
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
def L($c) { return str(col_to_list($c)); }   // column -> printable list

$a = col([1, 2, 3, 4], "i64");
$f = col([1.0, 2.0, 4.0, 8.0], "f64");

print("");
print("-- arithmetic (column & scalar operands) --");
eq(L(col_add($a, $a)), "[2, 4, 6, 8]", "col + col (i64 stays i64)");
eq(L(col_add($a, 10)), "[11, 12, 13, 14]", "col + scalar");
eq(L(col_mul($f, 1.5)), "[1.5, 3, 6, 12]", "col * scalar (f64)");
eq(L(col_sub($a, 1)), "[0, 1, 2, 3]", "col - scalar");
eq(L(col_div($a, 2)), "[0.5, 1, 1.5, 2]", "col / scalar -> f64");
eq(L(col_neg($a)), "[-1, -2, -3, -4]", "neg");
eq(L(col_abs(col([-3, 4, -5], "i64"))), "[3, 4, 5]", "abs");

print("");
print("-- comparisons -> boolean mask --");
eq(L(col_gt($a, 2)), "[false, false, true, true]", "col_gt scalar");
eq(L(col_le($a, 2)), "[true, true, false, false]", "col_le scalar");
eq(L(col_eq($a, 3)), "[false, false, true, false]", "col_eq scalar");
$names = col(["ada", "bob", "cy"], "utf8");
eq(L(col_eq($names, "bob")), "[false, true, false]", "utf8 eq");
eq(L(col_gt($names, "bob")), "[false, false, true]", "utf8 lexicographic gt");

print("");
print("-- mask logic --");
$m1 = col_gt($a, 1);   // [f,t,t,t]
$m2 = col_lt($a, 4);   // [t,t,t,f]
eq(L(col_and($m1, $m2)), "[false, true, true, false]", "and");
eq(L(col_or($m1, $m2)), "[true, true, true, true]", "or");
eq(L(col_not($m1)), "[true, false, false, false]", "not");

print("");
print("-- conditional: col_where / col_case --");
$tier = col_where(col_gt($a, 2), "big", "small");
eq(L($tier), "[small, small, big, big]", "col_where");
$score = col([95, 82, 71, 40], "i64");
$grade = col_case([col_ge($score,90), "A", col_ge($score,80), "B", col_ge($score,70), "C"], "F");
eq(L($grade), "[A, B, C, F]", "col_case (if/elif/else over a column)");

print("");
print("-- select: filter / take / head / tail --");
eq(L(col_filter($a, col_gt($a, 2))), "[3, 4]", "filter by mask");
eq(L(col_take($a, col([3,1,0], "i64"))), "[4, 2, 1]", "take by index");
eq(L(col_head($a, 2)), "[1, 2]", "head");
eq(L(col_tail($a, 2)), "[3, 4]", "tail");

print("");
print("-- aggregations (nulls skipped) --");
$agg = col([2, 4, 4, 4, 5, 5, 7, 9], "f64");   // classic stddev example
eq(col_sum($agg), 40, "sum");
eq(col_mean($agg), 5, "mean");
eq(col_min($agg), 2, "min");
eq(col_max($agg), 9, "max");
eq(col_median($agg), 4.5, "median (even count)");
$vs = col([2, 4, 6], "f64");   // sample variance = 8/2 = 4, sample std = 2
eq(col_var($vs), 4, "variance (sample, ddof=1)");
eq(col_std($vs), 2, "stddev (sample) = 2");
eq(col_count(col([1, null, 3], "f64")), 2, "count ignores nulls");
eq(col_sum(col([1, null, 3], "f64")), 4, "sum ignores nulls");
eq(col_nunique(col([1,1,2,2,3,null], "i64")), 3, "nunique ignores nulls");
ok(col_any(col([false, false, true], "bool")), "any true");
ok(!col_all(col([true, false, true], "bool")), "all false when one is false");

print("");
print("-- argsort --");
$u = col([30, 10, 20], "i64");
eq(L(col_argsort($u, false)), "[1, 2, 0]", "argsort ascending");
eq(L(col_argsort($u, true)), "[0, 2, 1]", "argsort descending");
eq(L(col_take($u, col_argsort($u, false))), "[10, 20, 30]", "take by argsort sorts the column");

print("");
print("-- group ids & group_agg --");
$region = col(["EU","US","EU","US","EU"], "utf8");
$amount = col([10, 5, 20, 7, 30], "f64");
eq(L(col_group_ids($region)), "[0, 1, 0, 1, 0]", "dense group ids in first-seen order");
$g = col_group_agg($region, $amount, "sum");
eq(L($g.keys[0]), "[EU, US]", "group_agg keys (first-seen order)");
eq(L($g.values), "[60, 12]", "group_agg sums per group");
eq($g.ngroups, 2, "ngroups");
$gm = col_group_agg($region, $amount, "mean");
eq(L($gm.values), "[20, 6]", "group_agg means per group");

print("");
print("-- join (index pairs) --");
$lk = col([1, 2, 3], "i64");
$rk = col([2, 3, 4], "i64");
$j = col_join($lk, $rk, "inner");
// inner: left rows 1,2 (values 2,3) match right rows 0,1
eq(L(col_take($lk, $j.left_idx)), "[2, 3]", "inner join left values");
eq(L(col_take($rk, $j.right_idx)), "[2, 3]", "inner join right values");
$jl = col_join($lk, $rk, "left");
eq(col_len($jl.left_idx), 3, "left join keeps all left rows");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
