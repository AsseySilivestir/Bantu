// ════════════════════════════════════════════════════════════════════════
//  random_test.b — unit tests for the Bantu random module.
//  Run:  bantu run random/random_test.b
//  A tiny assert harness is included (Bantu has no test framework yet).
// ════════════════════════════════════════════════════════════════════════

include "./random.b" as random;

// ─── Test harness ───────────────────────────────────────────────────────
// Counters live in a dict so mutation is reference-based and scope-proof.
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
    if ($cond) {
        $R.pass = $R.pass + 1;
        print("  ok    " + $name);
    } else {
        $R.fail = $R.fail + 1;
        print("  FAIL  " + $name);
    }
}

// ─── Small list helpers used by the assertions ──────────────────────────
// Flag-based (no early return inside the loop): returning from within a loop
// carries a per-call overhead in the current interpreter, which is costly when
// a helper is called many times in a hot loop.
def has($list, $v) {
    $found = false;
    $i = 0;
    while ($i < len($list)) {
        if ($list[$i] == $v) { $found = true; }
        $i = $i + 1;
    }
    return $found;
}

def allDistinct($l) {
    $i = 0;
    while ($i < len($l)) {
        $j = $i + 1;
        while ($j < len($l)) {
            if ($l[$i] == $l[$j]) { return false; }
            $j = $j + 1;
        }
        $i = $i + 1;
    }
    return true;
}

def sameElements($a, $b) {
    if (len($a) != len($b)) { return false; }
    $i = 0;
    while ($i < len($a)) {
        if (!has($b, $a[$i])) { return false; }
        $i = $i + 1;
    }
    return true;
}


// ════════════════════════════════════════════════════════════════════════
print("");
print("-- Reproducibility (seed) --");

random.seed(42);
$seqA = [];
$i = 0;
while ($i < 6) { $seqA[len($seqA)] = random._next(); $i = $i + 1; }

random.seed(42);
$seqB = [];
$i = 0;
while ($i < 6) { $seqB[len($seqB)] = random._next(); $i = $i + 1; }

eq(str($seqA), str($seqB), "same seed reproduces the raw sequence");

random.seed(7);
$x1 = random.random();
random.seed(7);
$x2 = random.random();
eq($x1, $x2, "same seed reproduces random()");

random.seed(7);
$y1 = random.random();
$y2 = random.random();
ok($y1 != $y2, "successive draws differ");

// getstate / setstate round-trip
random.seed(100);
$st = random.getstate();
$a = random.random();
random.setstate($st);
$b = random.random();
eq($a, $b, "setstate restores the stream");


print("");
print("-- Uniform reals --");

random.seed(1);
$oku = true;
$i = 0;
while ($i < 300) {
    $u = random.uniform(2, 3);
    if ($u < 2) { $oku = false; }
    if ($u > 3) { $oku = false; }
    $i = $i + 1;
}
ok($oku, "uniform(2,3) stays within [2,3]");

random.seed(123);
$sum = 0;
$i = 0;
while ($i < 2000) { $sum = $sum + random.random(); $i = $i + 1; }
$mean = $sum / 2000;
ok($mean > 0.45, "random() mean above 0.45");
ok($mean < 0.55, "random() mean below 0.55");


print("");
print("-- Integers --");

random.seed(2);
$okI = true;
$sawLo = false;
$sawHi = false;
$i = 0;
while ($i < 400) {
    $r = random.randint(1, 6);
    if ($r < 1) { $okI = false; }
    if ($r > 6) { $okI = false; }
    if ($r == 1) { $sawLo = true; }
    if ($r == 6) { $sawHi = true; }
    $i = $i + 1;
}
ok($okI, "randint(1,6) always in [1,6]");
ok($sawLo, "randint(1,6) can hit the low bound");
ok($sawHi, "randint(1,6) can hit the high bound");

random.seed(3);
$okB = true;
$i = 0;
while ($i < 300) {
    $r = random.randbelow(10);
    if ($r < 0) { $okB = false; }
    if ($r > 9) { $okB = false; }
    $i = $i + 1;
}
ok($okB, "randbelow(10) in [0,10)");

random.seed(4);
$okR = true;
$i = 0;
while ($i < 200) {
    $r = random.randrange(5, 10);
    if ($r < 5) { $okR = false; }
    if ($r > 9) { $okR = false; }
    $i = $i + 1;
}
ok($okR, "randrange(5,10) in [5,10)");

