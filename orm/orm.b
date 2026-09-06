// ════════════════════════════════════════════════════════════════════════
//  orm.b — Bantu ORM
//  A small, Django-inspired Object-Relational Mapper written entirely in the
//  Bantu language, with ultraorm-style ergonomics (pluggable dialects,
//  runtime database switching, bulk operations, auto-diff migrations).
//
//  Backends: SQLite (real today), PostgreSQL and MySQL (real when the
//  interpreter is built with -DBANTU_POSTGRES=ON / -DBANTU_MYSQL=ON).
//
//  Include it namespaced:
//      include "lib/orm.b" as orm;
//      $store = orm.open({"driver": "sqlite", "path": "app.db"});
//      $User  = orm.model($store, "users", [
//          orm.pk("id"),
//          orm.field("name",  "text", {"nullable": false}),
//          orm.field("age",   "int",  {}),
//          orm.field("email", "text", {"unique": true})
//      ]);
//      orm.insert($User, {"name": "Ada", "age": 36, "email": "ada@x.io"});
//      $q     = orm.orderBy(orm.filter(orm.query($User), [["age__gte", 18]]), ["-age"]);
//      $rows  = orm.all($q);
//
//  NOTE: some verbs are renamed to dodge Bantu reserved words —
//  open() (not connect), insert() (not create), modify() (not update).
//
//  DESIGN NOTES (why the API looks the way it does)
//  ------------------------------------------------
//  Bantu's `each` iterates lists only — there is no way to enumerate the keys
//  of an arbitrary dict. Therefore:
//    * Model fields are declared as an ordered LIST of field descriptors.
//    * Filters are passed as a LIST of [field_lookup, value] pairs.
//    * QuerySets accumulate SQL as STRINGS, with bound values collected in a
//      binder dict (dicts are reference-shared in Bantu; lists are copied).
//
//  SAFETY: values are BOUND as parameters (v1.3.0+), never inlined — SQLite via
//  sqlite3_bind_*, PostgreSQL via PQexecParams. `_escape()` remains only for
//  identifiers/DDL defaults and as the fallback for dialects that lack a
//  parameter path (see `supportsParams` on each dialect).
// ════════════════════════════════════════════════════════════════════════


// ─── Dialects ───────────────────────────────────────────────────────────
// A dialect describes how a specific backend spells SQL. Adding a backend is
// just adding one of these descriptors and wiring _exec/_query dispatch.

// _dialect($name) → dialect descriptor dict.
//
// supportsParams / paramStyle drive the SAFE value path: when a backend accepts
// bound parameters the ORM emits a placeholder and collects the value, instead
// of inlining an escaped literal. Backends without it fall back to _escape().
//   sqlite   → "?"   (sqlite3_bind_*)
//   postgres → "$n"  (PQexecParams)
//   mysql    → none  (sua.mysql.* is a simulation, not a real driver — see
//                     docs/v1.3.0-status.md; it keeps the escaping path)
def _dialect($name) {
    if ($name == "postgres") {
        return {
            "name": "postgres",
            "quote": "\"",
            "autopk": "SERIAL PRIMARY KEY",
            "boolTrue": "TRUE", "boolFalse": "FALSE",
            "quoteBackslash": false,
            "nowExpr": "CURRENT_TIMESTAMP",
            "supportsParams": true,
            "paramStyle": "$n"
        };
    }
    if ($name == "mysql") {
        return {
            "name": "mysql",
            "quote": "`",
            "autopk": "INT AUTO_INCREMENT PRIMARY KEY",
            "boolTrue": "1", "boolFalse": "0",
            "quoteBackslash": true,
            "nowExpr": "CURRENT_TIMESTAMP",
            "supportsParams": false,
            "paramStyle": "?"
        };
    }
    // default: sqlite
    return {
        "name": "sqlite",
        "quote": "\"",
        "autopk": "INTEGER PRIMARY KEY AUTOINCREMENT",
        "boolTrue": "1", "boolFalse": "0",
        "quoteBackslash": false,
        "nowExpr": "CURRENT_TIMESTAMP",
        "supportsParams": true,
        "paramStyle": "?"
    };
}

