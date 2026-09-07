// ════════════════════════════════════════════════════════════════════════
//  webpush_test.b — Web Push crypto atoms (RFC 8188 / 8291 / 8292).
//  Run:  bantu run tests/webpush_test.b
//
//  The byte-exact known-answer vectors (FIPS 197, McGrew-Viega GCM, RFC 5869,
//  RFC 6979 A.2.5, RFC 5903) live in the C++ selftest, which runs inside the
//  binary and can reach entry points that are deliberately not exposed to Bantu
//  (signing with a pinned nonce, encrypting with a pinned salt). This file
//  asserts the selftest passed and then exercises the Bantu-visible surface.
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
// Bantu has no repeat(); build the string.
def rep($ch, $n) {
    $s = "";
    $i = 0;
    while ($i < $n) { $s = $s + $ch; $i = $i + 1; }
    return $s;
}

print("");
print("-- availability --");
ok(has_native("webpush"), "has_native(\"webpush\")");

$st = webpush_selftest();
ok($st.ok, "known-answer selftest passes");
ok($st.ran > 40, "selftest ran a real suite (" + str($st.ran) + " checks)");
eq($st.failed, null, "no failing vector");

print("");
print("-- VAPID key generation --");
$kp = webpush_keygen();
ok($kp != null, "keygen returns a keypair");
// 32 raw octets -> 43 base64url chars; 65 -> 87. Both unpadded.
eq(len($kp.private_key), 43, "private key is 32 octets (43 base64url chars)");
eq(len($kp.public_key), 87, "public key is 65 octets (87 base64url chars)");
ok(!contains($kp.public_key, "="), "keys are unpadded base64url");
eq(webpush_public_key($kp.private_key), $kp.public_key, "public key derives from the private key");

$kp2 = webpush_keygen();
ok($kp.private_key != $kp2.private_key, "each keygen is distinct");
eq(webpush_public_key("not-a-key"), null, "rejects a malformed private key");

print("");
print("-- base64url --");
eq(b64url_encode(bytes("hello")), "aGVsbG8", "encodes without padding");
eq(frombytes(b64url_decode("aGVsbG8")), "hello", "decodes unpadded input");
eq(frombytes(b64url_decode("aGVsbG8=")), "hello", "tolerates padding");
eq(b64url_encode([251, 255]), "-_8", "uses the URL-safe alphabet");
eq(b64url_encode(bytes("")), "", "empty input");

print("");
print("-- the aud claim is an ORIGIN, not the endpoint URL --");
eq(webpush_aud("https://fcm.googleapis.com/fcm/send/abc123"), "https://fcm.googleapis.com", "FCM");
eq(webpush_aud("https://updates.push.services.mozilla.com/wpush/v2/xyz"),
   "https://updates.push.services.mozilla.com", "Mozilla autopush");
eq(webpush_aud("https://host:8443/p/x"), "https://host:8443", "keeps a non-default port");
eq(webpush_aud("https://host:443/x"), "https://host", "drops the default port");
eq(webpush_aud("https://host"), "https://host", "bare origin");
eq(webpush_aud("http://insecure.example/x"), null, "rejects http");
eq(webpush_aud("ftp://nope/x"), null, "rejects a non-http scheme");

print("");
print("-- VAPID JWT (RFC 8292 / RFC 7515) --");
$jwt = webpush_jwt($kp.private_key, "https://fcm.googleapis.com", "mailto:dev@example.com", 1700000000);
ok($jwt != null, "jwt signs");
$parts = split($jwt, ".");
eq(len($parts), 3, "three dot-separated parts");
eq(frombytes(b64url_decode($parts[0])), "{\"typ\":\"JWT\",\"alg\":\"ES256\"}", "JOSE header");
$claims = json.parse(frombytes(b64url_decode($parts[1])));
eq($claims.aud, "https://fcm.googleapis.com", "aud claim");
eq($claims.exp, 1700000000, "exp claim is in seconds");
eq($claims.sub, "mailto:dev@example.com", "sub claim");
// ES256 is raw r||s, 64 octets — NOT a DER SEQUENCE.
eq(len(b64url_decode($parts[2])), 64, "signature is 64 raw octets (r||s, not DER)");
ok(!contains($jwt, "="), "jwt is unpadded");
ok(!contains($jwt, "+"), "jwt uses the URL-safe alphabet");

