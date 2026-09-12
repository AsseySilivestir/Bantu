// ════════════════════════════════════════════════════════════════════════
//  equality_test.b — what == and != mean for every kind of value.
//
//  This exists because `==` on a list or a dict was ALWAYS false. Value::equals
//  handled numbers, strings, bools, null and native handles, then fell through
//  to `default: return false` for everything else -- so:
//
//      [1,2,3] == [1,2,3]   -> false
//      $a == $a             -> false     (a variable compared with ITSELF)
//      {"k":1} == {"k":1}   -> false
//
//  and `!=` was correspondingly always true. Any program checking whether two
//  lists or two dicts matched silently got the wrong answer, with no error and
//  nothing in the output to suggest it. It was found while testing sua.udp,
//  where a byte-list assertion failed while printing identical got and want.
//
//  Lists and dicts now compare structurally, like Python and Ruby. Functions,
//  class instances and class definitions compare by identity, because two
//  separately written functions with the same body are not the same function.
//
//  Run:  bantu run tests/equality_test.b
// ════════════════════════════════════════════════════════════════════════

$R = {"pass": 0, "fail": 0};

def ok($cond, $name) {
    if ($cond) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); }
}

print("-- scalars (these always worked) --");
ok(1 == 1, "numbers");
ok(!(1 == 2), "different numbers");
ok("ab" == "ab", "strings");
ok(!("ab" == "ac"), "different strings");
ok(true == true, "bools");
ok(null == null, "null");
ok(1 == true, "number/bool coercion");
ok(0 == false, "zero is false");

print("");
print("-- lists --");
$a = [1, 2, 3];
ok($a == $a, "a list equals ITSELF (this returned false)");
ok([1, 2, 3] == [1, 2, 3], "two lists with the same elements");
ok(!([1, 2, 3] == [1, 2, 4]), "one element different");
ok(!([1, 2] == [1, 2, 3]), "different lengths");
ok([] == [], "two empty lists");
ok([1, 2, 3] != [1, 2, 4], "!= is the negation");
ok(!([1, 2, 3] != [1, 2, 3]), "!= is false for equal lists");
ok(["a", "b"] == ["a", "b"], "lists of strings");
ok([[1, [2, 3]], [4]] == [[1, [2, 3]], [4]], "nested lists compare all the way down");
ok(!([[1, [2, 3]]] == [[1, [2, 9]]]), "a difference deep inside is found");

print("");
print("-- dicts --");
$d = {"k": 1};
ok($d == $d, "a dict equals itself");
ok({"k": 1} == {"k": 1}, "two dicts with the same entries");
ok(!({"k": 1} == {"k": 2}), "different values");
ok(!({"k": 1} == {"j": 1}), "different keys");
ok(!({"k": 1} == {"k": 1, "j": 2}), "different sizes");
ok({} == {}, "two empty dicts");
// Iteration order is insertion order, but equality is by key: two dicts with
// the same entries are equal however they were built.
ok({"a": 1, "b": 2} == {"b": 2, "a": 1}, "entry order does not affect equality");
ok({"o": {"n": [1, 2]}} == {"o": {"n": [1, 2]}}, "nested dicts and lists");

print("");
print("-- mixed and cross-type --");
ok(!([1, 2] == {"0": 1}), "a list is never equal to a dict");
ok(!([1] == 1), "a list is not its only element");
ok(!(null == []), "null is not an empty list");
ok(!(null == 0), "null is not zero");
ok([{"a": [1]}] == [{"a": [1]}], "a list of dicts of lists");

print("");
print("-- structures that contain themselves --");
// These would recurse forever without a depth guard. The answer at the limit
// is arbitrary; what matters is that it terminates instead of blowing the
// stack, so a cyclic structure can never hang or crash the interpreter.
$c = [1, 2];
$c.push($c);
$c2 = [1, 2];
$c2.push($c2);
ok($c == $c, "a self-referential list terminates");
ok($c == $c2 || !($c == $c2), "comparing two cyclic lists terminates either way");
$sd = {"k": 1};
$sd.self = $sd;
ok($sd == $sd, "a self-referential dict terminates");

print("");
print("-- functions and instances compare by identity --");
def f1() { return 1; }
def f2() { return 1; }
ok(f1 == f1, "a function equals itself");
ok(!(f1 == f2), "two functions with identical bodies are still different");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
