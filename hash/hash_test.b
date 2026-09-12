// ════════════════════════════════════════════════════════════════════════
//  hash_test.b — official test-vector checks for the Bantu hash module.
//  Run:  bantu run hash/hash_test.b
//  Vectors: RFC 1321 (MD5), RFC 3174 (SHA-1), FIPS 180 / NIST (SHA-224/256),
//           RFC 4231 + Wikipedia (HMAC-SHA256).
// ════════════════════════════════════════════════════════════════════════

include "./hash.b" as hash;

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
print("-- MD5 (RFC 1321) --");
eq(hash.md5(""), "d41d8cd98f00b204e9800998ecf8427e", "md5 empty");
eq(hash.md5("a"), "0cc175b9c0f1b6a831c399e269772661", "md5 'a'");
eq(hash.md5("abc"), "900150983cd24fb0d6963f7d28e17f72", "md5 'abc'");
eq(hash.md5("message digest"), "f96b697d7cb7938d525a2f31aaf161d0", "md5 'message digest'");
eq(hash.md5("abcdefghijklmnopqrstuvwxyz"), "c3fcd3d76192e4007dfb496cca67e13b", "md5 a-z");

print("");
print("-- SHA-1 (RFC 3174) --");
eq(hash.sha1(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1 empty");
eq(hash.sha1("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1 'abc'");
eq(hash.sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
   "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "sha1 448-bit multi-block");

print("");
print("-- SHA-224 (FIPS 180) --");
eq(hash.sha224(""), "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f", "sha224 empty");
eq(hash.sha224("abc"), "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7", "sha224 'abc'");

print("");
print("-- SHA-256 (FIPS 180 / NIST) --");
eq(hash.sha256(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
eq(hash.sha256("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 'abc'");
eq(hash.sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
   "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256 448-bit multi-block");

print("");
print("-- byte-list input equals string input --");
eq(hash.sha256([97, 98, 99]), hash.sha256("abc"), "sha256 of [97,98,99] == sha256('abc')");
eq(hash.md5([97, 98, 99]), hash.md5("abc"), "md5 of [97,98,99] == md5('abc')");

print("");
print("-- HMAC-SHA256 (RFC 4231 / Wikipedia) --");
eq(hash.hmac_sha256("key", "The quick brown fox jumps over the lazy dog"),
   "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8", "hmac 'key'/'quick fox'");
eq(hash.hmac_sha256("Jefe", "what do ya want for nothing?"),
   "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "hmac RFC4231 case 2");
// long key (> 64 bytes) path: the key is hashed first
eq(hash.hmac_sha256("this is a key longer than sixty-four bytes so it gets hashed first!!", "msg"),
   hash.hmac_sha256(hash.sha256_bytes("this is a key longer than sixty-four bytes so it gets hashed first!!"), "msg"),
   "hmac long-key is pre-hashed");

print("");
print("-- non-crypto hashes --");
eq(hash.fnv1a(""), 2166136261, "fnv1a empty = offset basis");
eq(hash.fnv1a("a"), 3826002220, "fnv1a 'a' = 0xe40c292c");
ok(hash.djb2("hello") != hash.djb2("world"), "djb2 distinguishes inputs");
ok(hash.djb2("abc") >= 0, "djb2 is non-negative (u32)");
ok(hash.fnv1a("abc") < 4294967296, "fnv1a stays within 32 bits");

print("");
print("-- hash_file --");
writefile("/tmp/bantu_hash_probe.txt", "abc");
eq(hash.hash_file("/tmp/bantu_hash_probe.txt", "sha256"), hash.sha256("abc"), "hash_file sha256 matches string");
eq(hash.hash_file("/tmp/bantu_hash_probe.txt", "md5"), hash.md5("abc"), "hash_file md5 matches string");


print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