// _columnType($dialect, $ftype) → backend column type for a logical field type.
def _columnType($dialect, $ftype) {
    string $d = $dialect.name;
    if ($ftype == "int") { return "INTEGER"; }
    if ($ftype == "integer") { return "INTEGER"; }
    if ($ftype == "real") {
        if ($d == "postgres") { return "DOUBLE PRECISION"; }
        if ($d == "mysql") { return "DOUBLE"; }
        return "REAL";
    }
    if ($ftype == "float") {
        if ($d == "postgres") { return "DOUBLE PRECISION"; }
        if ($d == "mysql") { return "DOUBLE"; }
        return "REAL";
    }
    if ($ftype == "bool") {
        if ($d == "postgres") { return "BOOLEAN"; }
        if ($d == "mysql") { return "TINYINT(1)"; }
        return "INTEGER";
    }
    if ($ftype == "datetime") {
        if ($d == "postgres") { return "TIMESTAMP"; }
        if ($d == "mysql") { return "DATETIME"; }
        return "TEXT";
    }
    // text / string / anything else
    return "TEXT";
}


// ─── Escaping & identifiers (the SQL-injection boundary) ────────────────

// _ident($dialect, $name) → quoted identifier, e.g. "users" or `users`.
def _ident($dialect, $name) {
    return $dialect.quote + $name + $dialect.quote;
}

// _escape($dialect, $v) → a safe SQL literal for $v.
// Handles null, numbers, booleans, raw/column markers, and strings.
// Every value that reaches generated SQL MUST go through here.
def _escape($dialect, $v) {
    string $t = type($v);
    if ($t == "null") { return "NULL"; }
    if ($t == "number") { return str($v); }
    if ($t == "bool") {
        if ($v) { return $dialect.boolTrue; }
        return $dialect.boolFalse;
    }
    if ($t == "dict") {
        // raw SQL / column-reference markers bypass quoting.
        if ($v.__raw) { return $v.sql; }
        if ($v.__f) { return _ident($dialect, $v.col); }
        // any other dict → stringify defensively
        return _escapeString($dialect, str($v));
    }
    if ($t == "list") {
        // a bare list escapes as a parenthesised value tuple.
        return "(" + _joinValues($dialect, $v) + ")";
    }
    return _escapeString($dialect, str($v));
}

// _escapeString — quote a string literal, doubling quotes (and backslashes on
// MySQL, which treats backslash as an escape character by default).
def _escapeString($dialect, $s) {
    string $out = $s;
    if ($dialect.quoteBackslash) {
        $out = replace($out, "\\", "\\\\");
    }
    $out = replace($out, "'", "''");
    return "'" + $out + "'";
}

// _joinValues — escape every element of a list and comma-join.
def _joinValues($dialect, $items) {
    string $out = "";
    number $i = 0;
    each ($v in $items) {
        if ($i > 0) { $out = $out + ", "; }
        $out = $out + _escape($dialect, $v);
        $i = $i + 1;
    }
    return $out;
}


// ─── Parameter binding (the preferred, injection-proof value path) ──────
//
// A "binder" carries the dialect plus the ordered list of bound values. It is a
// DICT on purpose: dicts are reference-shared in Bantu (lists are copied), so a
// binder passed into a helper accumulates params visible to the caller.
//
//   $b = _binder($dialect);
//   $sqlFragment = _bind($b, 42);   // → "?"  (and 42 is appended to $b.params)
//   ... _exec($conn, $sql, $b.params)

// _binder($dialect) → a fresh binder { dialect, params }.
def _binder($dialect) {
    return {"dialect": $dialect, "params": []};
}

// _bind($b, $v) → the SQL text to emit for value $v.
// When the dialect supports parameters this appends $v to $b.params and returns
// a placeholder; otherwise it falls back to an inline escaped literal. raw()/F()
// markers are SQL fragments and are never parameterized.
def _bind($b, $v) {
    dict $d = $b.dialect;
    if (type($v) == "dict") {
        if ($v.__raw) { return $v.sql; }
        if ($v.__f) { return _ident($d, $v.col); }
    }
    if (!$d.supportsParams) { return _escape($d, $v); }
    append($b.params, $v);
    if ($d.paramStyle == "$n") { return "$" + str(len($b.params)); }
    return "?";
}

// _bindList($b, $items) → comma-joined placeholders for every element
// (used by the `__in` lookup), binding each value.
def _bindList($b, $items) {
    string $out = "";
    number $i = 0;
    each ($v in $items) {
        if ($i > 0) { $out = $out + ", "; }
        $out = $out + _bind($b, $v);
        $i = $i + 1;
    }
    return $out;
}


