// ════════════════════════════════════════════════════════════════════════
//  crypto_encrypt_test.b — authenticated encryption + password hashing.
//  These require an interpreter built with libsodium (BANTU_SODIUM=1). When
//  it's absent the whole suite is SKIPPED (reported, not failed), so this test
//  is safe to run on any build.
//
//  Run:  bantu run crypto/crypto_encrypt_test.b
// ════════════════════════════════════════════════════════════════════════

include "./crypto.b" as crypto;

$R = {"pass": 0, "fail": 0, "skip": 0};
def ok($cond, $name) {
    if ($cond) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); }
}

print("");
if (!crypto.encryption_available()) {
    $R.skip = 1;
    print("-- native encryption not built in (BANTU_SODIUM off): SKIPPED --");
} else {
    print("-- AEAD (XChaCha20-Poly1305) --");
    $key = crypto.new_key();
    ok(len($key) == 32, "new_key is 32 bytes");

    $tok = crypto.encrypt($key, "attack at dawn", "v1-header");
    $pt  = crypto.decrypt($key, $tok, "v1-header");
    ok($pt != null, "decrypt returns plaintext");
    ok(frombytes($pt) == "attack at dawn", "round-trip matches original");

    // Same plaintext encrypts differently each time (random nonce).
    $tok2 = crypto.encrypt($key, "attack at dawn", "v1-header");
    ok($tok != $tok2, "nonce randomization: two ciphertexts differ");

    // Wrong AAD is rejected.
    ok(crypto.decrypt($key, $tok, "wrong-header") == null, "wrong AAD rejected");
    // Wrong key is rejected.
    ok(crypto.decrypt(crypto.new_key(), $tok, "v1-header") == null, "wrong key rejected");
    // Tampered ciphertext is rejected.
    $raw = crypto.encrypt_bytes($key, "hello world", "");
    $raw[30] = band($raw[30] + 1, 255);
    ok(crypto.decrypt_bytes($key, $raw, "") == null, "tampered ciphertext rejected");

    // Empty message and empty AAD are valid.
    $etok = crypto.encrypt($key, "", "");
    ok(frombytes(crypto.decrypt($key, $etok, "")) == "", "empty message round-trips");

    print("");
    print("-- password hashing (argon2id) --");
    $h = crypto.hash_password("correct horse battery staple");
    ok(substr($h, 0, 10) == "$argon2id$", "hash uses argon2id");
    ok(crypto.verify_password($h, "correct horse battery staple"), "verify accepts correct password");
    ok(!crypto.verify_password($h, "Tr0ub4dor&3"), "verify rejects wrong password");
    // Two hashes of the same password differ (random salt) but both verify.
    $h2 = crypto.hash_password("correct horse battery staple");
    ok($h != $h2, "salted: two hashes of same password differ");
    ok(crypto.verify_password($h2, "correct horse battery staple"), "second hash also verifies");
}

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail) + "   SKIP: " + str($R.skip));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
