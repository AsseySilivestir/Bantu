// ════════════════════════════════════════════════════════════════════════
//  crypto_test.b — tests for the Bantu crypto module.
//  Run:  bantu run crypto/crypto_test.b
//  Vectors: RFC 4648 (base64), RFC 5869 (HKDF-SHA256).
// ════════════════════════════════════════════════════════════════════════

include "./crypto.b" as crypto;

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
print("-- base64 encode (RFC 4648) --");
eq(crypto.base64_encode(""), "", "b64 empty");
eq(crypto.base64_encode("f"), "Zg==", "b64 'f'");
eq(crypto.base64_encode("fo"), "Zm8=", "b64 'fo'");
eq(crypto.base64_encode("foo"), "Zm9v", "b64 'foo'");
eq(crypto.base64_encode("foob"), "Zm9vYg==", "b64 'foob'");
eq(crypto.base64_encode("fooba"), "Zm9vYmE=", "b64 'fooba'");
eq(crypto.base64_encode("foobar"), "Zm9vYmFy", "b64 'foobar'");

print("");
print("-- base64 decode + round-trip --");
eq(frombytes(crypto.base64_decode("Zm9vYmFy")), "foobar", "b64 decode 'foobar'");
eq(frombytes(crypto.base64_decode("Zm8=")), "fo", "b64 decode 'fo'");
eq(frombytes(crypto.base64_decode(crypto.base64_encode("hello, bantu world!"))),
   "hello, bantu world!", "b64 round-trip");
// decode tolerates embedded whitespace/newlines
eq(frombytes(crypto.base64_decode("Zm9v\nYmFy")), "foobar", "b64 decode ignores newline");

print("");
print("-- base64url (URL-safe alphabet, no padding) --");
eq(crypto.base64_encode(fromhex("fbffbf")), "+/+/", "base64 uses + and /");
eq(crypto.base64url_encode(fromhex("fbffbf")), "-_-_", "base64url uses - and _");
eq(crypto.base64url_encode("foobar"), "Zm9vYmFy", "base64url 'foobar'");
eq(frombytes(crypto.base64url_decode(crypto.base64url_encode("Ada+Lovelace/1843"))),
   "Ada+Lovelace/1843", "base64url round-trip");

print("");
print("-- hex --");
eq(crypto.hex_encode("abc"), "616263", "hex_encode 'abc'");
eq(frombytes(crypto.hex_decode("616263")), "abc", "hex_decode round-trip");

print("");
print("-- HKDF-SHA256 (RFC 5869 Test Case 1) --");
$ikm = fromhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
$salt = fromhex("000102030405060708090a0b0c");
$info = fromhex("f0f1f2f3f4f5f6f7f8f9");
eq(tohex(crypto.hkdf_sha256($ikm, $salt, $info, 42)),
   "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
   "HKDF RFC5869 case 1 OKM");
eq(len(crypto.hkdf_sha256($ikm, $salt, $info, 42)), 42, "HKDF output length");

print("");
print("-- HMAC verify (constant-time) --");
$mac = crypto.hmac_sha256("secret-key", "authentic message");
ok(crypto.verify_hmac("secret-key", "authentic message", $mac), "verify accepts a good MAC (hex)");
ok(crypto.verify_hmac("secret-key", "authentic message", fromhex($mac)), "verify accepts a good MAC (bytes)");
ok(!crypto.verify_hmac("secret-key", "tampered message", $mac), "verify rejects a bad message");
ok(!crypto.verify_hmac("wrong-key", "authentic message", $mac), "verify rejects a bad key");

print("");
print("-- secure random --");
eq(len(crypto.random_bytes(24)), 24, "random_bytes length");
eq(len(crypto.token_hex(16)), 32, "token_hex(16) is 32 chars");
ok(crypto.token_hex(16) != crypto.token_hex(16), "two token_hex differ");
ok(len(crypto.token_urlsafe(32)) > 0, "token_urlsafe non-empty");
$badChar = false;
$t = crypto.token_urlsafe(48);
$i = 0;
while ($i < len($t)) {
    $ch = substr($t, $i, 1);
    if ($ch == "+") { $badChar = true; }
    if ($ch == "/") { $badChar = true; }
    if ($ch == "=") { $badChar = true; }
    $i = $i + 1;
}
ok(!$badChar, "token_urlsafe has no +, /, or = characters");

// random_int: bounds and both extremes reachable
$okRange = true;
$lo = false;
$hi = false;
$i = 0;
while ($i < 300) {
    $r = crypto.random_int(6);
    if ($r < 0) { $okRange = false; }
    if ($r > 5) { $okRange = false; }
    if ($r == 0) { $lo = true; }
    if ($r == 5) { $hi = true; }
    $i = $i + 1;
}
ok($okRange, "random_int(6) stays in [0,6)");
ok($lo, "random_int(6) can return 0");
ok($hi, "random_int(6) can return 5");


print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
