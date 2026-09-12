# Bantu ORM

A small, Django-inspired Object-Relational Mapper written **entirely in the Bantu
language** (`lib/orm.b`), with ultraorm-style ergonomics: a pluggable dialect layer,
runtime database switching, bulk operations, and auto-diff migrations.

- **Backends:** SQLite (works today), PostgreSQL and MySQL (work when the interpreter
  is built with `-DBANTU_POSTGRES=ON` / `-DBANTU_MYSQL=ON`).
- **Source:** [`lib/orm.b`](../lib/orm.b) · **Tests:** [`lib/orm_test.b`](../lib/orm_test.b)
  (46 assertions) · **Demo:** [`samples/orm-demo/main.b`](../samples/orm-demo/main.b)

```bantu
include "lib/orm.b" as orm;

$conn = orm.open({"driver": "sqlite", "path": "app.db"});
$User = orm.model($conn, "users", [
    orm.pk("id"),
    orm.field("name",  "text", {"nullable": false}),
    orm.field("email", "text", {"unique": true}),
    orm.field("age",   "int",  {})
]);

orm.insert($User, {"name": "Ada", "email": "ada@x.io", "age": 36});

$adults = orm.all(orm.orderBy(orm.filter(orm.query($User), [["age__gte", 18]]), ["-age"]));
```

Run the tests / demo:

```sh
./bantu run lib/orm_test.b          # 46 assertions, prints "ALL GREEN"
./bantu run samples/orm-demo/main.b # end-to-end feature tour
```

---

## Naming note (Bantu keywords)

A few natural verbs are Bantu **reserved words** (`connect`, `create`, `update`, `delete`,
`null`, `default`, `list`, `db`, …). The API sidesteps them:

| You might expect | Use instead |
|---|---|
| `connect()` | **`open()`** |
| `create()`  | **`insert()`** |
| `update()`  | **`modify()`** |
| `delete()`  | **`remove()`** |
| field opt `null`     | **`nullable`** |
| field opt `default`  | **`defaultVal`** |

Filters are passed as a **list of `[field, value]` pairs** (not a dict), because Bantu's
`each` cannot enumerate dict keys.

---

## Connection

### `orm.open(config)` → connection
Opens a backend and returns a connection handle. Re-points the runtime's active
handle, so calling it again is how you switch databases.

```bantu
$conn = orm.open({"driver": "sqlite",   "path": "app.db"});
$conn = orm.open({"driver": "sqlite",   "path": ":memory:"});
$conn = orm.open({"driver": "postgres", "conn": "host=localhost dbname=app user=me"});
$conn = orm.open({"driver": "mysql",    "conn": "host=localhost db=app user=me"});
```

### `orm.use(config)` → connection
Alias for `open()`. Semantic sugar for "switch the active database". Models are portable;
re-binding a model to a new connection lets identical code run on another backend.

---

## Models & fields

### `orm.field(name, ftype, opts)` → field
Declares a column. `ftype` ∈ `"int" | "text" | "real" | "bool" | "datetime"`.

`opts` keys (all optional):

