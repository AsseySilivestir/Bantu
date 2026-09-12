# Bantu `random`

A small, Python-inspired random-number module written **entirely in the Bantu
language** (`random/random.b`). It provides the familiar `random` API — `random()`,
`uniform()`, `randint()`, `choice()`, `sample()`, `shuffle()`, `gauss()` and more —
on top of a **seedable** generator, so `random.seed(n)` reproduces sequences exactly,
just like Python.

- **Source:** [`random/random.b`](../random/random.b) · **Tests:**
  [`random/random_test.b`](../random/random_test.b) (run with `bantu run`).

```bantu
include "./random.b" as random;

random.seed(42);                         // reproducible runs
print(str(random.randint(1, 6)));        // a dice roll
print(str(random.uniform(0, 1)));        // a float in [0,1]
print(random.choice(["rock", "paper", "scissors"]));

$hand = random.sample([1,2,3,4,5,6,7,8,9,10], 5);   // 5 distinct cards
$deck = random.shuffle([1,2,3,4,5]);                // a shuffled copy
print(str(random.gauss(0, 1)));                     // normal(0,1)
```

Run the tests:

```sh
bantu run random/random_test.b     # prints "RESULT: ALL GREEN"
```

---

## Seeding & reproducibility

Bantu's built-in `random()` is a Mersenne-Twister that **cannot be seeded from Bantu**.
So this module owns its own generator — a classic 32-bit **Linear Congruential Generator**
(`state = (1664525·state + 1013904223) mod 2³²`) whose arithmetic stays exact in Bantu's
doubles. That makes `seed()` fully reproducible.

```bantu
random.seed(123);  $a = random.random();
random.seed(123);  $b = random.random();   // $a == $b
```

The module auto-seeds from `clock()` at load, so without a `seed()` call each run differs.

> **Quality note.** This is an LCG — excellent for games, simulations, sampling, shuffling,
> and deterministic tests, but **not cryptographically secure**. Do not use it for keys,
> tokens, or password salts.

---

## API reference

### Seeding & state
| call | meaning |
|---|---|
| `random.seed(n)` | seed the generator (any number) for reproducible sequences |
| `random.getstate()` | return the current internal state (a number) |
| `random.setstate(s)` | restore a state previously read with `getstate()` |

### Uniform reals & integers
| call | returns |
|---|---|
| `random.random()` | float in `[0.0, 1.0)` |
| `random.uniform(a, b)` | float in `[a, b]` |
| `random.randint(a, b)` | integer in `[a, b]` (both inclusive) |
| `random.randbelow(n)` | integer in `[0, n)` |
| `random.randrange(start, stop)` | integer in `[start, stop)` |
| `random.randrangeStep(start, stop, step)` | integer from `start, start+step, …` that is `< stop` |
| `random.randbool(p)` | `true` with probability `p` |

### Choosing from sequences
| call | returns |
|---|---|
| `random.choice(seq)` | one element chosen uniformly |
| `random.choicesUniform(pop, k)` | `k` elements **with** replacement, uniform |
| `random.choices(pop, weights, k)` | `k` elements **with** replacement, weighted (pass `[]` weights for uniform) |
| `random.sample(pop, k)` | `k` **distinct** elements (without replacement) |
| `random.shuffle(seq)` | a **shuffled copy** of `seq` |

> **`shuffle` returns a copy.** Bantu passes lists by value, so Python's in-place
> `shuffle(list)` cannot mutate the caller's list. Reassign if you want to replace it:
> `$deck = random.shuffle($deck);`

### Distributions
| call | returns |
|---|---|
| `random.gauss(mu, sigma)` / `random.normalvariate(mu, sigma)` | normal distribution (Box–Muller) |
| `random.expovariate(lambd)` | exponential distribution, mean `1/lambd` |
| `random.triangular(low, high, mode)` | triangular distribution |

### Bits
| call | returns |
|---|---|
| `random.getrandbits(k)` | a non-negative integer with `k` random bits, in `[0, 2ᵏ)` (exact to ~52 bits) |

---

## Naming note (Python overloads)

Bantu functions take a fixed number of arguments (no optional/variadic params), so a few
Python overloads become separate named calls:

| Python | Bantu |
|---|---|
| `random.randrange(stop)` / `(start, stop)` / `(start, stop, step)` | `randbelow(stop)` · `randrange(start, stop)` · `randrangeStep(start, stop, step)` |
| `random.choices(pop, weights=…, k=…)` | `choices(pop, weights, k)` · `choicesUniform(pop, k)` |