// ─── Connection & runtime DB switching ──────────────────────────────────

// open($cfg) → open a backend and return a connection handle.
//   {"driver":"sqlite",   "path":"app.db"}
//   {"driver":"postgres", "conn":"host=... dbname=..."}
//   {"driver":"mysql",    "conn":"host=... db=..."}
// The underlying runtime keeps ONE active handle per driver, so calling
// open()/use() again re-points it — that is how you "switch databases".
// (Named open() rather than connect() because `connect` is a Bantu keyword.)
def open($cfg) {
    string $driver = "sqlite";
    if (type($cfg.driver) != "null") { $driver = $cfg.driver; }

    if ($driver == "postgres") {
        sua.postgres.connect($cfg.conn);
        return {"driver": "postgres", "dialect": _dialect("postgres")};
    }
    if ($driver == "mysql") {
        sua.mysql.connect($cfg.conn);
        return {"driver": "mysql", "dialect": _dialect("mysql")};
    }
    // sqlite
    string $path = ":memory:";
    if (type($cfg.path) != "null") { $path = $cfg.path; }
    sua.sqlite.open($path);
    return {"driver": "sqlite", "dialect": _dialect("sqlite")};
}

// use($cfg) — alias for open(); re-points the active database. Models are
// portable, so re-binding a model's connection lets the same code run
// against a different backend.
def use($cfg) {
    return open($cfg);
}

// _exec($conn, $sql, $params) — run a write statement, dispatching on driver.
// $params is the list of bound values ([] for plain SQL). When it is empty we
// deliberately use the NON-parameterized call: the driver's plain path handles
// multi-statement SQL (DDL), whereas the prepared path only runs the first
// statement.
def _exec($conn, $sql, $params) {
    if (len($params) == 0) {
        if ($conn.driver == "postgres") { return sua.postgres.exec($sql); }
        if ($conn.driver == "mysql") { return sua.mysql.query($sql); }
        return sua.sqlite.exec($sql);
    }
    if ($conn.driver == "postgres") { return sua.postgres.exec($sql, $params); }
    if ($conn.driver == "mysql") { return sua.mysql.query($sql); }   // simulation: no params
    return sua.sqlite.exec($sql, $params);
}

// _query($conn, $sql, $params) — run a read query, returns a LIST of row dicts.
def _query($conn, $sql, $params) {
    if (len($params) == 0) {
        if ($conn.driver == "postgres") { return sua.postgres.query($sql); }
        if ($conn.driver == "mysql") { return sua.mysql.query($sql); }
        return sua.sqlite.query($sql);
    }
    if ($conn.driver == "postgres") { return sua.postgres.query($sql, $params); }
    if ($conn.driver == "mysql") { return sua.mysql.query($sql); }   // simulation: no params
    return sua.sqlite.query($sql, $params);
}


// ─── Fields & models ────────────────────────────────────────────────────

// field($name, $ftype, $opts) → a field descriptor.
//   $ftype: "int" | "text" | "real" | "bool" | "datetime"
//   $opts : {"nullable":bool, "unique":bool, "defaultVal":val, "pk":bool,
//            "fk":"other_table.col"}
//   (keys are "nullable"/"defaultVal" because null/default are Bantu keywords.)
def field($name, $ftype, $opts) {
    dict $o = {};
    if (type($opts) != "null") { $o = $opts; }
    return {"name": $name, "ftype": $ftype, "opts": $o};
}

// pk($name) → an auto-increment primary-key field named $name.
def pk($name) {
    return {"name": $name, "ftype": "int", "opts": {"pk": true}};
}

// fk($name, $target) → an integer foreign-key field ("table.col").
def fk($name, $target) {
    return {"name": $name, "ftype": "int", "opts": {"fk": $target}};
}

// model($conn, $table, $fields) → a model bound to a connection.
// Automatically runs CREATE TABLE IF NOT EXISTS.
def model($conn, $table, $fields) {
    dict $m = {"conn": $conn, "table": $table, "fields": $fields};
    sync($m);
    return $m;
}

// _isPk($field) — true if the field is the auto primary key.
def _isPk($field) {
    if ($field.opts.pk) { return true; }
    return false;
}

