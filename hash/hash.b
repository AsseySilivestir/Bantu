// ════════════════════════════════════════════════════════════════════════
//  hash.b — Bantu hashing module (MD5, SHA-1, SHA-224/256, HMAC-SHA256)
//
//  The algorithms are written entirely in Bantu, on top of the interpreter's
//  native 32-bit primitives (band/bor/bxor/bnot/shl/shr/rotl/rotr/add32/mul32,
//  bytes/tohex). Digests are bit-exact to the official RFC/NIST test vectors.
//
//  Include it namespaced:
//      include "./hash.b" as hash;
//      print(hash.sha256("abc"));                 // ba7816bf…f20015ad
//      print(hash.md5("abc"));                    // 900150983cd24fb0…
//      print(hash.hmac_sha256("key", "message")); // hex MAC
//
//  Every function accepts a string OR a byte-list (list of 0..255). The default
//  form returns a lowercase hex string; the *_bytes form returns a byte-list.
//
//  ⚠ SECURITY: MD5 and SHA-1 are CRYPTOGRAPHICALLY BROKEN (practical
//  collisions). Use them ONLY for checksums, legacy interop, or RFC-4122 UUID
//  namespaces — NEVER for integrity against an adversary, digital signatures,
//  passwords, or deduplaction of untrusted data. Use SHA-256 / HMAC-SHA256
//  for anything security-relevant.
// ════════════════════════════════════════════════════════════════════════


// ─── Native accelerator detection ───────────────────────────────────────
// When the interpreter ships the native (C++) digest accelerators, the public
// hashes below delegate to them — byte-identical to the pure-Bantu code but
// ~10^5x faster. We probe ONCE at load, inside try/catch, so this module also
// runs correctly on an older interpreter that lacks `has_native` (it simply
// uses the pure-Bantu path). SHA-384/512 are native-ONLY (float64 can't hold
// 64-bit words) and return null when the accelerator is absent.
$_NATIVE_OK = false;
try { $_NATIVE_OK = has_native("sha256"); } catch ($e) { $_NATIVE_OK = false; }

// _hasNative($name) → is a specific native accelerator available?
// Short-circuits on the once-probed flag so old interpreters never call the
// (possibly undefined) has_native, and new ones pay no per-call try/catch cost.
def _hasNative($name) {
    if (!$_NATIVE_OK) { return false; }
    return has_native($name);
}


// ─── Insecure-primitive guard (C5) ──────────────────────────────────────
// MD5 and SHA-1 are collision-broken. We emit a ONE-TIME notice to stderr the
// first time each is used as a full message digest, steering callers to
// SHA-256/HMAC — without breaking output (stderr, not stdout) and without
// nagging (once per process). The notice fires only on the public string
// digests (md5/sha1/*_hex), NOT on the *_bytes cores, so RFC-4122 UUID v3/v5
// (which legitimately need MD5/SHA-1 over a namespace) stay silent.
// Silence it entirely with:  hash.allow_insecure(true);
// State lives in a dict: functions mutate module state via FIELD assignment
// (which targets the object), since a plain `$x = ...` inside a function would
// bind a function-local instead of the module var (function-local scoping).
$_INSEC = {"allow": false, "md5": false, "sha1": false};

// Opt out of the MD5/SHA-1 deprecation notices (e.g. for checksum tooling).
def allow_insecure($yes) { $_INSEC.allow = $yes; }

def _warnInsecure($algo) {
    if ($_INSEC.allow) { return null; }
    if ($_INSEC[$algo]) { return null; }
    $_INSEC[$algo] = true;
    // eprint may be absent on very old interpreters; degrade silently.
    try {
        eprint("[hash] WARNING: " + $algo + " is cryptographically broken; " +
               "use sha256/hmac_sha256 for security. " +
               "(hash.allow_insecure(true) silences this.)");
    } catch ($e) { }
    return null;
}


// ─── Constant tables (parsed once at load) ──────────────────────────────

