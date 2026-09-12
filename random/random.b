// ════════════════════════════════════════════════════════════════════════
//  random.b — Bantu random-number module (Python `random` style)
//
//  A self-contained, SEEDABLE pseudo-random generator written entirely in the
//  Bantu language, mirroring Python's `random` module: random(), uniform(),
//  randint(), randrange(), choice(), choices(), sample(), shuffle(), gauss(),
//  expovariate(), triangular(), getrandbits(), and more.
//
//  Include it namespaced:
//      include "./random.b" as random;
//      random.seed(42);                 // reproducible runs (like Python)
//      print(str(random.randint(1, 6)));
//      print(str(random.uniform(0, 1)));
//      $pick = random.choice(["a", "b", "c"]);
//      $hand = random.sample([1,2,3,4,5,6,7], 5);
//
//  ENGINE (why seed() works): Bantu's built-in random() is a Mersenne-Twister
//  that CANNOT be seeded from Bantu, so this module owns its own generator — a
//  classic 32-bit Linear Congruential Generator (Numerical Recipes constants):
//      state = (1664525 * state + 1013904223) mod 2^32
//  The largest intermediate value (1664525 * (2^32-1) + 1013904223 ≈ 7.15e15)
//  stays below 2^53, so every step is EXACT in Bantu's double arithmetic and
//  the sequence is perfectly reproducible after seed().
//
//  State lives in a module-level DICT so mutation is reference-shared across
//  calls (plain $variables are copied by value in Bantu; dicts are not).
//
//  QUALITY: LCG-grade — excellent for games, simulation, sampling, shuffling,
//  and deterministic tests. NOT cryptographically secure; do not use it to
//  generate keys, tokens, or password salts.
//
//  NOTE: shuffle() RETURNS a shuffled copy rather than mutating in place,
//  because Bantu passes lists by value (Python's in-place shuffle can't be
//  reproduced). Use  $x = random.shuffle($x);  if you want to replace it.
//
//  PERFORMANCE NOTE (current interpreter, v1.3.0): a single call is always
//  fast, even with a large input. However, calling a function that runs an
//  INTERNAL loop — choices(), choicesUniform(), sample(), shuffle() — many
//  hundreds of times in a tight loop degrades super-linearly (a per-call
//  overhead accumulates for looping functions defined in an included module).
//  Prefer ONE call with a large k (e.g. random.choices(pop, w, 1000)) over
//  1000 separate calls; for repeated single picks in a hot loop use choice(),
//  which has no internal loop. The loop-free functions (random, uniform,
//  randint, randbelow, randrange, gauss, expovariate, getrandbits, …) are
//  fast at any call volume.
// ════════════════════════════════════════════════════════════════════════


// ─── Engine constants & state ───────────────────────────────────────────
$_A   = 1664525;          // LCG multiplier
$_C   = 1013904223;       // LCG increment
$_M   = 4294967296;       // modulus = 2^32
$_TAU = 6.283185307179586;   // 2 * pi (for Box-Muller)

// State is a dict so functions can mutate it (dicts are reference-shared).
$_rng = {"state": 1};


// ─── Core generator ─────────────────────────────────────────────────────

// _next() → advance the LCG and return the new 32-bit state (0 .. 2^32-1).
def _next() {
    $x = $_A * $_rng.state + $_C;
    $q = floor($x / $_M);
    $r = $x - $q * $_M;
    // guard against a floor/division edge landing on the boundary
    if ($r >= $_M) { $r = $r - $_M; }
    if ($r < 0) { $r = $r + $_M; }
    $_rng.state = $r;
    return $r;
}

// _random01() → a float in [0.0, 1.0).
def _random01() {
    return _next() / $_M;
}

// _below($n) → an integer in [0, n)  (n assumed > 0).
def _below($n) {
    return floor(_random01() * $n);
}


// ─── Seeding & state ────────────────────────────────────────────────────

// seed($n) → seed the generator for reproducible sequences (like Python).
// Any number works; it is folded into [0, 2^32).
def seed($n) {
    $_rng.state = floor(abs($n)) % $_M;
    return $_rng.state;
}

// getstate() → the current internal state (a number).
def getstate() {
    return $_rng.state;
}

// setstate($s) → restore a state previously read with getstate().
def setstate($s) {
    $_rng.state = $s;
    return $s;
}


// ─── Uniform reals & integers ───────────────────────────────────────────

// random() → a float in [0.0, 1.0).
def random() {
    return _random01();
}

// uniform($a, $b) → a float in [a, b].
def uniform($a, $b) {
    return $a + ($b - $a) * _random01();
}

// randint($a, $b) → an integer in [a, b]  (both ends inclusive).
def randint($a, $b) {
    return $a + _below($b - $a + 1);
}

// randbelow($n) → an integer in [0, n).  Returns 0 for n <= 0.
def randbelow($n) {
    if ($n <= 0) { return 0; }
    return _below($n);
}

// randrange($start, $stop) → an integer in [start, stop).
def randrange($start, $stop) {
    return $start + _below($stop - $start);
}

