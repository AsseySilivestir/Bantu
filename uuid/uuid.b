// ════════════════════════════════════════════════════════════════════════
//  uuid.b — Bantu UUID module (RFC 4122 / RFC 9562)
//
//  Generates and inspects UUIDs, written in pure Bantu on the native CSPRNG
//  (randbytes) and the hash module (MD5/SHA-1 for name-based UUIDs).
//
//      include "./uuid.b" as uuid;
//      print(uuid.uuid4());                       // random
//      print(uuid.uuid7());                       // time-ordered (sortable)
//      print(uuid.uuid5(uuid.NAMESPACE_DNS, "python.org"));
//
//  - uuid4: random, from the OS CSPRNG (unpredictable).
//  - uuid7: time-ordered (48-bit ms timestamp + random) — sorts by creation
//           time; good for database primary keys.
//  - uuid3/uuid5: deterministic, namespace + name (MD5 / SHA-1 respectively).
// ════════════════════════════════════════════════════════════════════════

include "../hash/hash.b" as hash;

// RFC 4122 predefined namespaces.
$NAMESPACE_DNS  = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
$NAMESPACE_URL  = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";
$NAMESPACE_OID  = "6ba7b812-9dad-11d1-80b4-00c04fd430c8";
$NAMESPACE_X500 = "6ba7b814-9dad-11d1-80b4-00c04fd430c8";


// ─── helpers ────────────────────────────────────────────────────────────
def _bytesOf($x) {
    if (type($x) == "string") { return bytes($x); }
    return $x;
}
def _cat($a, $b) {
    $r = $a;
    extend($r, $b);
    return $r;
}

// A UUID (string with dashes, or a byte-list) → 16 raw bytes. fromhex ignores
// the dashes, so a canonical UUID string parses directly.
def _uuidToBytes($u) {
    if (type($u) == "string") { return fromhex($u); }
    return $u;
}

// First 16 bytes of a list (digests are ≥16 bytes).
def _first16($b) {
    $out = [];
    $i = 0;
    while ($i < 16) {
        $out[len($out)] = $b[$i];
        $i = $i + 1;
    }
    return $out;
}

// 16 bytes → canonical 8-4-4-4-12 string.
def _format($b) {
    $h = tohex($b);
    return substr($h, 0, 8) + "-" + substr($h, 8, 4) + "-" + substr($h, 12, 4) + "-" +
           substr($h, 16, 4) + "-" + substr($h, 20, 12);
}

// Stamp the version nibble (byte 6) and the RFC-4122 variant bits (byte 8).
def _stamp($b, $version) {
    $b[6] = bor(band($b[6], 15), $version * 16);   // version in the high nibble
    $b[8] = bor(band($b[8], 63), 128);             // variant 10xx
    return $b;
}


// ─── Generators ─────────────────────────────────────────────────────────

// uuid4() → a random UUID (version 4), from the OS CSPRNG.
def uuid4() {
    $b = _stamp(randbytes(16), 4);
    return _format($b);
}

// uuid7() → a time-ordered UUID (version 7): 48-bit Unix-ms timestamp in the
// high bytes, CSPRNG in the rest. Lexicographically sortable by creation time.
def uuid7() {
    $ms = floor(clock());
    $b = randbytes(16);
    $hi = floor($ms / 4294967296);       // top 16 bits of the 48-bit time
    $lo = $ms - $hi * 4294967296;        // low 32 bits
    $b[0] = band(shr($hi, 8), 255);
    $b[1] = band($hi, 255);
    $b[2] = band(shr($lo, 24), 255);
    $b[3] = band(shr($lo, 16), 255);
    $b[4] = band(shr($lo, 8), 255);
    $b[5] = band($lo, 255);
    $b = _stamp($b, 7);
    return _format($b);
}

// uuid5($namespace, $name) → deterministic name-based UUID (version 5, SHA-1).
def uuid5($namespace, $name) {
    $digest = hash.sha1_bytes(_cat(_uuidToBytes($namespace), _bytesOf($name)));
    return _format(_stamp(_first16($digest), 5));
}

// uuid3($namespace, $name) → deterministic name-based UUID (version 3, MD5).
def uuid3($namespace, $name) {
    $digest = hash.md5_bytes(_cat(_uuidToBytes($namespace), _bytesOf($name)));
    return _format(_stamp(_first16($digest), 3));
}


// ─── Inspection ─────────────────────────────────────────────────────────

// parse($s) → the 16 raw bytes of a UUID string.
def parse($s) { return _uuidToBytes($s); }

// format($bytes) → canonical string from 16 bytes.
def format($bytes) { return _format($bytes); }

// is_valid($s) → true if $s is a well-formed canonical UUID string.
def is_valid($s) {
    if (type($s) != "string") { return false; }
    if (len($s) != 36) { return false; }
    if (substr($s, 8, 1) != "-") { return false; }
    if (substr($s, 13, 1) != "-") { return false; }
    if (substr($s, 18, 1) != "-") { return false; }
    if (substr($s, 23, 1) != "-") { return false; }
    if (len(fromhex($s)) != 16) { return false; }   // exactly 32 hex digits
    return true;
}

// version_of($s) → the version digit (1..8), or -1 if not a hex digit.
def version_of($s) {
    $vc = substr($s, 14, 1);
    $p = indexOf("0123456789abcdef", $vc);
    if ($p < 0) { $p = indexOf("0123456789ABCDEF", $vc); }
    return $p;
}