// Deterministic nonces (RFC 6979) make the same input produce the same token.
$jwt2 = webpush_jwt($kp.private_key, "https://fcm.googleapis.com", "mailto:dev@example.com", 1700000000);
eq($jwt, $jwt2, "signing is deterministic (RFC 6979)");

$hdr = webpush_vapid_header($kp.private_key, "https://fcm.googleapis.com", "mailto:dev@example.com", 1700000000);
eq(substr($hdr, 0, 8), "vapid t=", "Authorization header starts with vapid t=");
ok(contains($hdr, ", k=" + $kp.public_key), "header carries the public key in k=");

print("");
print("-- RFC 8291 message encryption --");
// A subscriber keypair: its public key is what a browser reports as p256dh.
$sub = webpush_keygen();
$auth = b64url_encode(randbytes(16));
$msg = "{\"head\":\"Hello\",\"body\":\"from Bantu\"}";

$body = webpush_encrypt($sub.public_key, $auth, $msg);
ok($body != null, "encrypt succeeds");
// 16 salt + 4 rs + 1 idlen + 65 keyid + payload + 1 delimiter + 16 tag
eq(len($body), 86 + len($msg) + 1 + 16, "body length = 86-octet header + record");
eq($body[20], 65, "keyid length is 65 (the ephemeral public key)");
eq($body[16], 0, "rs high byte");
eq($body[18], 16, "rs == 4096, big-endian");

$back = webpush_decrypt($sub.private_key, $auth, $body);
eq(frombytes($back), $msg, "decrypt round-trips");

// RFC 8291 section 2 requires a fresh salt AND ephemeral key per message.
$body2 = webpush_encrypt($sub.public_key, $auth, $msg);
ok(str($body) != str($body2), "two encryptions of the same message differ");
eq(frombytes(webpush_decrypt($sub.private_key, $auth, $body2)), $msg, "the second body also decrypts");

print("");
print("-- rejection --");
$other = webpush_keygen();
eq(webpush_decrypt($other.private_key, $auth, $body), null, "wrong subscriber key is rejected");
eq(webpush_decrypt($sub.private_key, b64url_encode(randbytes(16)), $body), null, "wrong auth secret is rejected");

$tampered = $body;
$tampered[len($tampered) - 1] = ($tampered[len($tampered) - 1] + 1) % 256;
eq(webpush_decrypt($sub.private_key, $auth, $tampered), null, "a tampered tag is rejected");

$flip = $body;
$flip[100] = ($flip[100] + 1) % 256;
eq(webpush_decrypt($sub.private_key, $auth, $flip), null, "a tampered ciphertext is rejected");

eq(webpush_encrypt("not-a-key", $auth, $msg), null, "rejects a malformed p256dh");
eq(webpush_encrypt(b64url_encode(randbytes(65)), $auth, $msg), null, "rejects an off-curve p256dh");

print("");
print("-- payload size limit --");
// 4096 total minus the 86-octet header, the delimiter and the tag.
$max = 4096 - 86 - 1 - 16;
$big = rep("x", $max);
$okBody = webpush_encrypt($sub.public_key, $auth, $big);
ok($okBody != null, "accepts the maximum payload (" + str($max) + " octets)");
eq(len($okBody), 4096, "maximum body is exactly 4096 octets");
eq(webpush_encrypt($sub.public_key, $auth, rep("x", $max + 1)), null, "rejects one octet too many");

print("");
print("-- binary payloads survive intact --");
// The transport bug this suite guards against: a body containing NUL used to be
// truncated by libcurl's strlen().
$binary = [0, 1, 2, 0, 255, 0, 128];
$bBody = webpush_encrypt($sub.public_key, $auth, $binary);
eq(str(webpush_decrypt($sub.private_key, $auth, $bBody)), str($binary), "NUL-containing payload round-trips");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