// _wordsFromHex($hex) → list of 32-bit words (big-endian grouping of the bytes).
def _wordsFromHex($hex) {
    $b = fromhex($hex);
    $words = [];
    $i = 0;
    while ($i < len($b)) {
        $words[len($words)] = $b[$i] * 16777216 + $b[$i + 1] * 65536 + $b[$i + 2] * 256 + $b[$i + 3];
        $i = $i + 4;
    }
    return $words;
}

$_SHA256_K = _wordsFromHex("428a2f9871374491b5c0fbcfe9b5dba53956c25b59f111f1923f82a4ab1c5ed5d807aa9812835b01243185be550c7dc372be5d7480deb1fe9bdc06a7c19bf174e49b69c1efbe47860fc19dc6240ca1cc2de92c6f4a7484aa5cb0a9dc76f988da983e5152a831c66db00327c8bf597fc7c6e00bf3d5a7914706ca635114292967 27b70a852e1b21384d2c6dfc53380d13650a7354766a0abb81c2c92e92722c85a2bfe8a1a81a664bc24b8b70c76c51a3d192e819d6990624f40e3585106aa07019a4c1161e376c082748774c34b0bcb5391c0cb34ed8aa4a5b9cca4f682e6ff3748f82ee78a5636f84c878148cc7020890befffaa4506cebbef9a3f7c67178f2");

$_SHA256_IV = _wordsFromHex("6a09e667bb67ae853c6ef372a54ff53a510e527f9b05688c1f83d9ab5be0cd19");
$_SHA224_IV = _wordsFromHex("c1059ed8367cd5073070dd17f70e5939ffc00b316858151164f98fa7befa4fa4");

$_MD5_K = _wordsFromHex("d76aa478e8c7b756242070dbc1bdceeef57c0faf4787c62aa8304613fd469501698098d88b44f7afffff5bb1895cd7be6b901122fd987193a679438e49b40821f61e2562c040b340265e5a51e9b6c7aad62f105d02441453d8a1e681e7d3fbc821e1cde6c33707d6f4d50d87455a14eda9e3e905fcefa3f8676f02d98d2a4c8afffa39428771f6816d9d6122fde5380ca4beea444bdecfa9f6bb4b60bebfbc70289b7ec6eaa127fad4ef308504881d05d9d4d039e6db99e51fa27cf8c4ac5665f4292244432aff97ab9423a7fc93a039655b59c38f0ccc92ffeff47d85845dd16fa87e4ffe2ce6e0a30143144e0811a1f7537e82bd3af2352ad7d2bbeb86d391");

$_MD5_S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
           5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
           4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
           6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];

$_SHA1_IV = [1732584193, 4023233417, 2562383102, 271733878, 3285377520];
$_MD5_IV  = [1732584193, 4023233417, 2562383102, 271733878];


// ─── Shared helpers ─────────────────────────────────────────────────────

// _toBytes($input) → a byte-list (list of 0..255). Strings are UTF-8/raw bytes.
def _toBytes($input) {
    if (type($input) == "string") { return bytes($input); }
    return $input;
}

// _cat($a, $b) → a new list = a followed by b (inputs untouched).
def _cat($a, $b) {
    $r = $a;
    extend($r, $b);
    return $r;
}

// _padMsg($msg, $bigEndianLen) → message padded to a multiple of 64 bytes.
// Appends 0x80, then zeros, then the 64-bit bit-length (big-endian if the flag
// is true, little-endian for MD5). Runs once per digest.
def _padMsg($msg, $bigEndian) {
    $m = $msg;
    $bitlen = len($msg) * 8;
    $m[len($m)] = 128;
    while ((len($m) % 64) != 56) { $m[len($m)] = 0; }
    $hi = floor($bitlen / 4294967296);
    $lo = $bitlen - $hi * 4294967296;
    if ($bigEndian) {
        $m[len($m)] = band(shr($hi, 24), 255);
        $m[len($m)] = band(shr($hi, 16), 255);
        $m[len($m)] = band(shr($hi, 8), 255);
        $m[len($m)] = band($hi, 255);
        $m[len($m)] = band(shr($lo, 24), 255);
        $m[len($m)] = band(shr($lo, 16), 255);
        $m[len($m)] = band(shr($lo, 8), 255);
        $m[len($m)] = band($lo, 255);
    } else {
        $m[len($m)] = band($lo, 255);
        $m[len($m)] = band(shr($lo, 8), 255);
        $m[len($m)] = band(shr($lo, 16), 255);
        $m[len($m)] = band(shr($lo, 24), 255);
        $m[len($m)] = band($hi, 255);
        $m[len($m)] = band(shr($hi, 8), 255);
        $m[len($m)] = band(shr($hi, 16), 255);
        $m[len($m)] = band(shr($hi, 24), 255);
    }
    return $m;
}

