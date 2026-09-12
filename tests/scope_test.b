// ════════════════════════════════════════════════════════════════════════
//  scope_test.b — regression tests for function-local variable scoping.
//  Run:  bantu run tests/scope_test.b
//
//  Guards the fix for the assignment-scope bug: a `$x = v` inside a function
//  must create/update a FUNCTION-LOCAL binding, never reach past the function
//  boundary and clobber a caller's / a global's variable of the same name.
//  (Reads still fall through to enclosing scopes.)
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

// A helper that internally uses $i (the same name a caller loop uses).
def usesI($n) {
    $i = 0;
    $sum = 0;
    while ($i < $n) { $sum = $sum + $i; $i = $i + 1; }
    return $sum;
}

// A helper with an inner loop (previously caused the caller's loop to hang).
def innerLoop($x) {
    $j = 0;
    while ($j < 3) { $j = $j + 1; }
    return $x * 2;
}

print("");
print("-- callee does not clobber caller's loop counter --");
$i = 0;
$count = 0;
while ($i < 5) {
    usesI(10);                 // internally sets $i = 0..10
    $count = $count + 1;
    $i = $i + 1;               // caller's $i must survive
}
eq($count, 5, "caller loop ran exactly 5 times (callee reused $i safely)");
eq($i, 5, "caller's $i is intact after the loop");

print("");
print("-- calling an inner-loop function in a loop terminates --");
$i = 0;
$acc = 0;
while ($i < 200) { $acc = $acc + innerLoop($i); $i = $i + 1; }
eq($i, 200, "200-iteration loop calling innerLoop terminated");
ok($acc > 0, "accumulator advanced");

print("");
print("-- assignment in a function does not clobber a same-named global --");
$leak = "outer";
def tryClobber() { $leak = "inner"; return $leak; }
eq(tryClobber(), "inner", "function sees its own local $leak");
eq($leak, "outer", "global $leak was NOT clobbered by the function");

print("");
print("-- functions can still READ globals --");
$globalCfg = "prod";
def readsGlobal() { return $globalCfg; }
eq(readsGlobal(), "prod", "function reads a global via the scope chain");

print("");
print("-- assignment inside a block updates the function-level var --");
def blockAssign() {
    $total = 0;
    $k = 0;
    while ($k < 4) {
        if (($k % 2) == 0) { $total = $total + $k; }   // update outer $total from inside if/while
        $k = $k + 1;
    }
    return $total;   // 0 + 2 = 2
}
eq(blockAssign(), 2, "block/if assignment updates the enclosing function var");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