| key | meaning |
|---|---|
| `nullable` | `false` → `NOT NULL` (columns are nullable unless set false) |
| `unique` | `true` → `UNIQUE` |
| `defaultVal` | column default (`"now"` maps to the backend's `CURRENT_TIMESTAMP`) |
| `pk` | `true` → auto-increment primary key |
| `fk` | `"other_table.col"` → foreign-key reference |

### `orm.pk(name)` → field
Shorthand for an auto-increment integer primary key.

### `orm.fk(name, target)` → field
Shorthand for an integer foreign-key column (`target` = `"table.col"`).

### `orm.model(conn, table, fields)` → model
Binds a table to a connection and **auto-creates it** (`CREATE TABLE IF NOT EXISTS`).
`fields` is an ordered list of field descriptors.

### `orm.sync(model)` / `orm.drop(model)`
Create (idempotent) or drop the model's table.

```bantu
$User = orm.model($conn, "users", [
    orm.pk("id"),
    orm.field("name", "text", {"nullable": false}),
    orm.fk("team_id", "teams.id"),
    orm.field("joined", "datetime", {"defaultVal": "now"})
]);
```

---

## Writing rows

### `orm.insert(model, data)` → `{id, changes, ok}`
Inserts one row. Only the keys present (non-null) in `data` are written; the rest fall
back to their database defaults.

```bantu
$r = orm.insert($User, {"name": "Ada", "age": 36});
print($r.id);   // new primary key
```

### `orm.bulkCreate(model, rows)` → `{count, ok}`
Inserts many rows in a **single** multi-row `INSERT`. Provide values for every `NOT NULL`
column without a default (missing values become `NULL`).

```bantu
orm.bulkCreate($User, [
    {"name": "Alan", "age": 41},
    {"name": "Grace", "age": 27}
]);
```

### `orm.modify(queryset, data)` → `{changes, ok}`
`UPDATE` the rows matched by the queryset's filter. Only non-null keys in `data` are set.
Values may be raw expressions (see `orm.raw` / `orm.colref`).

```bantu
orm.modify(orm.filter(orm.query($User), [["name", "Ada"]]), {"age": 37});
orm.modify(orm.query($Post), {"views": orm.raw(orm.colref($Post, "views") + " + 1")});
```

### `orm.remove(queryset, force)` → `{changes, ok}`
`DELETE` matching rows. Refuses to run without a filter unless `force` is `true` (guards
against full-table wipes).

```bantu
orm.remove(orm.filter(orm.query($User), [["id", 5]]), false);
```

---

## Querying (chainable, lazy)

Start with `orm.query(model)` and chain builders; each returns the queryset. Nothing hits
the database until a **terminal** (`all`, `first`, `get`, `count`, `exists`, `aggregate`).

### Builders

| call | effect |
|---|---|
| `orm.filter(qs, pairs)` | AND the `[field, value]` pairs into `WHERE` |
| `orm.exclude(qs, pairs)` | AND `NOT (…)` |
| `orm.filterQ(qs, q)` | AND a `Q` tree (for OR / NOT) |
| `orm.whereRaw(qs, sql)` | AND a raw SQL predicate |
| `orm.orderBy(qs, fields)` | `ORDER BY`; prefix a field with `-` for `DESC` |
| `orm.limit(qs, n)` / `orm.offset(qs, n)` | pagination |
| `orm.values(qs, cols)` | project only these columns |
| `orm.distinct(qs)` | `SELECT DISTINCT` |

### Field lookups

Suffix a field with `__lookup`:

| lookup | SQL |
|---|---|
| *(none)* | `=` (or `IS NULL` when value is null) |
| `ne` | `<>` / `IS NOT NULL` |
| `gt` `gte` `lt` `lte` | `>` `>=` `<` `<=` |
| `in` | `IN (…)` (value is a list) |
| `contains` / `icontains` | `LIKE '%v%'` (case-insensitive variant) |
| `startswith` / `istartswith` | `LIKE 'v%'` |
| `endswith` | `LIKE '%v'` |
| `isnull` | `IS NULL` / `IS NOT NULL` (value is a bool) |

### Terminals

| call | returns |
|---|---|
| `orm.all(qs)` | list of row dicts |
| `orm.first(qs)` | first row dict, or `null` |
| `orm.get(qs, pairs)` | filter by pairs then return the single row (or `null`) |
| `orm.count(qs)` | number of matching rows |
| `orm.exists(qs)` | bool |
| `orm.aggregate(qs, expr, alias)` | scalar, e.g. `orm.aggregate(qs, "SUM(age)", "t")` |

```bantu
$adults = orm.all(
    orm.orderBy(
        orm.filter(orm.query($User), [["age__gte", 18], ["name__icontains", "a"]]),
        ["-age"]));

$one = orm.get(orm.query($User), [["email", "ada@x.io"]]);
$n   = orm.count(orm.filter(orm.query($User), [["age__lt", 18]]));
```

### Complex queries — `Q` objects

```bantu
// (age < 18) OR (age >= 65)
$edge = orm.all(orm.filterQ(orm.query($User),
    orm.qOr(orm.Q([["age__lt", 18]]), orm.Q([["age__gte", 65]]))));
```

| call | meaning |
|---|---|
| `orm.Q(pairs)` | leaf predicate (pairs ANDed) |
| `orm.qAnd(a, b)` / `orm.qOr(a, b)` | combine Q nodes |
| `orm.qNot(a)` | negate |

### Raw / column expressions

| call | meaning |
|---|---|
| `orm.raw(sql)` | inject a raw SQL fragment (bypasses quoting) |
| `orm.colref(model, col)` | a dialect-quoted column reference string |
| `orm.F(col)` | column-reference marker (Django `F`) |

---

## Migrations (auto-diff)

### `orm.migrate(models, opts)` → list of SQL statements
Introspects the live schema (`PRAGMA table_info` on SQLite, `information_schema.columns`
on Postgres/MySQL), diffs it against each model, and emits `CREATE TABLE` for new tables
and `ALTER TABLE … ADD COLUMN` for added columns. Idempotent.

`opts.dryRun = true` returns the plan **without** executing it.

```bantu
$plan = orm.migrate([$User, $Post], {"dryRun": true});
each ($sql in $plan) { print($sql); }
orm.migrate([$User, $Post], {"dryRun": false});   // apply
```

---

## Security

There are no prepared statements exposed to Bantu scripts, so **every value passes through
one dialect-aware escaping function** (`_escape`) — the single SQL-injection boundary.
It quotes strings (doubling `'`, and `\` on MySQL), renders numbers/booleans/null directly,
and lets only explicit `orm.raw(...)` fragments through unquoted. Never build ORM SQL by
concatenating untrusted input outside the query API.

## Known limitations

- Dict keys can't be enumerated in Bantu, so filters/updates use `[field, value]` pair
  lists and `data` dicts are read against the model's declared columns.
- No prepared statements (escaping is the safety layer).
- Postgres/MySQL require building the interpreter with their driver flags; without them the
  dialects still generate correct SQL but execution is stubbed.