// randrangeStep($start, $stop, $step) → an integer from the arithmetic
// sequence start, start+step, ... that is < stop.
def randrangeStep($start, $stop, $step) {
    $count = ceil(($stop - $start) / $step);
    return $start + $step * _below($count);
}

// randbool($p) → true with probability p (0..1).
def randbool($p) {
    return _random01() < $p;
}


// ─── Choosing from sequences ────────────────────────────────────────────

// choice($seq) → one element chosen uniformly at random.
def choice($seq) {
    return $seq[_below(len($seq))];
}

// choicesUniform($pop, $k) → a list of k elements chosen WITH replacement,
// uniformly.
def choicesUniform($pop, $k) {
    $out = [];
    $i = 0;
    while ($i < $k) {
        $out[len($out)] = choice($pop);
        $i = $i + 1;
    }
    return $out;
}

// choices($pop, $weights, $k) → a list of k elements chosen WITH replacement,
// weighted by $weights (a parallel list of relative weights). Pass [] for
// $weights to fall back to a uniform choice.
def choices($pop, $weights, $k) {
    if (len($weights) == 0) {
        return choicesUniform($pop, $k);
    }
    // build cumulative weights
    $cum = [];
    $total = 0;
    $i = 0;
    while ($i < len($weights)) {
        $total = $total + $weights[$i];
        $cum[len($cum)] = $total;
        $i = $i + 1;
    }
    $out = [];
    $j = 0;
    while ($j < $k) {
        $r = _random01() * $total;
        $idx = len($cum) - 1;   // default to the last bucket
        $found = false;
        $m = 0;
        while ($m < len($cum)) {
            if (!$found) {
                if ($r < $cum[$m]) { $idx = $m; $found = true; }
            }
            $m = $m + 1;
        }
        $out[len($out)] = $pop[$idx];
        $j = $j + 1;
    }
    return $out;
}

// sample($pop, $k) → a list of k DISTINCT elements (WITHOUT replacement).
// Works on a value-copy of $pop via a partial Fisher-Yates shuffle, so the
// caller's list is untouched. k is clamped to len(pop).
def sample($pop, $k) {
    $work = $pop;            // value copy
    $n = len($work);
    if ($k > $n) { $k = $n; }
    $out = [];
    $i = 0;
    while ($i < $k) {
        $j = $i + _below($n - $i);
        $tmp = $work[$i];
        $work[$i] = $work[$j];
        $work[$j] = $tmp;
        $out[len($out)] = $work[$i];
        $i = $i + 1;
    }
    return $out;
}

// shuffle($seq) → a shuffled COPY of $seq (Fisher-Yates). The input is not
// mutated (Bantu lists are passed by value). Use $x = random.shuffle($x);
def shuffle($seq) {
    $work = $seq;           // value copy
    $n = len($work);
    $i = $n - 1;
    while ($i > 0) {
        $j = _below($i + 1);
        $tmp = $work[$i];
        $work[$i] = $work[$j];
        $work[$j] = $tmp;
        $i = $i - 1;
    }
    return $work;
}


// ─── Real-valued distributions ──────────────────────────────────────────

// gauss($mu, $sigma) → a normally-distributed float (Box-Muller transform).
def gauss($mu, $sigma) {
    $u1 = _random01();
    if ($u1 <= 0.0) { $u1 = 0.0000000001; }   // avoid log(0)
    $u2 = _random01();
    $mag = sqrt(0 - 2 * log($u1));
    $z = $mag * cos($_TAU * $u2);
    return $mu + $sigma * $z;
}

// normalvariate($mu, $sigma) → alias of gauss().
def normalvariate($mu, $sigma) {
    return gauss($mu, $sigma);
}

// expovariate($lambd) → an exponentially-distributed float with rate $lambd
// (mean 1/lambd).
def expovariate($lambd) {
    $u = _random01();
    $val = 0 - log(1 - $u);   // 1-u is in (0, 1], so log is defined
    return $val / $lambd;
}

// triangular($low, $high, $mode) → a float from a triangular distribution.
def triangular($low, $high, $mode) {
    $u = _random01();
    $c = ($mode - $low) / ($high - $low);
    if ($u > $c) {
        $u = 1 - $u;
        $c = 1 - $c;
        $tmp = $low;
        $low = $high;
        $high = $tmp;
    }
    return $low + ($high - $low) * sqrt($u * $c);
}


// ─── Bits ───────────────────────────────────────────────────────────────

// getrandbits($k) → a non-negative integer with k random bits, in [0, 2^k).
// Loop-free (a single draw scaled by 2^k) so it stays fast under heavy repeated
// use. Meaningful for k up to ~52 (Bantu numbers are doubles); the generator
// carries ~32 bits of state, so very large k gains resolution but not entropy.
def getrandbits($k) {
    return floor(_random01() * pow(2, $k));
}


// ─── Auto-seed ──────────────────────────────────────────────────────────
// Seed from the wall clock so runs differ by default; call seed(n) yourself
// for reproducible sequences.
seed(clock());
