// ════════════════════════════════════════════════════════════════════════
//  crypto_differential_test.b — proves the NATIVE digest accelerators are
//  byte-identical to the pure-Bantu reference implementations, over many
//  random inputs (differential fuzzing), and checks the native-only SHA-512
//  family against official vectors.
//
//  Run with a native-capable build:
//      bantu-src/compiler/build/bantu -q run tests/crypto_differential_test.b
//
//  If the interpreter lacks the accelerators (has_native == false), the
//  differential section is skipped with a notice — there is nothing to compare.
// ════════════════════════════════════════════════════════════════════════

include "../hash/hash.b" as hash;

$R = {"pass": 0, "fail": 0, "skip": 0};

def eq($got, $want, $name) {
    if ($got == $want) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else {
        $R.fail = $R.fail + 1;
        print("  FAIL  " + $name);
        print("          got:  " + str($got));
        print("          want: " + str($want));
    }
}

// A deterministic pseudo-random byte-list of length $n (LCG — for INPUTS only,
// never for anything security-relevant; the point is reproducible fuzz data).
$_seed = 2463534242;
def _rndbyte() {
    $_seed = add32(mul32($_seed, 1664525), 1013904223);
    return band(shr($_seed, 16), 255);
}
def rndBytes($n) {
    $out = [];
    $i = 0;
    while ($i < $n) { $out[len($out)] = _rndbyte(); $i = $i + 1; }
    return $out;
}

// Pure (un-delegated) reference digests, reaching into the module internals.
def pureSha256($b) { return tohex(hash._beBytes(hash._sha256_run($b, hash._SHA256_IV), 8)); }
def pureSha224($b) { return tohex(hash._beBytes(hash._sha256_run($b, hash._SHA224_IV), 7)); }
def pureSha1($b)   { return tohex(hash._beBytes(hash._sha1_run($b), 5)); }
def pureMd5($b)    { return tohex(hash._md5_bytes(hash._md5_run($b))); }


print("");
if (!has_native("sha256")) {
    $R.skip = $R.skip + 1;
    print("-- native accelerators absent: differential section SKIPPED --");
} else {
    print("-- differential: native == pure over random inputs & lengths --");
    // Cover boundary lengths around the 64-byte block + padding edges.
    $lens = [0, 1, 3, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 200, 1000];
    $li = 0;
    while ($li < len($lens)) {
        $n = $lens[$li];
        $b = rndBytes($n);
        eq(native_sha256($b), pureSha256($b), "sha256 len=" + str($n));
        eq(native_sha224($b), pureSha224($b), "sha224 len=" + str($n));
        eq(native_sha1($b),   pureSha1($b),   "sha1   len=" + str($n));
        eq(native_md5($b),    pureMd5($b),    "md5    len=" + str($n));
        $li = $li + 1;
    }
}

print("");
print("-- SHA-512 / SHA-384 official vectors (native-only) --");
if (!has_native("sha512")) {
    $R.skip = $R.skip + 1;
    print("  (skipped — no native sha512)");
} else {
    eq(hash.sha512(""),
       "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
       "sha512(\"\")");
    eq(hash.sha512("abc"),
       "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
       "sha512(\"abc\")");
    eq(hash.sha384("abc"),
       "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
       "sha384(\"abc\")");
    // multi-block message (>128 bytes) exercises the SHA-512 block loop.
    eq(hash.sha512("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
       "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
       "sha512(multi-block)");
    eq(hash.sha512_bytes("abc")[0], 221, "sha512_bytes first byte = 0xdd");
}

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail) + "   SKIP: " + str($R.skip));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
