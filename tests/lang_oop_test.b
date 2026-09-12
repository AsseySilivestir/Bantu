// ════════════════════════════════════════════════════════════════════════
//  lang_oop_test.b — OOP correctness guards:
//    (1) `this` binds to the receiver on cross-instance method calls
//    (2) mutating a list field in place (this.list[i]=x / .push / append) persists
//  Run:  build/bantu run tests/lang_oop_test.b
// ════════════════════════════════════════════════════════════════════════

$R = {"pass": 0, "fail": 0};
def eq($got, $want, $name) {
    if ($got == $want) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); print("          got:  " + str($got)); print("          want: " + str($want)); }
}

// ── (1) cross-instance `this` ────────────────────────────────────────────────
class Wallet { def init($a) { this.a = $a; } def balance() { return this.a; } }
class Bank {
    def init() { this.fee = 999; }
    def check($w) { return $w.balance(); }               // other instance's method
    def total($x, $y) { return $x.balance() + $y.balance(); }
    def make($v) { return new Wallet($v); }
    def makeRead($v) { return this.make($v).balance(); } // call on a returned instance
}
$b = new Bank();
eq($b.check(new Wallet(100)), 100, "cross-instance method call");
eq($b.total(new Wallet(100), new Wallet(50)), 150, "two cross-instance calls");
eq($b.makeRead(42), 42, "call method on returned instance");

class Inner { def init($v) { this.v = $v; } def twice() { return this.v * 2; } }
class Outer { def init($i) { this.inner = $i; } def compute() { return this.inner.twice() + 1; } }
eq((new Outer(new Inner(10))).compute(), 21, "call method on a field instance");

// inheritance + polymorphic dispatch still correct
class Animal { def init($n) { this.name = $n; } def speak() { return this.name + " sound"; } }
class Dog extends Animal { def init($n) { super($n); } def speak() { return this.name + " barks"; } }
$zoo = [new Dog("Rex"), new Animal("Cat")];
$rep = "";
each ($a in $zoo) { $rep = $rep + $a.speak() + "|"; }
eq($rep, "Rex barks|Cat sound|", "polymorphic dispatch over instances");

// free function still gets dynamic `this` (backward-compat)
def freeThis() { return this.tag; }
class Dyn { def init() { this.tag = "DYN"; } def run() { return freeThis(); } }
eq((new Dyn()).run(), "DYN", "free function inherits caller this");

// ── (2) in-place list-field mutation ─────────────────────────────────────────
class Store {
    def init() { this.l = [10, 20, 30]; this.grid = [[0, 0], [0, 0]]; this.items = []; }
    def setIdx($i, $v) { this.l[$i] = $v; }
    def append($v) { this.l[len(this.l)] = $v; }
    def pushIt($v) { this.l.push($v); }
    def popIt() { return this.l.pop(); }
    def set2D($i, $j, $v) { this.grid[$i][$j] = $v; }
    def add($x) { this.items[len(this.items)] = $x; return this; }
    def count() { return len(this.items); }
}
$s = new Store();
$s.setIdx(1, 999); eq(str($s.l), "[10, 999, 30]", "this.list[i]=v persists");
$s.append(40);     eq(str($s.l), "[10, 999, 30, 40]", "append to list field persists");
$s.pushIt(50);     eq(str($s.l), "[10, 999, 30, 40, 50]", "this.list.push persists");
eq($s.popIt(), 50, "this.list.pop returns last");
eq(str($s.l), "[10, 999, 30, 40]", "this.list.pop mutated the field");
$s.set2D(1, 0, 9); eq(str($s.grid), "[[0, 0], [9, 0]]", "2D this.grid[i][j]=v persists");
$s.add("a").add("b").add("c"); eq($s.count(), 3, "chained append (return this)");

// growing a field list past its end
class G { def init() { this.l = []; } def put($i, $v) { this.l[$i] = $v; } }
$g = new G(); $g.put(3, "x");
eq(str($g.l), "[0, 0, 0, x]", "grow field list by index");

// dict-field index-assign still works
class D { def init() { this.d = {}; } def put($k, $v) { this.d[$k] = $v; } }
$d = new D(); $d.put("k", 1);
eq(str($d.d), "{k: 1}", "dict field index-assign still works");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
