// ════════════════════════════════════════════════════════════════════════
//  arctic_csv2_test.b — fast two-pass CSV reader: correctness, differential
//  against the reference engine, projection pushdown, and edge cases.
//  Run:  build/bantu run tests/arctic_csv2_test.b
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

def keyIn($dict, $key) {
    each ($k in keys($dict)) { if ($k == $key) { return true; } }
    return false;
}

// Render a whole frame as a comparable string: names + each column's to_list.
def frameStr($fr) {
    $s = "names=" + str($fr.names);
    each ($n in $fr.names) {
        $s = $s + "|" + $n + "=" + str(col_to_list($fr.cols[$n]));
    }
    return $s;
}

$dir = "/tmp";
$path = $dir + "/arctic_csv2_fix.csv";

print("");
print("-- basic types & inference --");
$csv = "name,age,amount,flag\nAda,36,1200.5,true\nBob,41,500,false\nCy,29,2000.0,true\n";
writefile($path, $csv);
$f = read_csv($path);
eq(str($f.names), "[name, age, amount, flag]", "header names");
eq(col_dtype($f.cols["age"]), "i64", "age inferred i64");
eq(col_dtype($f.cols["amount"]), "f64", "amount inferred f64");
eq(col_dtype($f.cols["flag"]), "bool", "flag inferred bool");
eq(col_dtype($f.cols["name"]), "utf8", "name inferred utf8");
eq(col_get($f.cols["age"], 1), 41, "age[1]");
eq(col_get($f.cols["amount"], 0), 1200.5, "amount[0]");
eq(col_get($f.cols["flag"], 2), true, "flag[2]");

print("");
print("-- differential: fast == slow (well-formed) --");
$fast = read_csv($path);
$slow = read_csv($path, {"engine": "slow"});
eq(frameStr($fast), frameStr($slow), "fast engine matches reference engine");

print("");
print("-- quotes, escapes, embedded delimiter & newline --");
$q = "id,text\n1,\"hello, world\"\n2,\"a \"\"quoted\"\" word\"\n3,\"line1\nline2\"\n4,plain\n";
writefile($path, $q);
$fq = read_csv($path);
$sq = read_csv($path, {"engine": "slow"});
eq(frameStr($fq), frameStr($sq), "quoted fields: fast == slow");
eq(col_get($fq.cols["text"], 0), "hello, world", "embedded delimiter kept");
eq(col_get($fq.cols["text"], 1), "a \"quoted\" word", "\"\" unescaped");
eq(col_get($fq.cols["text"], 2), "line1\nline2", "embedded newline kept");

print("");
print("-- missing values -> null; quoted empty -> empty string --");
$m = "a,b,c\n1,,3\n4,5,\n,7,9\n";
writefile($path, $m);
$fm = read_csv($path);
eq(col_null_count($fm.cols["b"]), 1, "b has one null");
eq(col_null_count($fm.cols["c"]), 1, "c has one null");
eq(col_null_count($fm.cols["a"]), 1, "a has one null");
$qe = "a,b\n1,\"\"\n2,\n";
writefile($path, $qe);
$fqe = read_csv($path);
eq(col_null_count($fqe.cols["b"]), 1, "unquoted empty is null, quoted empty is not");

print("");
print("-- header:false --");
$nh = "10,20\n30,40\n";
writefile($path, $nh);
$fnh = read_csv($path, {"header": false});
eq(str($fnh.names), "[col0, col1]", "synthetic names when header:false");
eq($fnh.shape[0], 2, "two data rows");
eq(col_get($fnh.cols["col0"], 1), 30, "value read with no header");

print("");
print("-- custom delimiter --");
$td = "x;y;z\n1;2;3\n";
writefile($path, $td);
$ftd = read_csv($path, {"delim": ";"});
eq(str($ftd.names), "[x, y, z]", "semicolon delimiter");
eq(col_get($ftd.cols["y"], 0), 2, "value with ; delimiter");

print("");
print("-- ragged rows -> missing become null --");
$rg = "a,b,c\n1,2,3\n4,5\n6\n";
writefile($path, $rg);
$frg = read_csv($path);
eq($frg.shape[0], 3, "three rows");
eq(col_null_count($frg.cols["c"]), 2, "c null on short rows");
$srg = read_csv($path, {"engine": "slow"});
eq(frameStr($frg), frameStr($srg), "ragged: fast == slow");

print("");
print("-- projection pushdown (columns=) --");
$big = "name,region,age,amount\nAda,EU,36,1200\nBob,US,41,500\n";
writefile($path, $big);
$proj = read_csv($path, {"columns": ["region", "amount"]});
eq(str($proj.names), "[region, amount]", "only requested columns, in order");
eq($proj.shape[1], 2, "width is 2");
ok(!keyIn($proj.cols, "name"), "name not materialized");
eq(col_get($proj.cols["amount"], 1), 500, "projected value correct");

print("");
print("-- CRLF line endings --");
$crlf = "a,b\r\n1,2\r\n3,4\r\n";
writefile($path, $crlf);
$fcrlf = read_csv($path);
eq($fcrlf.shape[0], 2, "CRLF: two rows");
eq(col_get($fcrlf.cols["b"], 0), 2, "CRLF: value parsed, \\r stripped");
eq(col_dtype($fcrlf.cols["b"]), "i64", "CRLF: trailing \\r did not spoil int inference");

print("");
print("-- empty & trailing-newline files --");
writefile($path, "");
$fe = read_csv($path);
eq($fe.shape[0], 0, "empty file -> 0 rows");
writefile($path, "a,b\n1,2");
$fnt = read_csv($path);
eq($fnt.shape[0], 1, "no trailing newline still reads last row");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
