// ════════════════════════════════════════════════════════════════════════
//  orm-demo — a runnable tour of the Bantu ORM.
//  Run from the repo root:   ./bantu run samples/orm-demo/main.b
// ════════════════════════════════════════════════════════════════════════

include "../../orm/orm.b" as orm;

print("");
print("╔══════════════════════════════════════════╗");
print("║  Bantu ORM — feature demo                 ║");
print("╚══════════════════════════════════════════╝");

// 1) Connect. Swap this one line for postgres/mysql and everything else is
//    unchanged (needs -DBANTU_POSTGRES/MYSQL builds for those backends).
$conn = orm.open({"driver": "sqlite", "path": ":memory:"});

// 2) Declare models. The table is created automatically.
$User = orm.model($conn, "users", [
    orm.pk("id"),
    orm.field("name",  "text", {"nullable": false}),
    orm.field("email", "text", {"unique": true}),
    orm.field("age",   "int",  {}),
    orm.field("joined", "datetime", {"defaultVal": "now"})
]);

$Post = orm.model($conn, "posts", [
    orm.pk("id"),
    orm.fk("author_id", "users.id"),
    orm.field("title", "text", {"nullable": false}),
    orm.field("views", "int", {"defaultVal": 0})
]);

// 3) Insert one row (returns the new id).
$ada = orm.insert($User, {"name": "Ada", "email": "ada@x.io", "age": 36});
print("");
print("inserted Ada with id " + str($ada.id));

// 4) Bulk insert.
orm.bulkCreate($User, [
    {"name": "Alan",  "email": "alan@x.io",  "age": 41},
    {"name": "Grace", "email": "grace@x.io", "age": 27},
    {"name": "Kofi",  "email": "kofi@x.io",  "age": 19}
]);
print("total users: " + str(orm.count(orm.query($User))));

// 5) Django-style chained query: adults, newest name first, projected columns.
$adults = orm.all(
    orm.values(
        orm.orderBy(
            orm.filter(orm.query($User), [["age__gte", 21]]),
            ["-age"]),
        ["name", "age"]));
print("");
print("adults (age>=21), oldest first:");
each ($u in $adults) {
    print("  - " + $u.name + " (" + str($u.age) + ")");
}

// 6) Complex OR query with Q objects.
$young_or_old = orm.all(
    orm.filterQ(orm.query($User),
        orm.qOr(orm.Q([["age__lt", 20]]), orm.Q([["age__gte", 40]]))));
print("");
print("under-20 OR 40-plus: " + str(len($young_or_old)) + " users");

// 7) Aggregate.
$avg = orm.aggregate(orm.query($User), "AVG(age)", "avg_age");
print("average age: " + str($avg));

// 8) Update with a filter, and a raw column expression.
orm.modify(orm.filter(orm.query($User), [["email", "ada@x.io"]]), {"age": 37});
orm.insert($Post, {"author_id": $ada.id, "title": "Notes on the Analytical Engine"});
orm.modify(orm.query($Post), {"views": orm.raw(orm.colref($Post, "views") + " + 5")});
$post = orm.first(orm.query($Post));
print("");
print("post '" + $post.title + "' now has " + str($post.views) + " views");

// 9) Auto-diff migration: add a column that the model gained.
$UserV2 = {
    "conn": $conn,
    "table": "users",
    "fields": [
        orm.pk("id"),
        orm.field("name",  "text", {"nullable": false}),
        orm.field("email", "text", {"unique": true}),
        orm.field("age",   "int",  {}),
        orm.field("joined", "datetime", {"defaultVal": "now"}),
        orm.field("bio",   "text", {})
    ]
};
$plan = orm.migrate([$UserV2], {"dryRun": true});
print("");
print("migration plan (dry run):");
each ($sql in $plan) { print("  " + $sql); }
orm.migrate([$UserV2], {"dryRun": false});
orm.modify(orm.filter(orm.query($UserV2), [["email", "ada@x.io"]]), {"bio": "Countess of Lovelace"});
$ada2 = orm.get(orm.query($UserV2), [["email", "ada@x.io"]]);
print("after migration, Ada.bio = " + $ada2.bio);

// 10) Switch databases at runtime — same model code, a fresh store.
$conn2 = orm.use({"driver": "sqlite", "path": ":memory:"});
$Fresh = orm.model($conn2, "users", [orm.pk("id"), orm.field("name", "text", {})]);
orm.insert($Fresh, {"name": "Only Me"});
print("");
print("switched to a new database — it has " + str(orm.count(orm.query($Fresh))) + " user");

print("");
print("done.");