// _columnDef($dialect, $field) → one column definition for CREATE TABLE.
def _columnDef($dialect, $field) {
    if (_isPk($field)) {
        return _ident($dialect, $field.name) + " " + $dialect.autopk;
    }
    string $colDef = _ident($dialect, $field.name) + " " + _columnType($dialect, $field.ftype);
    dict $o = $field.opts;
    if ($o.unique) { $colDef = $colDef + " UNIQUE"; }
    // Column is NOT NULL only when explicitly declared nullable:false.
    if (type($o.nullable) != "null") {
        if (!$o.nullable) { $colDef = $colDef + " NOT NULL"; }
    }
    if (type($o.defaultVal) != "null") {
        $colDef = $colDef + " DEFAULT " + _defaultExpr($dialect, $o.defaultVal);
    }
    if (type($o.fk) != "null") {
        list $parts = split($o.fk, ".");
        $colDef = $colDef + " REFERENCES " + _ident($dialect, $parts[0]) + "(" + _ident($dialect, $parts[1]) + ")";
    }
    return $colDef;
}

// _defaultExpr — a DEFAULT clause value ("now" maps to the backend NOW()).
def _defaultExpr($dialect, $v) {
    if (type($v) == "string") {
        if ($v == "now") { return $dialect.nowExpr; }
        if ($v == "CURRENT_TIMESTAMP") { return $dialect.nowExpr; }
    }
    return _escape($dialect, $v);
}

// _createTableSql($model) → the CREATE TABLE IF NOT EXISTS statement.
def _createTableSql($model) {
    dict $d = $model.conn.dialect;
    string $cols = "";
    number $i = 0;
    each ($f in $model.fields) {
        if ($i > 0) { $cols = $cols + ", "; }
        $cols = $cols + _columnDef($d, $f);
        $i = $i + 1;
    }
    return "CREATE TABLE IF NOT EXISTS " + _ident($d, $model.table) + " (" + $cols + ");";
}

// sync($model) — create the table if it does not exist. Idempotent.
def sync($model) {
    return _exec($model.conn, _createTableSql($model), []);
}

// drop($model) — DROP TABLE IF EXISTS.
def drop($model) {
    dict $d = $model.conn.dialect;
    return _exec($model.conn, "DROP TABLE IF EXISTS " + _ident($d, $model.table) + ";", []);
}


// ─── Writes: create / bulkCreate / raw update helpers ───────────────────

// _insertColumns($model) → list of non-pk column names (the writable set).
def _insertColumns($model) {
    list $cols = [];
    each ($f in $model.fields) {
        if (!_isPk($f)) { $cols[len($cols)] = $f.name; }
    }
    return $cols;
}

// insert($model, $data) → INSERT one row. Returns {"id":.., "changes":.., "ok":..}.
// Only columns present (non-null) in $data are written; the rest fall back to
// their database defaults. (Named insert() because `create` is a Bantu keyword.)
def insert($model, $data) {
    dict $d = $model.conn.dialect;
    dict $b = _binder($d);
    string $colSql = "";
    string $valSql = "";
    number $i = 0;
    each ($col in _insertColumns($model)) {
        // read value by dynamic key; treat null as "not provided".
        if (type($data[$col]) != "null") {
            if ($i > 0) { $colSql = $colSql + ", "; $valSql = $valSql + ", "; }
            $colSql = $colSql + _ident($d, $col);
            $valSql = $valSql + _bind($b, $data[$col]);
            $i = $i + 1;
        }
    }
    string $sql = "INSERT INTO " + _ident($d, $model.table) + " (" + $colSql + ") VALUES (" + $valSql + ");";
    dict $r = _exec($model.conn, $sql, $b.params);
    dict $out = {"ok": true, "changes": 0, "id": 0};
    if (type($r.success) != "null") { $out.ok = $r.success; }
    if (type($r.changes) != "null") { $out.changes = $r.changes; }
    if (type($r.lastInsertId) != "null") { $out.id = $r.lastInsertId; }
    return $out;
}

