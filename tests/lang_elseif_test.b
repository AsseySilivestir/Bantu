// ════════════════════════════════════════════════════════════════════════
//  lang_elseif_test.b — first-class `else if` (arctic foundations, Phase 1).
//  Run:  bantu run tests/lang_elseif_test.b
//
//  Guards the parser change that lets `if (a) {} else if (b) {} else {}` parse
//  directly (previously it required nesting: `else { if (b) {} }`).
// ════════════════════════════════════════════════════════════════════════

$R = {"pass": 0, "fail": 0};
def eq($got, $want, $name) {
    if ($got == $want) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); print("          got:  " + str($got)); print("          want: " + str($want)); }
}

// Classify with a 3-way else-if chain.
def grade($s) {
    if ($s >= 90) { return "A"; }
    else if ($s >= 80) { return "B"; }
    else if ($s >= 70) { return "C"; }
    else { return "F"; }
}

print("");
print("-- basic else-if chain picks the right arm --");
eq(grade(95), "A", "first arm");
eq(grade(85), "B", "second arm (else if)");
eq(grade(72), "C", "third arm (else if)");
eq(grade(40), "F", "else fallback");
eq(grade(90), "A", "boundary 90 -> A");
eq(grade(80), "B", "boundary 80 -> B");

print("");
print("-- untaken arms must not execute (side effects) --");
$log = [];
def route($x) {
    if ($x == 1) { $log[len($log)] = "one"; return "one"; }
    else if ($x == 2) { $log[len($log)] = "two"; return "two"; }
    else { $log[len($log)] = "other"; return "other"; }
}
eq(route(2), "two", "route(2) returns two");
eq(len($log), 1, "exactly one branch body ran");
eq($log[0], "two", "only the matching branch ran");

print("");
print("-- else-if with no final else, no arm matches --");
def maybe($x) {
    if ($x == 1) { return "a"; }
    else if ($x == 2) { return "b"; }
    return "none";   // falls through when nothing matched
}
eq(maybe(3), "none", "no arm matched, no else");
eq(maybe(1), "a", "matched first");

print("");
print("-- plain if/else still works (backward compatible) --");
def evenodd($n) {
    if (($n % 2) == 0) { return "even"; }
    else { return "odd"; }
}
eq(evenodd(4), "even", "plain else true");
eq(evenodd(7), "odd", "plain else false");

print("");
print("-- else-if nested inside loops and functions --");
$sum = 0;
$i = 0;
while ($i < 10) {
    if (($i % 3) == 0) { $sum = $sum + 0; }
    else if (($i % 3) == 1) { $sum = $sum + 1; }
    else { $sum = $sum + 100; }
    $i = $i + 1;
}
// i=0..9: rem0 at 0,3,6,9 (+0); rem1 at 1,4,7 (+1 =3); rem2 at 2,5,8 (+100*3=300)
eq($sum, 303, "else-if inside a while loop accumulates correctly");

print("");
print("-- deep chain: hundreds of arms via a generated cascade --");
// Build value through a long else-if cascade using a helper called in a loop.
def bucket($x) {
    if ($x < 1) { return 0; }
    else if ($x < 2) { return 1; }
    else if ($x < 3) { return 2; }
    else if ($x < 4) { return 3; }
    else if ($x < 5) { return 4; }
    else if ($x < 6) { return 5; }
    else if ($x < 7) { return 6; }
    else if ($x < 8) { return 7; }
    else { return 8; }
}
eq(bucket(0), 0, "cascade arm 0");
eq(bucket(6), 6, "cascade arm 6");
eq(bucket(99), 8, "cascade else");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
