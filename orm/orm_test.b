// ════════════════════════════════════════════════════════════════════════
//  orm_test.b — unit + integration tests for the Bantu ORM.
//  Run from the repo root:   ./bantu run lib/orm_test.b
//  A tiny assert harness is included (Bantu has no test framework yet).
// ════════════════════════════════════════════════════════════════════════

include "./orm.b" as orm;

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

// A model built by hand (no DB) so we can test pure SQL generation.
def fakeModel($driverName) {
    dict $c = {"driver": $driverName, "dialect": orm._dialect($driverName)};
    return {
        "conn": $c,
        "table": "users",
        "fields": [
            orm.pk("id"),
            orm.field("name", "text", {"nullable": false}),
            orm.field("age", "int", {}),
            orm.field("email", "text", {"unique": true})
        ]
    };
}


// ════════════════════════════════════════════════════════════════════════
print("");
print("-- Unit: escaping & identifiers --");

$sq = orm._dialect("sqlite");
$pg = orm._dialect("postgres");
$my = orm._dialect("mysql");

eq(orm._escape($sq, "O'Brien"), "'O''Brien'", "escape single quote (sqlite)");
eq(orm._escape($my, "a\\b"), "'a\\\\b'", "escape backslash (mysql)");
eq(orm._escape($sq, 42), "42", "escape number");
eq(orm._escape($sq, true), "1", "escape bool true (sqlite)");
eq(orm._escape($pg, true), "TRUE", "escape bool true (postgres)");
eq(orm._escape($sq, null), "NULL", "escape null");
eq(orm._ident($sq, "users"), "\"users\"", "ident quote (sqlite/pg)");
eq(orm._ident($my, "users"), "`users`", "ident quote (mysql)");
eq(orm._escape($sq, orm.raw("views + 1")), "views + 1", "raw marker bypasses quoting");


print("");
print("-- Unit: lookup parsing --");

$l1 = orm._parseLookup("age__gte");
eq($l1.col, "age", "parse col of age__gte");
eq($l1.op, "gte", "parse op of age__gte");
$l2 = orm._parseLookup("name");
eq($l2.op, "eq", "bare field defaults to eq");
$l3 = orm._parseLookup("created_at");
eq($l3.col, "created_at", "underscored column not mis-split");
eq($l3.op, "eq", "underscored column op eq");
$l4 = orm._parseLookup("bio__icontains");
eq($l4.op, "icontains", "icontains beats contains");


print("");
print("-- Unit: condition compilation (values are BOUND, not inlined) --");

// _cond now takes a binder; values become placeholders and land in b.params.
$bg = orm._binder($sq);
eq(orm._cond($bg, "age", "gte", 18), "\"age\" >= ?", "cond gte emits placeholder");
eq(str($bg.params), "[18]", "cond gte binds the value");

$bi = orm._binder($sq);
eq(orm._cond($bi, "id", "in", [1, 2, 3]), "\"id\" IN (?, ?, ?)", "cond in emits one placeholder each");
eq(str($bi.params), "[1, 2, 3]", "cond in binds every value");

$bc = orm._binder($sq);
eq(orm._cond($bc, "name", "contains", "al"), "\"name\" LIKE ?", "cond contains emits placeholder");
eq(str($bc.params), "[%al%]", "cond contains binds the LIKE pattern");

$bn = orm._binder($sq);
eq(orm._cond($bn, "deleted", "isnull", true), "\"deleted\" IS NULL", "cond isnull true");
eq(len($bn.params), 0, "cond isnull binds nothing");

$be = orm._binder($sq);
eq(orm._cond($be, "name", "eq", null), "\"name\" IS NULL", "cond eq null becomes IS NULL");
eq(len($be.params), 0, "cond eq null binds nothing");

// Postgres uses numbered placeholders ($1, $2 …)
$bp = orm._binder($pg);
eq(orm._cond($bp, "age", "gte", 18), "\"age\" >= $1", "postgres cond uses $1");
eq(orm._cond($bp, "name", "eq", "x"), "\"name\" = $2", "postgres numbering increments");

// MySQL has no real driver (simulation) → falls back to inline escaping
$bm = orm._binder($my);
eq(orm._cond($bm, "name", "eq", "al"), "`name` = 'al'", "mysql falls back to escaping");
eq(len($bm.params), 0, "mysql binds nothing (no param support)");


print("");
print("-- Unit: SELECT builder --");

$m = fakeModel("sqlite");
$q = orm.query($m);
$q = orm.filter($q, [["age__gte", 18]]);
$q = orm.orderBy($q, ["-age"]);
$q = orm.limit($q, 5);
eq(orm._buildSelect($q),
   "SELECT * FROM \"users\" WHERE \"age\" >= ? ORDER BY \"age\" DESC LIMIT 5;",
   "buildSelect filter+order+limit");
eq(str(orm._paramsOf($q)), "[18]", "queryset collects the bound value");

$q2 = orm.exclude(orm.query($m), [["name", "bob"]]);
eq(orm._buildSelect($q2),
   "SELECT * FROM \"users\" WHERE NOT (\"name\" = ?);",
   "buildSelect exclude becomes NOT");
eq(str(orm._paramsOf($q2)), "[bob]", "exclude binds its value");

$q3 = orm.values(orm.query($m), ["id", "name"]);
eq(orm._buildSelect($q3),
   "SELECT \"id\", \"name\" FROM \"users\";",
   "buildSelect projection");

$q4 = orm.filterQ(orm.query($m), orm.qOr(orm.Q([["age__lt", 18]]), orm.Q([["age__gte", 65]])));
eq(orm._buildSelect($q4),
   "SELECT * FROM \"users\" WHERE ((\"age\" < ?) OR (\"age\" >= ?));",
   "buildSelect Q OR tree");