// bulkCreate($model, $rows) → a single multi-row INSERT. Returns {"count":..,"ok":..}.
// Uses the full writable column set for every row (missing values become NULL,
// so provide values for NOT NULL columns without a default).
def bulkCreate($model, $rows) {
    dict $d = $model.conn.dialect;
    dict $b = _binder($d);
    list $cols = _insertColumns($model);
    string $colSql = "";
    number $ci = 0;
    each ($c in $cols) {
        if ($ci > 0) { $colSql = $colSql + ", "; }
        $colSql = $colSql + _ident($d, $c);
        $ci = $ci + 1;
    }
    string $rowsSql = "";
    number $ri = 0;
    each ($row in $rows) {
        if ($ri > 0) { $rowsSql = $rowsSql + ", "; }
        string $vals = "";
        number $vi = 0;
        each ($c in $cols) {
            if ($vi > 0) { $vals = $vals + ", "; }
            $vals = $vals + _bind($b, $row[$c]);
            $vi = $vi + 1;
        }
        $rowsSql = $rowsSql + "(" + $vals + ")";
        $ri = $ri + 1;
    }
    string $sql = "INSERT INTO " + _ident($d, $model.table) + " (" + $colSql + ") VALUES " + $rowsSql + ";";
    dict $r = _exec($model.conn, $sql, $b.params);
    dict $out = {"ok": true, "count": $ri};
    if (type($r.success) != "null") { $out.ok = $r.success; }
    return $out;
}


// ─── Lookup parsing & condition compilation ─────────────────────────────

// _endsWith($s, $suf) → true if $s ends with $suf.
def _endsWith($s, $suf) {
    number $ls = len($s);
    number $lf = len($suf);
    if ($lf > $ls) { return false; }
    return substr($s, $ls - $lf, $lf) == $suf;
}

// _parseLookup($field) → {"col":.., "op":..}. Splits a Django-style
// "col__lookup" into its column and operator (defaulting to equality).
def _parseLookup($field) {
    // most specific first so "icontains" wins over "contains".
    list $lks = ["gte", "lte", "gt", "lt", "ne", "in",
                 "icontains", "contains", "istartswith", "startswith",
                 "endswith", "isnull"];
    each ($lk in $lks) {
        if (_endsWith($field, "__" + $lk)) {
            number $cut = len($field) - len($lk) - 2;
            return {"col": substr($field, 0, $cut), "op": $lk};
        }
    }
    return {"col": $field, "op": "eq"};
}

// _cond($dialect, $col, $op, $value) → a single SQL condition string.
// Values are bound through $b (placeholders) rather than inlined, so untrusted
// input never reaches the SQL text on backends that support parameters.
def _cond($b, $col, $op, $value) {
    string $c = _ident($b.dialect, $col);

    if ($op == "eq") {
        if (type($value) == "null") { return $c + " IS NULL"; }
        return $c + " = " + _bind($b, $value);
    }
    if ($op == "ne") {
        if (type($value) == "null") { return $c + " IS NOT NULL"; }
        return $c + " <> " + _bind($b, $value);
    }
    if ($op == "gt") { return $c + " > " + _bind($b, $value); }
    if ($op == "gte") { return $c + " >= " + _bind($b, $value); }
    if ($op == "lt") { return $c + " < " + _bind($b, $value); }
    if ($op == "lte") { return $c + " <= " + _bind($b, $value); }
    if ($op == "in") { return $c + " IN (" + _bindList($b, $value) + ")"; }
    if ($op == "contains") {
        return $c + " LIKE " + _bind($b, "%" + str($value) + "%");
    }
    if ($op == "icontains") {
        return "LOWER(" + $c + ") LIKE LOWER(" + _bind($b, "%" + str($value) + "%") + ")";
    }
    if ($op == "startswith") {
        return $c + " LIKE " + _bind($b, str($value) + "%");
    }
    if ($op == "istartswith") {
        return "LOWER(" + $c + ") LIKE LOWER(" + _bind($b, str($value) + "%") + ")";
    }
    if ($op == "endswith") {
        return $c + " LIKE " + _bind($b, "%" + str($value));
    }
    if ($op == "isnull") {
        if ($value) { return $c + " IS NULL"; }
        return $c + " IS NOT NULL";
    }
    // unknown operator → safe equality fallback
    return $c + " = " + _bind($b, $value);
}

// _compilePairs($dialect, $pairs) → AND-joined condition string from a list of
// [field_lookup, value] pairs.
def _compilePairs($b, $pairs) {
    string $out = "";
    number $i = 0;
    each ($p in $pairs) {
        dict $lk = _parseLookup($p[0]);
        string $frag = _cond($b, $lk.col, $lk.op, $p[1]);
        if ($i > 0) { $out = $out + " AND "; }
        $out = $out + $frag;
        $i = $i + 1;
    }
    return $out;
}