random.seed(5);
$okS = true;
$i = 0;
while ($i < 200) {
    $r = random.randrangeStep(0, 100, 10);   // 0,10,20,...,90
    if ($r < 0) { $okS = false; }
    if ($r > 90) { $okS = false; }
    if (($r % 10) != 0) { $okS = false; }
    $i = $i + 1;
}
ok($okS, "randrangeStep(0,100,10) yields multiples of 10 in range");


print("");
print("-- Choosing --");

// Membership checked inline (top-level ifs) rather than via a helper call in
// the hot loop — see the performance note; pool is [10,20,30,40,50].
random.seed(11);
$okC = true;
$seenLo = false;
$seenHi = false;
$i = 0;
while ($i < 200) {
    $c = random.choice([10, 20, 30, 40, 50]);
    if ($c < 10) { $okC = false; }
    if ($c > 50) { $okC = false; }
    if (($c % 10) != 0) { $okC = false; }
    if ($c == 10) { $seenLo = true; }
    if ($c == 50) { $seenHi = true; }
    $i = $i + 1;
}
ok($okC, "choice() always returns a pool member");
ok($seenLo, "choice() can return the first element");
ok($seenHi, "choice() can return the last element");

random.seed(12);
$smp = random.sample([1, 2, 3, 4, 5, 6, 7, 8], 4);
eq(len($smp), 4, "sample() returns k items");
ok(allDistinct($smp), "sample() items are distinct");

random.seed(13);
$orig = [1, 2, 3, 4, 5, 6];
$sh = random.shuffle($orig);
eq(len($sh), 6, "shuffle() preserves length");
ok(sameElements($orig, $sh), "shuffle() preserves the elements");
eq(str($orig), "[1, 2, 3, 4, 5, 6]", "shuffle() does not mutate the input (copy)");

// weighted choices should favor the heavy option.
// ONE call with a large k (fast); count the returned picks in a top-level loop.
random.seed(14);
$picks = random.choices(["a", "b"], [1, 9], 500);
eq(len($picks), 500, "choices() returns k picks");
$heavy = 0;
$light = 0;
$i = 0;
while ($i < len($picks)) {
    if ($picks[$i] == "b") { $heavy = $heavy + 1; }
    if ($picks[$i] == "a") { $light = $light + 1; }
    $i = $i + 1;
}
ok($heavy > $light, "weighted choices() favors the heavy option");

// choicesUniform: one call, k items, all drawn from the pool
random.seed(15);
$cu = random.choicesUniform([1, 2, 3], 60);
eq(len($cu), 60, "choicesUniform() returns k items");
$okCU = true;
$i = 0;
while ($i < len($cu)) {
    if ($cu[$i] < 1) { $okCU = false; }
    if ($cu[$i] > 3) { $okCU = false; }
    $i = $i + 1;
}
ok($okCU, "choicesUniform() items are from the pool");


print("");
print("-- Distributions --");

random.seed(99);
$gs = 0;
$i = 0;
while ($i < 2000) { $gs = $gs + random.gauss(0, 1); $i = $i + 1; }
$gm = $gs / 2000;
ok($gm > -0.15, "gauss(0,1) mean near 0 (lower)");
ok($gm < 0.15, "gauss(0,1) mean near 0 (upper)");

random.seed(55);
$es = 0;
$i = 0;
while ($i < 2000) { $es = $es + random.expovariate(1); $i = $i + 1; }
$em = $es / 2000;
ok($em > 0.85, "expovariate(1) mean near 1 (lower)");
ok($em < 1.15, "expovariate(1) mean near 1 (upper)");

random.seed(77);
$okT = true;
$i = 0;
while ($i < 300) {
    $t = random.triangular(0, 10, 5);
    if ($t < 0) { $okT = false; }
    if ($t > 10) { $okT = false; }
    $i = $i + 1;
}
ok($okT, "triangular(0,10,5) within [0,10]");


print("");
print("-- Bits --");

random.seed(8);
$okBits = true;
$limit = pow(2, 8);
$i = 0;
while ($i < 200) {
    $g = random.getrandbits(8);
    if ($g < 0) { $okBits = false; }
    if ($g >= $limit) { $okBits = false; }
    $i = $i + 1;
}
ok($okBits, "getrandbits(8) in [0, 256)");

random.seed(9);
$t = 0;
$i = 0;
while ($i < 1000) {
    if (random.randbool(0.5)) { $t = $t + 1; }
    $i = $i + 1;
}
ok($t > 400, "randbool(0.5) true-rate above 0.4");
ok($t < 600, "randbool(0.5) true-rate below 0.6");


// ─── Summary ────────────────────────────────────────────────────────────
print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
