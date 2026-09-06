// ════════════════════════════════════════════════════════════════════════
//  arctic_io_test.b — typed CSV / SQLite I/O (arctic foundations Phase 4).
//  Run:  bantu run tests/arctic_io_test.b
//  Writes its fixtures under the system temp dir.
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
def L($c) { return str(col_to_list($c)); }

$dir = "/tmp";
$csv = $dir + "/arctic_io_fixture.csv";

// Write a CSV fixture by hand (with a quoted field containing a comma, and a
// missing value to become null).
$text = "name,age,score,active\n";
$text = $text + "Ada,36,9.5,true\n";
$text = $text + "\"Bob, Jr\",41,8.0,false\n";
$text = $text + "Cy,,7.5,true\n";       // missing age -> null
writefile($csv, $text);

print("");
print("-- read_csv: shape, names, type inference --");
$df = read_csv($csv);
eq(str($df.names), "[name, age, score, active]", "header names parsed");
eq(str($df.shape), "[3, 4]", "shape rows x cols");
eq(col_dtype($df.cols.name), "utf8", "name inferred utf8");
eq(col_dtype($df.cols.age), "i64", "age inferred i64");
eq(col_dtype($df.cols.score), "f64", "score inferred f64");
eq(col_dtype($df.cols.active), "bool", "active inferred bool");

print("");
print("-- read_csv: values, quoting, nulls --");
eq(col_len($df.cols.name), 3, "name column has 3 rows");
eq(col_get($df.cols.name, 1), "Bob, Jr", "quoted field keeps its comma");
eq(col_get($df.cols.age, 0), 36, "first age");
eq(col_get($df.cols.age, 2), null, "missing age is null");
eq(col_null_count($df.cols.age), 1, "one null in age");
eq(col_get($df.cols.score, 1), 8, "score value");
eq(col_get($df.cols.active, 0), true, "bool value");

print("");
print("-- compute on loaded columns (end to end) --");
eq(col_sum($df.cols.score), 25, "sum of score column");
eq(col_mean($df.cols.age), 38.5, "mean age ignores the null");   // (36+41)/2

print("");
print("-- write_csv round-trip --");
$out = $dir + "/arctic_io_out.csv";
write_csv($df, $out);
$df2 = read_csv($out);
eq(str($df2.names), "[name, age, score, active]", "round-trip names");
eq(str($df2.shape), "[3, 4]", "round-trip shape");
eq(col_get($df2.cols.name, 1), "Bob, Jr", "round-trip keeps quoted comma");
eq(col_get($df2.cols.age, 2), null, "round-trip keeps null");
eq(col_sum($df2.cols.score), 25, "round-trip values intact");

print("");
print("-- read_sqlite --");
$dbpath = $dir + "/arctic_io.db";
sua.sqlite.open($dbpath);
sua.sqlite.exec("DROP TABLE IF EXISTS t");
sua.sqlite.exec("CREATE TABLE t (id INTEGER, name TEXT, score REAL)");
sua.sqlite.exec("INSERT INTO t VALUES (1, 'Ada', 9.5)");
sua.sqlite.exec("INSERT INTO t VALUES (2, 'Bob', 8.0)");
sua.sqlite.exec("INSERT INTO t VALUES (3, 'Cy', 7.5)");
$sq = read_sqlite($dbpath, "SELECT id, name, score FROM t ORDER BY id");
eq(str($sq.names), "[id, name, score]", "sqlite column names");
eq(str($sq.shape), "[3, 3]", "sqlite shape");
eq(col_dtype($sq.cols.id), "i64", "sqlite id -> i64");
eq(col_dtype($sq.cols.name), "utf8", "sqlite name -> utf8");
eq(col_dtype($sq.cols.score), "f64", "sqlite score -> f64");
eq(col_sum($sq.cols.score), 25, "sqlite numeric sum");
eq(col_get($sq.cols.name, 0), "Ada", "sqlite text value");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