// _and($a, $b) → combine two WHERE fragments with AND (skipping empties).
def _and($a, $b) {
    if ($a == "") { return $b; }
    if ($b == "") { return $a; }
    return "(" + $a + ") AND (" + $b + ")";
}


// ─── Q objects (complex AND / OR / NOT trees) ───────────────────────────

// Q($pairs) → a leaf predicate (its pairs ANDed together).
def Q($pairs) { return {"__q": true, "kind": "leaf", "pairs": $pairs}; }
// qAnd/qOr/qNot → combine Q nodes.
def qAnd($a, $b) { return {"__q": true, "kind": "and", "left": $a, "right": $b}; }
def qOr($a, $b) { return {"__q": true, "kind": "or", "left": $a, "right": $b}; }
def qNot($a) { return {"__q": true, "kind": "not", "node": $a}; }

// _compileQ($dialect, $q) → SQL string for a Q tree.
def _compileQ($b, $q) {
    if ($q.kind == "leaf") { return "(" + _compilePairs($b, $q.pairs) + ")"; }
    if ($q.kind == "and") {
        return "(" + _compileQ($b, $q.left) + " AND " + _compileQ($b, $q.right) + ")";
    }
    if ($q.kind == "or") {
        return "(" + _compileQ($b, $q.left) + " OR " + _compileQ($b, $q.right) + ")";
    }
    if ($q.kind == "not") {
        return "(NOT " + _compileQ($b, $q.node) + ")";
    }
    return "1=1";
}


// ─── Expression / raw markers ───────────────────────────────────────────

// raw($sql) → inject a raw SQL fragment (e.g. a default or update expression).
def raw($sql) { return {"__raw": true, "sql": $sql}; }
// F($col) → a column reference (Django F-expression), e.g. update views to
// orm.raw(orm.colref($m,"views") + " + 1").
def F($col) { return {"__f": true, "col": $col}; }


// ─── QuerySet (lazy, chainable, string-accumulating) ────────────────────

// query($model) → a fresh QuerySet over $model.
// The queryset carries its own binder, so WHERE values accumulate as bound
// parameters (in qs.binder.params) alongside the SQL text.
def query($model) {
    return {
        "model": $model,
        "binder": _binder($model.conn.dialect),
        "where": "",
        "order": "",
        "limitN": -1,
        "offsetN": 0,
        "cols": "*",
        "distinct": false
    };
}

// _dialectOf($qs) → the active dialect for a queryset.
def _dialectOf($qs) { return $qs.model.conn.dialect; }

// _paramsOf($qs) → the values bound by this queryset's WHERE clause.
def _paramsOf($qs) { return $qs.binder.params; }

// filter($qs, $pairs) → AND the given [field,value] pairs into WHERE.
def filter($qs, $pairs) {
    string $frag = _compilePairs($qs.binder, $pairs);
    $qs.where = _and($qs.where, $frag);
    return $qs;
}

// exclude($qs, $pairs) → AND NOT (pairs) into WHERE.
def exclude($qs, $pairs) {
    string $frag = _compilePairs($qs.binder, $pairs);
    if ($frag != "") { $qs.where = _and($qs.where, "NOT (" + $frag + ")"); }
    return $qs;
}

// filterQ($qs, $q) → AND a Q tree into WHERE (for OR / NOT logic).
def filterQ($qs, $q) {
    $qs.where = _and($qs.where, _compileQ($qs.binder, $q));
    return $qs;
}

// whereRaw($qs, $sql) → AND a raw SQL predicate into WHERE.
def whereRaw($qs, $sql) {
    $qs.where = _and($qs.where, $sql);
    return $qs;
}

// orderBy($qs, $fields) → ORDER BY. Prefix a field with "-" for DESC.
def orderBy($qs, $fields) {
    dict $d = _dialectOf($qs);
    string $ord = "";
    number $i = 0;
    each ($f in $fields) {
        string $name = $f;
        string $dir = "ASC";
        if (substr($f, 0, 1) == "-") {
            $name = substr($f, 1, len($f) - 1);
            $dir = "DESC";
        }
        if ($i > 0) { $ord = $ord + ", "; }
        $ord = $ord + _ident($d, $name) + " " + $dir;
        $i = $i + 1;
    }
    $qs.order = $ord;
    return $qs;
}