// _beBytes($words, $n) → first $n words as big-endian bytes.
def _beBytes($words, $n) {
    $out = [];
    $i = 0;
    while ($i < $n) {
        $w = $words[$i];
        $out[len($out)] = band(shr($w, 24), 255);
        $out[len($out)] = band(shr($w, 16), 255);
        $out[len($out)] = band(shr($w, 8), 255);
        $out[len($out)] = band($w, 255);
        $i = $i + 1;
    }
    return $out;
}


// ─── SHA-256 / SHA-224 ──────────────────────────────────────────────────

// _sha256_run($bytes, $IV) → final 8-word state. Hot loops are branchless and
// call only native ops, so repeated hashing stays fast.
def _sha256_run($msg, $IV) {
    $m = _padMsg($msg, true);
    $h0 = $IV[0]; $h1 = $IV[1]; $h2 = $IV[2]; $h3 = $IV[3];
    $h4 = $IV[4]; $h5 = $IV[5]; $h6 = $IV[6]; $h7 = $IV[7];

    $total = len($m);
    $off = 0;
    while ($off < $total) {
        $W = [];
        $i = 0;
        while ($i < 16) {
            $j = $off + $i * 4;
            $W[$i] = $m[$j] * 16777216 + $m[$j + 1] * 65536 + $m[$j + 2] * 256 + $m[$j + 3];
            $i = $i + 1;
        }
        $i = 16;
        while ($i < 64) {
            $w15 = $W[$i - 15];
            $s0 = bxor(bxor(rotr($w15, 7), rotr($w15, 18)), shr($w15, 3));
            $w2 = $W[$i - 2];
            $s1 = bxor(bxor(rotr($w2, 17), rotr($w2, 19)), shr($w2, 10));
            $W[$i] = add32(add32(add32($W[$i - 16], $s0), $W[$i - 7]), $s1);
            $i = $i + 1;
        }

        $a = $h0; $b = $h1; $c = $h2; $d = $h3;
        $e = $h4; $f = $h5; $g = $h6; $hh = $h7;
        $i = 0;
        while ($i < 64) {
            $S1 = bxor(bxor(rotr($e, 6), rotr($e, 11)), rotr($e, 25));
            $ch = bxor(band($e, $f), band(bnot($e), $g));
            $t1 = add32(add32(add32(add32($hh, $S1), $ch), $_SHA256_K[$i]), $W[$i]);
            $S0 = bxor(bxor(rotr($a, 2), rotr($a, 13)), rotr($a, 22));
            $maj = bxor(bxor(band($a, $b), band($a, $c)), band($b, $c));
            $t2 = add32($S0, $maj);
            $hh = $g; $g = $f; $f = $e; $e = add32($d, $t1);
            $d = $c; $c = $b; $b = $a; $a = add32($t1, $t2);
            $i = $i + 1;
        }
        $h0 = add32($h0, $a); $h1 = add32($h1, $b); $h2 = add32($h2, $c); $h3 = add32($h3, $d);
        $h4 = add32($h4, $e); $h5 = add32($h5, $f); $h6 = add32($h6, $g); $h7 = add32($h7, $hh);
        $off = $off + 64;
    }
    return [$h0, $h1, $h2, $h3, $h4, $h5, $h6, $h7];
}

