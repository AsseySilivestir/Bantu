// ════════════════════════════════════════════════════════════════════════
//  crypto_primitives_test.b — tests for the native crypto primitives added
//  for the hash/crypto/uuid modules (u32 bitwise, bytes/hex, CSPRNG, ct_equal).
//  Run:  bantu run tests/crypto_primitives_test.b
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


print("");
print("-- u32 bitwise --");
eq(band(65280, 4080), 3840, "band 0xff00 & 0x0ff0");
eq(bor(240, 15), 255, "bor 0xf0 | 0x0f");
eq(bxor(4294967295, 252645135), 4042322160, "bxor 0xffffffff ^ 0x0f0f0f0f");
eq(bnot(0), 4294967295, "bnot 0 = 0xffffffff (32-bit)");
eq(bnot(4294967295), 0, "bnot 0xffffffff = 0");
eq(shl(1, 8), 256, "shl 1 << 8");
eq(shl(1, 31), 2147483648, "shl 1 << 31");
eq(shr(256, 4), 16, "shr 256 >> 4");
eq(shr(2147483648, 31), 1, "shr logical (no sign bit)");

print("");
print("-- u32 rotate & modular --");
eq(rotl(2147483648, 1), 1, "rotl 0x80000000 <<< 1 = 1");
eq(rotr(1, 1), 2147483648, "rotr 1 >>> 1 = 0x80000000");
eq(rotl(305419896, 0), 305419896, "rotl by 0 is identity");
// round-trip: rotr(rotl(x, r), r) == x
eq(rotr(rotl(305419896, 7), 7), 305419896, "rotl then rotr round-trips");
eq(rotl(rotr(4042322160, 13), 13), 4042322160, "rotr then rotl round-trips");
eq(add32(4294967295, 1), 0, "add32 wraps at 2^32");
eq(add32(4000000000, 1000000000), 705032704, "add32 wraparound value");
eq(mul32(65536, 65536), 0, "mul32 2^16 * 2^16 wraps to 0");
eq(mul32(3, 4), 12, "mul32 small");

print("");
print("-- bytes / hex --");
eq(str(bytes("ABC")), "[65, 66, 67]", "bytes(\"ABC\")");
eq(frombytes([65, 66, 67]), "ABC", "frombytes -> string");
eq(frombytes(bytes("hello, bantu")), "hello, bantu", "bytes/frombytes round-trip");
eq(len(bytes("")), 0, "bytes of empty string");
eq(ord("A"), 65, "ord(\"A\")");
eq(ord("z"), 122, "ord(\"z\")");
eq(len(chr(200)), 1, "chr(200) is one byte (0..255 range)");
eq(ord(chr(200)), 200, "chr/ord round-trip at 200 (beyond ASCII)");
eq(tohex([0, 255, 16]), "00ff10", "tohex bytes");
eq(tohex(bytes("abc")), "616263", "tohex of \"abc\"");
eq(str(fromhex("00ff10")), "[0, 255, 16]", "fromhex -> bytes");
eq(tohex(fromhex("deadbeef")), "deadbeef", "hex round-trip");
eq(str(fromhex("de ad:be ef")), "[222, 173, 190, 239]", "fromhex ignores spaces/colons");

print("");
print("-- CSPRNG randbytes --");
$rb = randbytes(32);
eq(len($rb), 32, "randbytes(32) length");
$inRange = true;
$i = 0;
while ($i < len($rb)) {
    if ($rb[$i] < 0) { $inRange = false; }
    if ($rb[$i] > 255) { $inRange = false; }
    $i = $i + 1;
}
ok($inRange, "randbytes values are in 0..255");
$a = tohex(randbytes(16));
$b = tohex(randbytes(16));
ok($a != $b, "two randbytes draws differ (not constant)");
eq(len(randbytes(0)), 0, "randbytes(0) is empty");

print("");
print("-- constant-time compare --");
ok(ct_equal("secret-token", "secret-token"), "ct_equal equal strings");
ok(!ct_equal("secret-token", "secret-toker"), "ct_equal one-char difference");
ok(!ct_equal("abc", "abcd"), "ct_equal length mismatch");
ok(ct_equal([1, 2, 3], [1, 2, 3]), "ct_equal equal byte-lists");
ok(!ct_equal([1, 2, 3], [1, 2, 4]), "ct_equal differing byte-lists");


print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