// limit / offset.
def limit($qs, $n) { $qs.limitN = $n; return $qs; }
def offset($qs, $n) { $qs.offsetN = $n; return $qs; }

// values($qs, $cols) → project only the given columns.
def values($qs, $cols) {
    dict $d = _dialectOf($qs);
    string $c = "";
    number $i = 0;
    each ($col in $cols) {
        if ($i > 0) { $c = $c + ", "; }
        $c = $c + _ident($d, $col);
        $i = $i + 1;
    }
    $qs.cols = $c;
    return $qs;
}

// distinct($qs) → SELECT DISTINCT.
def distinct($qs) { $qs.distinct = true; return $qs; }

// _buildSelect($qs) → the SELECT statement for the current queryset.
def _buildSelect($qs) {
    dict $d = _dialectOf($qs);
    string $sql = "SELECT ";
    if ($qs.distinct) { $sql = $sql + "DISTINCT "; }
    $sql = $sql + $qs.cols + " FROM " + _ident($d, $qs.model.table);
    if ($qs.where != "") { $sql = $sql + " WHERE " + $qs.where; }
    if ($qs.order != "") { $sql = $sql + " ORDER BY " + $qs.order; }
    if ($qs.limitN >= 0) { $sql = $sql + " LIMIT " + str($qs.limitN); }
    if ($qs.offsetN > 0) { $sql = $sql + " OFFSET " + str($qs.offsetN); }
    return $sql + ";";
}

// ─── Terminal operations (execute the query) ────────────────────────────

// all($qs) → LIST of row dicts.
def all($qs) {
    return _query($qs.model.conn, _buildSelect($qs), _paramsOf($qs));
}

// first($qs) → the first row dict, or null.
def first($qs) {
    limit($qs, 1);
    list $rows = all($qs);
    if (len($rows) == 0) { return null; }
    return $rows[0];
}

// get($qs, $pairs) → filter by pairs then return the single matching row (or null).
def get($qs, $pairs) {
    filter($qs, $pairs);
    return first($qs);
}

// count($qs) → number of matching rows.
def count($qs) {
    dict $d = _dialectOf($qs);
    string $sql = "SELECT COUNT(*) AS n FROM " + _ident($d, $qs.model.table);
    if ($qs.where != "") { $sql = $sql + " WHERE " + $qs.where; }
    $sql = $sql + ";";
    list $rows = _query($qs.model.conn, $sql, _paramsOf($qs));
    if (len($rows) == 0) { return 0; }
    return num($rows[0].n);
}

// exists($qs) → true if at least one row matches.
def exists($qs) {
    return count($qs) > 0;
}

// aggregate($qs, $expr, $alias) → a scalar aggregate, e.g.
// aggregate($qs, "SUM(amount)", "total") → number.
def aggregate($qs, $expr, $alias) {
    dict $d = _dialectOf($qs);
    string $sql = "SELECT " + $expr + " AS " + _ident($d, $alias) + " FROM " + _ident($d, $qs.model.table);
    if ($qs.where != "") { $sql = $sql + " WHERE " + $qs.where; }
    $sql = $sql + ";";
    list $rows = _query($qs.model.conn, $sql, _paramsOf($qs));
    if (len($rows) == 0) { return null; }
    return $rows[0][$alias];
}


// ─── QuerySet writes: update / delete ───────────────────────────────────

// modify($qs, $data) → UPDATE matching rows. Only non-null keys in $data (read
// against the model's columns) are written. Returns {"changes":..,"ok":..}.
// (Named modify() because `update` is a Bantu keyword.)
def modify($qs, $data) {
    dict $model = $qs.model;
    dict $d = $model.conn.dialect;

    // Placeholder ordering differs by dialect:
    //  * "$n" (postgres) — placeholders are numbered explicitly, so SET values
    //    can just continue the queryset's binder (they become $k+1…), and the
    //    params list stays [where…, set…].
    //  * "?"  (sqlite)   — binding is positional by TEXT order, and SET appears
    //    before WHERE, so SET values must be bound into a fresh binder and
    //    placed FIRST: [set…, where…].
    bool $numbered = ($d.paramStyle == "$n");
    dict $b = _binder($d);
    if ($numbered) { $b = $qs.binder; }

    string $set = "";
    number $i = 0;
    each ($col in _insertColumns($model)) {
        if (type($data[$col]) != "null") {
            if ($i > 0) { $set = $set + ", "; }
            $set = $set + _ident($d, $col) + " = " + _bind($b, $data[$col]);
            $i = $i + 1;
        }
    }

    list $params = [];
    if ($numbered) {
        $params = $qs.binder.params;
    } else {
        $params = $b.params;
        extend($params, _paramsOf($qs));
    }

    string $sql = "UPDATE " + _ident($d, $model.table) + " SET " + $set;
    if ($qs.where != "") { $sql = $sql + " WHERE " + $qs.where; }
    $sql = $sql + ";";
    dict $r = _exec($model.conn, $sql, $params);
    dict $out = {"ok": true, "changes": 0};
    if (type($r.success) != "null") { $out.ok = $r.success; }
    if (type($r.changes) != "null") { $out.changes = $r.changes; }
    return $out;
}

