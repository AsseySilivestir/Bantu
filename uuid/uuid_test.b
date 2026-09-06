// ════════════════════════════════════════════════════════════════════════
//  uuid_test.b — tests for the Bantu UUID module.
//  Run:  bantu run uuid/uuid_test.b
//  Vectors: RFC 4122 name-based UUIDs (v3 MD5, v5 SHA-1).
// ════════════════════════════════════════════════════════════════════════

include "./uuid.b" as uuid;

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

def isVariant($u) {
    // variant char (start of the 4th group, index 19) must be 8, 9, a, or b.
    $c = substr($u, 19, 1);
    if ($c == "8") { return true; }
    if ($c == "9") { return true; }
    if ($c == "a") { return true; }
    if ($c == "b") { return true; }
    return false;
}

// The 48-bit millisecond timestamp embedded in a v7 UUID (bytes 0..5).
// (Bantu's > is numeric, not lexicographic, so we compare the timestamps.)
def ts48($u) {
    $b = uuid.parse($u);
    return $b[0] * 1099511627776 + $b[1] * 4294967296 + $b[2] * 16777216 +
           $b[3] * 65536 + $b[4] * 256 + $b[5];
}


print("");
print("-- name-based UUIDs (RFC 4122 vectors) --");
eq(uuid.uuid5(uuid.NAMESPACE_DNS, "python.org"),
   "886313e1-3b8a-5372-9b90-0c9aee199e5d", "uuid5 DNS python.org");
eq(uuid.uuid3(uuid.NAMESPACE_DNS, "python.org"),
   "6fa459ea-ee8a-3ca4-894e-db77e160355e", "uuid3 DNS python.org");
eq(uuid.uuid5(uuid.NAMESPACE_URL, "http://example.com/"),
   uuid.uuid5(uuid.NAMESPACE_URL, "http://example.com/"), "uuid5 is deterministic");
ok(uuid.uuid5(uuid.NAMESPACE_DNS, "a") != uuid.uuid5(uuid.NAMESPACE_DNS, "b"),
   "uuid5 differs by name");

print("");
print("-- uuid4 (random) --");
$u4 = uuid.uuid4();
ok(uuid.is_valid($u4), "uuid4 is well-formed");
eq(uuid.version_of($u4), 4, "uuid4 version nibble is 4");
ok(isVariant($u4), "uuid4 has RFC-4122 variant");
ok(uuid.uuid4() != uuid.uuid4(), "two uuid4 differ");

print("");
print("-- uuid7 (time-ordered) --");
$u7 = uuid.uuid7();
ok(uuid.is_valid($u7), "uuid7 is well-formed");
eq(uuid.version_of($u7), 7, "uuid7 version nibble is 7");
ok(isVariant($u7), "uuid7 has RFC-4122 variant");
// generated later in time → larger embedded timestamp (>= 5 ms apart)
$ta = ts48(uuid.uuid7());
sleep(5);
$tb = ts48(uuid.uuid7());
ok($tb > $ta, "uuid7 is time-ordered (later has a larger timestamp)");

print("");
print("-- parse / format / validate --");
eq(uuid.format(uuid.parse("886313e1-3b8a-5372-9b90-0c9aee199e5d")),
   "886313e1-3b8a-5372-9b90-0c9aee199e5d", "parse/format round-trip");
eq(len(uuid.parse("886313e1-3b8a-5372-9b90-0c9aee199e5d")), 16, "parse yields 16 bytes");
ok(!uuid.is_valid("not-a-uuid"), "is_valid rejects garbage");
ok(!uuid.is_valid("886313e13b8a53729b900c9aee199e5d"), "is_valid rejects missing dashes");
ok(uuid.is_valid(uuid.uuid4()), "is_valid accepts a generated uuid4");


print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