// Public SHA-256/224. When the interpreter ships the native accelerator we
// delegate to it (bit-identical, ~10^5x faster); otherwise we run the pure
// reference above. Delegating in the *_bytes form accelerates the hex form and
// any caller that needs raw bytes (e.g. uuid5) alike.
def sha256_bytes($input) {
    if (_hasNative("sha256")) { return fromhex(native_sha256($input)); }
    return _beBytes(_sha256_run(_toBytes($input), $_SHA256_IV), 8);
}
def sha256($input)       { return tohex(sha256_bytes($input)); }
def sha256_hex($input)   { return tohex(sha256_bytes($input)); }

def sha224_bytes($input) {
    if (_hasNative("sha224")) { return fromhex(native_sha224($input)); }
    return _beBytes(_sha256_run(_toBytes($input), $_SHA224_IV), 7);
}
def sha224($input)       { return tohex(sha224_bytes($input)); }
def sha224_hex($input)   { return tohex(sha224_bytes($input)); }


// ─── SHA-1 ──────────────────────────────────────────────────────────────

def _sha1_run($msg) {
    $m = _padMsg($msg, true);
    $h0 = $_SHA1_IV[0]; $h1 = $_SHA1_IV[1]; $h2 = $_SHA1_IV[2]; $h3 = $_SHA1_IV[3]; $h4 = $_SHA1_IV[4];

    $total = len($m);
    $off = 0;
    while ($off < $total) {
        $W = [];
        $i = 0;
        while ($i < 16) {
            $j = $off + $i * 4;
            $W[$i] = $m[$j] * 16777216 + $m[$j + 1] * 65536 + $m[$j + 2] * 256 + $m[$j + 3];
            $i = $i + 1;
        }
        $i = 16;
        while ($i < 80) {
            $W[$i] = rotl(bxor(bxor(bxor($W[$i - 3], $W[$i - 8]), $W[$i - 14]), $W[$i - 16]), 1);
            $i = $i + 1;
        }
        $a = $h0; $b = $h1; $c = $h2; $d = $h3; $e = $h4;

        // Phase 1: rounds 0..19
        $i = 0;
        while ($i < 20) {
            $f = bor(band($b, $c), band(bnot($b), $d));
            $t = add32(add32(add32(add32(rotl($a, 5), $f), $e), 1518500249), $W[$i]);
            $e = $d; $d = $c; $c = rotl($b, 30); $b = $a; $a = $t;
            $i = $i + 1;
        }
        // Phase 2: rounds 20..39
        while ($i < 40) {
            $f = bxor(bxor($b, $c), $d);
            $t = add32(add32(add32(add32(rotl($a, 5), $f), $e), 1859775393), $W[$i]);
            $e = $d; $d = $c; $c = rotl($b, 30); $b = $a; $a = $t;
            $i = $i + 1;
        }
        // Phase 3: rounds 40..59
        while ($i < 60) {
            $f = bor(bor(band($b, $c), band($b, $d)), band($c, $d));
            $t = add32(add32(add32(add32(rotl($a, 5), $f), $e), 2400959708), $W[$i]);
            $e = $d; $d = $c; $c = rotl($b, 30); $b = $a; $a = $t;
            $i = $i + 1;
        }
        // Phase 4: rounds 60..79
        while ($i < 80) {
            $f = bxor(bxor($b, $c), $d);
            $t = add32(add32(add32(add32(rotl($a, 5), $f), $e), 3395469782), $W[$i]);
            $e = $d; $d = $c; $c = rotl($b, 30); $b = $a; $a = $t;
            $i = $i + 1;
        }
        $h0 = add32($h0, $a); $h1 = add32($h1, $b); $h2 = add32($h2, $c);
        $h3 = add32($h3, $d); $h4 = add32($h4, $e);
        $off = $off + 64;
    }
    return [$h0, $h1, $h2, $h3, $h4];
}

