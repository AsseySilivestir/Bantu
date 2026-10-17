// ════════════════════════════════════════════════════════════════════════
//  format_test.b — the format() builtin, new in v1.4.0.
//
//  format(fmt, args...) is Python-style "{}"/"{0}"/"{name}" formatting with
//  {{ and }} escapes. Out-of-range indices, missing keys and unbalanced
//  braces raise; a bad template cannot silently render "0" or "null" where a
//  value should be.
//
//  Run:  bantu run tests/format_test.b
// ════════════════════════════════════════════════════════════════════════

$R = {"pass": 0, "fail": 0};

def eq($got, $want, $name) {
    if ($got == $want) {
        $R.pass = $R.pass + 1;
        print("  ok    " + $name);
    } else {
        $R.fail = $R.fail + 1;
        print("  FAIL  " + $name);
        print("          got:  " + str($got));
        print("          want: " + str($want));
    }
}

def ok($cond, $name) {
    if ($cond) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); }
}

// Helper: assert that calling $fn raises an error containing $needle.
def assertRaises($fn, $needle, $name) {
    $crashed = false;
    try {
        $fn();
        $crashed = true;
    } catch ($e) {
        ok(contains(str($e), $needle), $name);
    }
    if ($crashed) { $R.fail = $R.fail + 1; print("  FAIL  " + $name + " (expected an error, none was thrown)"); }
}

print("-- positional --");
eq(format("Hello, {}!", "World"), "Hello, World!", "auto positional {}");
eq(format("{} {}", "a", "b"), "a b", "two auto positional {} {}");
eq(format("{0} + {0} = {1}", "1", "2"), "1 + 1 = 2", "explicit {0} reused, {1} second arg");
eq(format("{}", 42), "42", "auto positional of a number renders via str()");
eq(format("{}", [1, 2, 3]), "[1, 2, 3]", "auto positional of a list renders via str()");
eq(format("{0}", "only"), "only", "explicit {0} with a single argument");

print("");
print("-- keyed (dict as the second argument) --");
eq(format("{name} is {age}", {"name": "Ada", "age": 36}), "Ada is 36", "{name}/{age} from a dict");
eq(format("hi {name}!", {"name": "Bantu"}), "hi Bantu!", "single key lookup");
eq(format("{0} + {name}", {"name": "x"}, 5), "5 + x", "mixing positional and keyed (dict at args[1], positional at args[2])");

print("");
print("-- escapes --");
eq(format("{{literal}}"), "{literal}", "{{ and }} escape to literal braces");
eq(format("{{}}"), "{}", "{{}} -> {}");
eq(format("text with {{braces}} inside"), "text with {braces} inside", "{{braces}} inside text");

print("");
print("-- no placeholders / no args --");
eq(format(""), "", "empty format string returns empty string");
eq(format("no placeholders"), "no placeholders", "a plain string with no placeholders passes through");

print("");
print("-- errors --");
assertRaises(def() { format("{"); }, "unbalanced", "an unbalanced '{' raises");
assertRaises(def() { format("}"); }, "unbalanced", "an unbalanced '}' raises");
assertRaises(def() { format("{name}"); }, "dict", "a key placeholder with no dict raises");
assertRaises(def() { format("{missing}", {"x": 1}); }, "not found", "a missing key raises");
assertRaises(def() { format("{}", 1); format("{}", 2, 3); format("{0}{1}{2}", "a", "b"); }, "out of range", "an out-of-range index raises");

print("");
print("-- multiple positionals, mixed types --");
eq(format("{} and {} and {}", 1, "two", 3), "1 and two and 3", "three auto-positionals with mixed types");
eq(format("{2}-{1}-{0}", "a", "b", "c"), "c-b-a", "explicit indices can be in any order");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