eq(str(orm._paramsOf($q4)), "[18, 65]", "Q tree binds values in order");


print("");
print("-- Integration: SQLite in-memory --");

$conn = orm.open({"driver": "sqlite", "path": ":memory:"});
$User = orm.model($conn, "people", [
    orm.pk("id"),
    orm.field("name", "text", {"nullable": false}),
    orm.field("age", "int", {}),
    orm.field("email", "text", {"unique": true})
]);

$c1 = orm.insert($User, {"name": "Ada", "age": 36, "email": "ada@x.io"});
ok($c1.ok, "insert Ada ok");
ok($c1.id > 0, "insert returns id");

orm.insert($User, {"name": "Alan", "age": 41, "email": "alan@x.io"});
orm.insert($User, {"name": "Grace", "age": 12, "email": "grace@x.io"});

eq(orm.count(orm.query($User)), 3, "count after 3 inserts");

$bulk = orm.bulkCreate($User, [
    {"name": "Bo", "age": 20, "email": "bo@x.io"},
    {"name": "Cy", "age": 70, "email": "cy@x.io"}
]);
ok($bulk.ok, "bulkCreate ok");
eq(orm.count(orm.query($User)), 5, "count after bulkCreate");

// filter chain: adults, oldest first
$adults = orm.all(orm.orderBy(orm.filter(orm.query($User), [["age__gte", 18]]), ["-age"]));
eq(len($adults), 4, "4 adults (age>=18)");
eq($adults[0].name, "Cy", "oldest adult is Cy");

// get by unique
$one = orm.get(orm.query($User), [["email", "ada@x.io"]]);
ok($one != null, "get by email found");
eq($one.name, "Ada", "get returns Ada");

// icontains
$aNames = orm.all(orm.filter(orm.query($User), [["name__icontains", "a"]]));
ok(len($aNames) >= 3, "icontains a matches Ada/Alan/Grace");

// SQL injection is neutralized: the payload is BOUND as a value, so it is
// stored verbatim as data and the table survives.
$before = orm.count(orm.query($User));
orm.insert($User, {"name": "Robert'); DROP TABLE people;--", "age": 7, "email": "bobby@x.io"});
eq(orm.count(orm.query($User)), $before + 1, "injection payload inserted as one ordinary row");
$bobby = orm.get(orm.query($User), [["email", "bobby@x.io"]]);
eq($bobby.name, "Robert'); DROP TABLE people;--", "payload round-trips verbatim (not executed)");
// a filter carrying a payload matches nothing rather than altering the query
$evil = orm.all(orm.filter(orm.query($User), [["name", "x' OR '1'='1"]]));
eq(len($evil), 0, "injection in a filter matches nothing");
orm.remove(orm.filter(orm.query($User), [["email", "bobby@x.io"]]), false);

// aggregate
$sum = orm.aggregate(orm.query($User), "SUM(age)", "total");
eq(num($sum), 179, "SUM(age) = 179");

// Q OR: minors or seniors
$edges = orm.all(orm.filterQ(orm.query($User),
    orm.qOr(orm.Q([["age__lt", 18]]), orm.Q([["age__gte", 65]]))));
eq(len($edges), 2, "Q OR: Grace(12) + Cy(70)");

// modify (update)
$upd = orm.modify(orm.filter(orm.query($User), [["name", "Ada"]]), {"age": 37});
eq($upd.changes, 1, "modify changed 1 row");
$ada2 = orm.get(orm.query($User), [["email", "ada@x.io"]]);
eq(num($ada2.age), 37, "Ada is now 37");

// raw expression: bump everyone's age by 1
orm.modify(orm.query($User), {"age": orm.raw(orm.colref($User, "age") + " + 1")});
$ada3 = orm.get(orm.query($User), [["email", "ada@x.io"]]);
eq(num($ada3.age), 38, "raw expression bumped age");

// remove with filter
$del = orm.remove(orm.filter(orm.query($User), [["name", "Grace"]]), false);
eq($del.changes, 1, "remove deleted Grace");
eq(orm.count(orm.query($User)), 4, "count after remove");

// remove without filter refused unless forced
$guard = orm.remove(orm.query($User), false);
ok(!$guard.ok, "remove without filter is refused");


print("");
print("-- Integration: migrations (auto-diff) --");

// A model that adds a "nickname" column the table lacks.
$UserV2 = {
    "conn": $conn,
    "table": "people",
    "fields": [
        orm.pk("id"),
        orm.field("name", "text", {"nullable": false}),
        orm.field("age", "int", {}),
        orm.field("email", "text", {"unique": true}),
        orm.field("nickname", "text", {})
    ]
};
$plan = orm.migrate([$UserV2], {"dryRun": true});
eq(len($plan), 1, "migrate dryRun: 1 ALTER planned");
ok(orm._endsWith($plan[0], "ADD COLUMN \"nickname\" TEXT;"), "migrate plans ADD COLUMN nickname");

orm.migrate([$UserV2], {"dryRun": false});
$plan2 = orm.migrate([$UserV2], {"dryRun": true});
eq(len($plan2), 0, "migrate is idempotent (no changes second run)");


print("");
print("-- Integration: database switching --");

// Re-point the ORM at a different SQLite database. The SAME model code runs
// against it, proving dialect/handle portability.
$conn2 = orm.use({"driver": "sqlite", "path": ":memory:"});
$Prod = orm.model($conn2, "people", [
    orm.pk("id"),
    orm.field("name", "text", {"nullable": false})
]);
orm.insert($Prod, {"name": "Fresh"});
eq(orm.count(orm.query($Prod)), 1, "switched DB has its own data");


// ─── Summary ────────────────────────────────────────────────────────────
print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
