// ════════════════════════════════════════════════════════════════════════
//  crypto.b — Bantu cryptography module (secure random, HKDF, encoding)
//
//  Built on the native OS CSPRNG (randbytes) and the pure-Bantu hash module
//  (HMAC-SHA256). Everything security-sensitive draws from the CSPRNG — NEVER
//  from the `random` module (which is a predictable PRNG).
//
//  Include it namespaced:
//      include "./crypto.b" as crypto;
//      $token = crypto.token_urlsafe(32);          // a URL-safe secret
//      $mac   = crypto.hmac_sha256("k", "msg");    // hex MAC
//      $key   = crypto.hkdf_sha256($ikm, $salt, "context", 32);
//
//  ENCRYPTION & PASSWORDS: authenticated encryption (XChaCha20-Poly1305) and
//  password hashing (argon2id) are provided via a vetted native library
//  (libsodium) — see the "Authenticated encryption" section below. They are
//  available only when the interpreter was built with libsodium support
//  (crypto.encryption_available()); we never hand-roll these primitives.
// ════════════════════════════════════════════════════════════════════════

include "../hash/hash.b" as hash;

// ─── local helpers ──────────────────────────────────────────────────────
def _bytesOf($x) {
    if (type($x) == "string") { return bytes($x); }
    return $x;
}
def _cat($a, $b) {
    $r = $a;
    extend($r, $b);
    return $r;
}

$_B64    = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
$_B64URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";


// ─── Secure randomness (OS CSPRNG) ──────────────────────────────────────

// random_bytes($n) → list of n cryptographically-secure bytes.
def random_bytes($n) { return randbytes($n); }

// token_hex($n) → 2n-char hex string of n secure bytes (like Python secrets.token_hex).
def token_hex($n) { return tohex(randbytes($n)); }

// token_urlsafe($n) → URL-safe base64 (no padding) of n secure bytes.
def token_urlsafe($n) { return _b64enc(randbytes($n), $_B64URL, false); }

// random_int($max) → an unbiased integer in [0, max) using rejection sampling.
def random_int($max) {
    if ($max <= 1) { return 0; }
    $limit = floor(4294967296 / $max) * $max;
    $v = 4294967296;
    while ($v >= $limit) {
        $rb = randbytes(4);
        $v = $rb[0] * 16777216 + $rb[1] * 65536 + $rb[2] * 256 + $rb[3];
    }
    return $v % $max;
}


// ─── HMAC + verification ────────────────────────────────────────────────

def hmac_sha256($key, $msg)       { return hash.hmac_sha256($key, $msg); }
def hmac_sha256_bytes($key, $msg) { return hash.hmac_sha256_bytes($key, $msg); }

// verify_hmac($key, $msg, $tag) → bool, constant-time. $tag may be a hex string
// or a byte-list. Always use this (not ==) to check a MAC.
def verify_hmac($key, $msg, $tag) {
    $expected = hash.hmac_sha256_bytes($key, $msg);
    $actual = $tag;
    if (type($tag) == "string") { $actual = fromhex($tag); }
    return ct_equal($expected, $actual);
}


// ─── HKDF-SHA256 (RFC 5869) ─────────────────────────────────────────────

def hkdf_extract($salt, $ikm) {
    return hash.hmac_sha256_bytes($salt, $ikm);
}

def hkdf_expand($prk, $info, $length) {
    $infob = _bytesOf($info);
    $okm = [];
    $t = [];
    $counter = 1;
    while (len($okm) < $length) {
        $block = _cat(_cat($t, $infob), [$counter]);
        $t = hash.hmac_sha256_bytes($prk, $block);
        extend($okm, $t);
        $counter = $counter + 1;
    }
    // truncate to the requested length
    $out = [];
    $i = 0;
    while ($i < $length) {
        $out[len($out)] = $okm[$i];
        $i = $i + 1;
    }
    return $out;
}

// hkdf_sha256($ikm, $salt, $info, $length) → derived key as a byte-list.
def hkdf_sha256($ikm, $salt, $info, $length) {
    $prk = hkdf_extract(_bytesOf($salt), _bytesOf($ikm));
    return hkdf_expand($prk, $info, $length);
}
def hkdf_sha256_hex($ikm, $salt, $info, $length) {
    return tohex(hkdf_sha256($ikm, $salt, $info, $length));
}


// ─── Encoding: hex + base64 + base64url (RFC 4648) ──────────────────────

def hex_encode($x) { return tohex(_bytesOf($x)); }
def hex_decode($s) { return fromhex($s); }

// _b64enc($input, $alpha, $pad) → base64 with the given 64-char alphabet.
def _b64enc($input, $alpha, $pad) {
    $b = _bytesOf($input);
    $out = "";
    $n = len($b);
    $i = 0;
    while ($i + 3 <= $n) {
        $v = $b[$i] * 65536 + $b[$i + 1] * 256 + $b[$i + 2];
        $out = $out + substr($alpha, band(shr($v, 18), 63), 1);
        $out = $out + substr($alpha, band(shr($v, 12), 63), 1);
        $out = $out + substr($alpha, band(shr($v, 6), 63), 1);
        $out = $out + substr($alpha, band($v, 63), 1);
        $i = $i + 3;
    }
    $rem = $n - $i;
    if ($rem == 1) {
        $v = $b[$i] * 65536;
        $out = $out + substr($alpha, band(shr($v, 18), 63), 1);
        $out = $out + substr($alpha, band(shr($v, 12), 63), 1);
        if ($pad) { $out = $out + "=="; }
    }
    if ($rem == 2) {
        $v = $b[$i] * 65536 + $b[$i + 1] * 256;
        $out = $out + substr($alpha, band(shr($v, 18), 63), 1);
        $out = $out + substr($alpha, band(shr($v, 12), 63), 1);
        $out = $out + substr($alpha, band(shr($v, 6), 63), 1);
        if ($pad) { $out = $out + "="; }
    }
    return $out;
}