def sha1_bytes($input) {
    if (_hasNative("sha1")) { return fromhex(native_sha1($input)); }
    return _beBytes(_sha1_run(_toBytes($input)), 5);
}
def sha1($input)       { _warnInsecure("sha1"); return tohex(sha1_bytes($input)); }
def sha1_hex($input)   { _warnInsecure("sha1"); return tohex(sha1_bytes($input)); }


// ─── MD5 ────────────────────────────────────────────────────────────────

// _leWord($m, $j) → little-endian 32-bit word from 4 bytes at offset $j.
def _leWord($m, $j) {
    return $m[$j] + $m[$j + 1] * 256 + $m[$j + 2] * 65536 + $m[$j + 3] * 16777216;
}

def _md5_run($msg) {
    $m = _padMsg($msg, false);
    $a0 = $_MD5_IV[0]; $b0 = $_MD5_IV[1]; $c0 = $_MD5_IV[2]; $d0 = $_MD5_IV[3];

    $total = len($m);
    $off = 0;
    while ($off < $total) {
        $M = [];
        $i = 0;
        while ($i < 16) {
            $M[$i] = _leWord($m, $off + $i * 4);
            $i = $i + 1;
        }
        $A = $a0; $B = $b0; $C = $c0; $D = $d0;

        // Round 1: i 0..15, F = (B&C)|(~B&D), g = i
        $i = 0;
        while ($i < 16) {
            $F = bor(band($B, $C), band(bnot($B), $D));
            $F = add32(add32(add32($F, $A), $_MD5_K[$i]), $M[$i]);
            $A = $D; $D = $C; $C = $B;
            $B = add32($B, rotl($F, $_MD5_S[$i]));
            $i = $i + 1;
        }
        // Round 2: i 16..31, F = (D&B)|(~D&C), g = (5i+1)%16
        while ($i < 32) {
            $g = (5 * $i + 1) % 16;
            $F = bor(band($D, $B), band(bnot($D), $C));
            $F = add32(add32(add32($F, $A), $_MD5_K[$i]), $M[$g]);
            $A = $D; $D = $C; $C = $B;
            $B = add32($B, rotl($F, $_MD5_S[$i]));
            $i = $i + 1;
        }
        // Round 3: i 32..47, F = B^C^D, g = (3i+5)%16
        while ($i < 48) {
            $g = (3 * $i + 5) % 16;
            $F = bxor(bxor($B, $C), $D);
            $F = add32(add32(add32($F, $A), $_MD5_K[$i]), $M[$g]);
            $A = $D; $D = $C; $C = $B;
            $B = add32($B, rotl($F, $_MD5_S[$i]));
            $i = $i + 1;
        }
        // Round 4: i 48..63, F = C^(B|~D), g = (7i)%16
        while ($i < 64) {
            $g = (7 * $i) % 16;
            $F = bxor($C, bor($B, bnot($D)));
            $F = add32(add32(add32($F, $A), $_MD5_K[$i]), $M[$g]);
            $A = $D; $D = $C; $C = $B;
            $B = add32($B, rotl($F, $_MD5_S[$i]));
            $i = $i + 1;
        }
        $a0 = add32($a0, $A); $b0 = add32($b0, $B); $c0 = add32($c0, $C); $d0 = add32($d0, $D);
        $off = $off + 64;
    }
    return [$a0, $b0, $c0, $d0];
}

// MD5 output is little-endian per word.
def _md5_bytes($words) {
    $out = [];
    $i = 0;
    while ($i < 4) {
        $w = $words[$i];
        $out[len($out)] = band($w, 255);
        $out[len($out)] = band(shr($w, 8), 255);
        $out[len($out)] = band(shr($w, 16), 255);
        $out[len($out)] = band(shr($w, 24), 255);
        $i = $i + 1;
    }
    return $out;
}

def md5_bytes($input) {
    if (_hasNative("md5")) { return fromhex(native_md5($input)); }
    return _md5_bytes(_md5_run(_toBytes($input)));
}
def md5($input)       { _warnInsecure("md5"); return tohex(md5_bytes($input)); }
def md5_hex($input)   { _warnInsecure("md5"); return tohex(md5_bytes($input)); }


