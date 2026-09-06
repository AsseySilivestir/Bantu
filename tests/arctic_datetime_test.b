// ════════════════════════════════════════════════════════════════════════
//  arctic_datetime_test.b — native datetime/date logical dtype.
//  Run:  build/bantu run tests/arctic_datetime_test.b
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

$strs = col(["2020-01-15 10:30:00", "2021-12-31 23:59:59", "2019-07-04"], "utf8");
$dt = col_to_datetime($strs);

print("");
print("-- construction & round-trip --");
eq(col_dtype($dt), "datetime", "dtype is datetime");
eq(col_get($dt, 0), "2020-01-15 10:30:00", "round-trips full timestamp");
eq(col_get($dt, 2), "2019-07-04 00:00:00", "date-only parses at midnight");

print("");
print("-- calendar components --");
eq(col_get(col_dt_year($dt), 0), 2020, "year[0]");
eq(col_get(col_dt_month($dt), 0), 1, "month[0]");
eq(col_get(col_dt_day($dt), 0), 15, "day[0]");
eq(col_get(col_dt_hour($dt), 0), 10, "hour[0]");
eq(col_get(col_dt_minute($dt), 0), 30, "minute[0]");
eq(col_get(col_dt_second($dt), 1), 59, "second[1]");
eq(col_get(col_dt_weekday($dt), 0), 3, "weekday[0] Wed=3 (Sun=0)");
eq(col_get(col_dt_weekday($dt), 1), 5, "weekday[1] Fri=5");
eq(col_get(col_dt_weekday($dt), 2), 4, "weekday[2] Thu=4");

print("");
print("-- strftime --");
$fmt = col_strftime($dt, "%Y/%m/%d");
eq(col_get($fmt, 0), "2020/01/15", "strftime %Y/%m/%d");
eq(col_get(col_strftime($dt, "%H:%M:%S"), 1), "23:59:59", "strftime time");

print("");
print("-- comparison against ISO string --");
$mask = col_gt($dt, "2020-06-01");
eq(str(col_to_list($mask)), "[false, true, false]", "col_gt(datetime, iso string)");

print("");
print("-- sort chronologically --");
$order = col_argsort($dt, false);
eq(str(col_to_list($order)), "[2, 0, 1]", "argsort ascending = chronological");

print("");
print("-- date dtype --");
$d = col_to_date(col(["2019-07-04", "2020-02-29"], "utf8"));
eq(col_dtype($d), "date", "dtype is date");
eq(col_get($d, 1), "2020-02-29", "leap day round-trips");
eq(col_get(col_dt_year($d), 1), 2020, "date year");
eq(col_get(col_dt_day($d), 0), 4, "date day");

print("");
print("-- nulls: unparseable -> null --");
$bad = col_to_datetime(col(["2020-01-01", "not-a-date", "2020-01-03"], "utf8"));
eq(col_null_count($bad), 1, "one unparseable -> null");
ok(col_get($bad, 1) == null, "the bad cell is null");

print("");
print("-- filter/head keep datetime rendering --");
$kept = col_filter($dt, col_gt($dt, "2020-06-01"));
eq(col_dtype($kept), "datetime", "filtered column stays datetime");
eq(col_get($kept, 0), "2021-12-31 23:59:59", "filtered value renders as timestamp");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