// remove($qs) → DELETE matching rows. Refuses to run without a WHERE clause
// unless $force is true (guards against accidental full-table wipes).
def remove($qs, $force) {
    dict $model = $qs.model;
    dict $d = $model.conn.dialect;
    if ($qs.where == "") {
        if (!$force) {
            return {"ok": false, "error": "remove() without a filter needs force=true"};
        }
    }
    string $sql = "DELETE FROM " + _ident($d, $model.table);
    if ($qs.where != "") { $sql = $sql + " WHERE " + $qs.where; }
    $sql = $sql + ";";
    dict $r = _exec($model.conn, $sql, _paramsOf($qs));
    dict $out = {"ok": true, "changes": 0};
    if (type($r.success) != "null") { $out.ok = $r.success; }
    if (type($r.changes) != "null") { $out.changes = $r.changes; }
    return $out;
}


// ─── Migrations (ultraorm-style auto-diff) ──────────────────────────────

// _listHas($items, $val) → membership test.
def _listHas($items, $val) {
    each ($x in $items) {
        if ($x == $val) { return true; }
    }
    return false;
}

// _existingColumns($conn, $table) → list of column names currently in the DB
// (empty list if the table does not exist).
def _existingColumns($conn, $table) {
    list $names = [];
    if ($conn.driver == "sqlite") {
        list $rows = _query($conn, "PRAGMA table_info(" + _ident($conn.dialect, $table) + ");", []);
        each ($r in $rows) {
            if (type($r.name) != "null") { $names[len($names)] = $r.name; }
        }
        return $names;
    }
    // postgres / mysql — bind the table name through the dialect's own
    // placeholder style ("$1" on postgres, escaped literal on mysql).
    dict $b = _binder($conn.dialect);
    string $ph = _bind($b, $table);
    list $rows = _query($conn,
        "SELECT column_name FROM information_schema.columns WHERE table_name = " + $ph + ";", $b.params);
    each ($r in $rows) {
        if (type($r.column_name) != "null") { $names[len($names)] = $r.column_name; }
    }
    return $names;
}

// migrate($models, $opts) → bring the schema in line with the models.
//   $opts: {"dryRun": bool}
// Returns the list of SQL statements that were (or would be) run.
def migrate($models, $opts) {
    bool $dry = false;
    if (type($opts) != "null") {
        if ($opts.dryRun) { $dry = true; }
    }
    list $statements = [];
    each ($model in $models) {
        dict $conn = $model.conn;
        dict $d = $conn.dialect;
        list $existing = _existingColumns($conn, $model.table);
        if (len($existing) == 0) {
            // brand-new table
            $statements[len($statements)] = _createTableSql($model);
        } else {
            // add any columns the model has that the table lacks
            each ($f in $model.fields) {
                if (!_isPk($f)) {
                    if (!_listHas($existing, $f.name)) {
                        string $alter = "ALTER TABLE " + _ident($d, $model.table) +
                            " ADD COLUMN " + _columnDef($d, $f) + ";";
                        $statements[len($statements)] = $alter;
                    }
                }
            }
        }
    }
    if (!$dry) {
        each ($sql in $statements) {
            _exec($models[0].conn, $sql, []);
        }
    }
    return $statements;
}

// colref($model, $col) → a fully-dialect-quoted column reference string,
// handy when composing raw() expressions like orm.raw(orm.colref($m,"n")+" + 1").
def colref($model, $col) {
    return _ident($model.conn.dialect, $col);
}