// ─── HMAC-SHA256 (RFC 2104) ─────────────────────────────────────────────

def hmac_sha256_bytes($key, $msg) {
    if (_hasNative("hmac_sha256")) { return fromhex(native_hmac_sha256($key, $msg)); }
    $kb = _toBytes($key);
    $mb = _toBytes($msg);
    if (len($kb) > 64) { $kb = sha256_bytes($kb); }
    while (len($kb) < 64) { $kb[len($kb)] = 0; }   // right-pad to block size

    $ipad = [];
    $opad = [];
    $i = 0;
    while ($i < 64) {
        $ipad[$i] = bxor($kb[$i], 54);   // 0x36
        $opad[$i] = bxor($kb[$i], 92);   // 0x5c
        $i = $i + 1;
    }
    $inner = sha256_bytes(_cat($ipad, $mb));
    return sha256_bytes(_cat($opad, $inner));
}
def hmac_sha256($key, $msg)     { return tohex(hmac_sha256_bytes($key, $msg)); }
def hmac_sha256_hex($key, $msg) { return tohex(hmac_sha256_bytes($key, $msg)); }


// ─── SHA-512 / SHA-384 (native-only) ────────────────────────────────────
// The SHA-512 family uses 64-bit words, which Bantu's float64 numbers cannot
// represent exactly — so these are provided ONLY by the native accelerator
// (there is no pure-Bantu fallback). They are secure, modern digests; prefer
// them when you want a 512/384-bit output. If a build lacks the accelerator
// these return null, which callers can detect.
def sha512($input) {
    if (_hasNative("sha512")) { return native_sha512($input); }
    return null;
}
def sha512_hex($input)   { return sha512($input); }
def sha512_bytes($input) {
    if (_hasNative("sha512")) { return fromhex(native_sha512($input)); }
    return null;
}
def sha384($input) {
    if (_hasNative("sha384")) { return native_sha384($input); }
    return null;
}
def sha384_hex($input)   { return sha384($input); }
def sha384_bytes($input) {
    if (_hasNative("sha384")) { return fromhex(native_sha384($input)); }
    return null;
}


// ─── Non-cryptographic hashes (fast; for hashtables/checksums ONLY) ─────
// NOT collision-resistant. Do not use where security matters.

// djb2 — Dan Bernstein's string hash → 32-bit unsigned.
def djb2($input) {
    $b = _toBytes($input);
    $h = 5381;
    $i = 0;
    while ($i < len($b)) {
        $h = add32(mul32($h, 33), $b[$i]);
        $i = $i + 1;
    }
    return $h;
}

// fnv1a — 32-bit Fowler–Noll–Vo (offset 2166136261, prime 16777619).
def fnv1a($input) {
    $b = _toBytes($input);
    $h = 2166136261;
    $i = 0;
    while ($i < len($b)) {
        $h = mul32(bxor($h, $b[$i]), 16777619);
        $i = $i + 1;
    }
    return $h;
}


// ─── File hashing helper ────────────────────────────────────────────────

// hash_file($path, $algo) → hex digest of a file.
// $algo ∈ "md5"|"sha1"|"sha224"|"sha256"|"sha384"|"sha512".
// When the native accelerator is present, the file is read and digested
// entirely in C++ (native_hash_file), so large files never have to be
// materialized as a Bantu byte-list — this is what makes bulk file hashing
// usable. Falls back to reading + the pure digests otherwise.
def hash_file($path, $algo) {
    if (_hasNative("hash_file")) {
        $r = native_hash_file($path, $algo);
        if ($r == null) { return null; }   // missing file, unreadable, or unknown algo
        return $r;
    }
    // Pure-Bantu fallback (kept for builds without the accelerator).
    $data = readfile($path);
    if ($algo == "md5")    { return md5($data); }
    if ($algo == "sha1")   { return sha1($data); }
    if ($algo == "sha224") { return sha224($data); }
    if ($algo == "sha256") { return sha256($data); }
    return sha256($data);
}