// _b64dec($str, $alpha) → decode base64/base64url into a byte-list. Ignores
// padding and any non-alphabet characters (whitespace, newlines).
def _b64dec($str, $alpha) {
    $out = [];
    $acc = 0;
    $bits = 0;
    $L = len($str);
    $i = 0;
    while ($i < $L) {
        $v = indexOf($alpha, substr($str, $i, 1));
        if ($v >= 0) {
            $acc = $acc * 64 + $v;
            $bits = $bits + 6;
            if ($bits >= 8) {
                $rembits = $bits - 8;
                $div = pow(2, $rembits);
                $byte = floor($acc / $div);
                $out[len($out)] = $byte % 256;
                $acc = $acc - $byte * $div;
                $bits = $rembits;
            }
        }
        $i = $i + 1;
    }
    return $out;
}

def base64_encode($x)    { return _b64enc($x, $_B64, true); }
def base64_decode($s)    { return _b64dec($s, $_B64); }
def base64url_encode($x) { return _b64enc($x, $_B64URL, false); }
def base64url_decode($s) { return _b64dec($s, $_B64URL); }


// ─── Authenticated encryption (libsodium; native, optional) ─────────────
// XChaCha20-Poly1305-IETF via libsodium. Confidentiality + integrity in one
// step: decrypt returns null (never garbage) if the key is wrong or the data
// was tampered with. A fresh 192-bit random nonce is generated per message, so
// you never manage nonces yourself. Requires an interpreter built with
// libsodium (see encryption_available); otherwise these throw a clear error.
//
//   $key = crypto.new_key();                      // 32-byte random key — STORE SAFELY
//   $tok = crypto.encrypt($key, "secret", "aad"); // base64url token (nonce+ct+tag)
//   $pt  = crypto.decrypt($key, $tok, "aad");     // byte-list, or null if invalid

// Probe once whether the native encryption layer is compiled in. Wrapped in
// try/catch so this module still loads on interpreters lacking the builtin.
$_ENC = {"ok": false};
try { $_ENC.ok = sodium_available(); } catch ($e) { $_ENC.ok = false; }

// encryption_available() → is native AEAD / password hashing usable here?
def encryption_available() { return $_ENC.ok; }

def _requireEnc() {
    if (!$_ENC.ok) {
        throw "crypto: encryption not available — rebuild the interpreter with libsodium (BANTU_SODIUM=1).";
    }
    return null;
}

// new_key() → a fresh 32-byte AEAD key (byte-list) from the OS CSPRNG.
def new_key() { return randbytes(32); }

// encrypt_bytes(key, message, aad?) → raw blob byte-list (nonce||ct||tag).
// key must be 32 bytes; aad (optional) is authenticated but not encrypted.
def encrypt_bytes($key, $message, $aad) {
    _requireEnc();
    $blob = aead_encrypt($key, $message, $aad);
    if ($blob == null) {
        throw "crypto.encrypt: invalid key (must be 32 bytes) or encryption failed.";
    }
    return $blob;
}

// decrypt_bytes(key, blob, aad?) → plaintext byte-list, or null if the data
// fails authentication (wrong key / tampered / truncated). Treat null as REJECT.
def decrypt_bytes($key, $blob, $aad) {
    _requireEnc();
    return aead_decrypt($key, $blob, $aad);
}

// encrypt(key, message, aad?) → base64url token string (safe to store/transmit).
def encrypt($key, $message, $aad) {
    return base64url_encode(encrypt_bytes($key, $message, $aad));
}

// decrypt(key, token, aad?) → plaintext byte-list, or null if invalid.
// Use frombytes(...) to get a string back when the plaintext is text.
def decrypt($key, $token, $aad) {
    _requireEnc();
    return aead_decrypt($key, base64url_decode($token), $aad);
}


// ─── Password hashing (argon2id via libsodium; native, optional) ────────
// Memory-hard, salted, self-describing. Store the string hash_password returns;
// verify_password checks a candidate in constant time. Requires libsodium.
//
//   $h  = crypto.hash_password($pw);       // "$argon2id$v=19$m=...,t=...,p=..$salt$hash"
//   $ok = crypto.verify_password($h, $pw); // true / false

// hash_password(password) → encoded argon2id hash string (store this).
def hash_password($password) {
    _requireEnc();
    $h = pwhash($password);
    if ($h == null) { throw "crypto.hash_password: hashing failed."; }
    return $h;
}

// verify_password(encoded, password) → bool. Constant-time comparison inside.
def verify_password($encoded, $password) {
    _requireEnc();
    return pwhash_verify($encoded, $password);
}
