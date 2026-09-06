// ════════════════════════════════════════════════════════════════════════════
//  arctic.b — a data-science DataFrame library for Bantu.
//
//  The power of pandas + polars, made simple. Written in PURE Bantu on top of
//  the native column primitives (the `col_*` atoms). If you know spreadsheets,
//  you can use this.
//
//      include "./arctic.b" as arctic;
//
//      $df = arctic.read_csv("sales.csv");
//      print($df.head().show());
//      $big = $df.query("amount > 1000 and region == 'EU'")
//                .select(["region", "amount"])
//                .sort("amount", true);
//      $by  = $df.groupby("region").agg([["amount", "sum", "total"]]);
//      print($by.show());
//
//  Two ways to work, both easy:
//    • query("amount > 1000 and region == 'EU'")  — the plain-English filter path
//    • $df.get("amount").gt(1000)                  — composable Series expressions
//
//  Requires an interpreter with the native `col` primitives (has_native("col")).
// ════════════════════════════════════════════════════════════════════════════

// Guard: these need the native column atoms.
$_ARCTIC_OK = false;
try { $_ARCTIC_OK = has_native("col"); } catch ($e) { $_ARCTIC_OK = false; }
def _need() {
    if (!$_ARCTIC_OK) {
        throw "arctic: this interpreter lacks the native column primitives (need has_native('col')). Rebuild Bantu.";
    }
    return null;
}

// Capture the native I/O builtins NOW, before our public read_csv/read_sqlite
// defs below shadow those names inside this module. (Defs are not hoisted over
// top-level statements, so this reference stays the native one.)
$_native_read_csv = null;
$_native_read_sqlite = null;
if ($_ARCTIC_OK) {
    $_native_read_csv = read_csv;
    $_native_read_sqlite = read_sqlite;
}


// ════════════════════════════════════════════════════════════════════════════
//  Series — one named column, with arithmetic, comparisons and summaries.
// ════════════════════════════════════════════════════════════════════════════
class Series {
    def init($name, $col) { this.name = $name; this.col = $col; }

    // introspection
    def len()      { return col_len(this.col); }
    def dtype()    { return col_dtype(this.col); }
    def to_list()  { return col_to_list(this.col); }
    def get($i)    { return col_get(this.col, $i); }
    def rename($n) { return new Series($n, this.col); }
    def alias($n)  { return new Series($n, this.col); }

    // internal: pull the native column out of a Series OR pass a scalar through
    def _operand($x) {
        if (type($x) == "instance") { return $x.col; }
        return $x;
    }

    // arithmetic (Series or scalar) -> Series
    def add($x) { return new Series(this.name, col_add(this.col, this._operand($x))); }
    def sub($x) { return new Series(this.name, col_sub(this.col, this._operand($x))); }
    def mul($x) { return new Series(this.name, col_mul(this.col, this._operand($x))); }
    def div($x) { return new Series(this.name, col_div(this.col, this._operand($x))); }
    def mod($x) { return new Series(this.name, col_mod(this.col, this._operand($x))); }
    def pow($x) { return new Series(this.name, col_pow(this.col, this._operand($x))); }
    def neg()   { return new Series(this.name, col_neg(this.col)); }
    def abs()   { return new Series(this.name, col_abs(this.col)); }

    // comparisons (Series or scalar) -> Series (boolean mask)
    def gt($x) { return new Series(this.name, col_gt(this.col, this._operand($x))); }
    def ge($x) { return new Series(this.name, col_ge(this.col, this._operand($x))); }
    def lt($x) { return new Series(this.name, col_lt(this.col, this._operand($x))); }
    def le($x) { return new Series(this.name, col_le(this.col, this._operand($x))); }
    def eq($x) { return new Series(this.name, col_eq(this.col, this._operand($x))); }
    def ne($x) { return new Series(this.name, col_ne(this.col, this._operand($x))); }

    // boolean-mask logic
    def and_($x) { return new Series(this.name, col_and(this.col, this._operand($x))); }
    def or_($x)  { return new Series(this.name, col_or(this.col, this._operand($x))); }
    def not_()   { return new Series(this.name, col_not(this.col)); }

    // nulls
    def is_null()       { return new Series(this.name, col_is_null(this.col)); }
    def null_count()    { return col_null_count(this.col); }
    def fill_null($v)   { return new Series(this.name, col_fill_null(this.col, $v)); }
    def cast($dtype)    { return new Series(this.name, col_cast(this.col, $dtype)); }

    // summaries -> scalar
    def sum()     { return col_sum(this.col); }
    def mean()    { return col_mean(this.col); }
    def min()     { return col_min(this.col); }
    def max()     { return col_max(this.col); }
    def std()     { return col_std(this.col); }
    def var()     { return col_var(this.col); }
    def median()  { return col_median(this.col); }
    def count()   { return col_count(this.col); }
    def nunique() { return col_nunique(this.col); }
    def any_()    { return col_any(this.col); }   // `any` is a reserved word
    def all_()    { return col_all(this.col); }    // (paired with and_/or_/not_)

    // ordering / selection -> Series
    def sort($desc)      { return new Series(this.name, col_take(this.col, col_argsort(this.col, $desc))); }
    def argsort($desc)   { return new Series(this.name, col_argsort(this.col, $desc)); }
    def take($idxSeries) { return new Series(this.name, col_take(this.col, this._operand($idxSeries))); }
    def filter($mask)    { return new Series(this.name, col_filter(this.col, this._operand($mask))); }
    def head($n)         { return new Series(this.name, col_head(this.col, $n)); }
    def tail($n)         { return new Series(this.name, col_tail(this.col, $n)); }
}


// ════════════════════════════════════════════════════════════════════════════
//  DataFrame — ordered named columns.
// ════════════════════════════════════════════════════════════════════════════
class DataFrame {
    // $names: list of column names (order); $cols: dict name -> native column.
    def init($names, $cols) {
        this.names = $names;
        this.cols = $cols;
        this.ncols = len($names);
        if (this.ncols == 0) { this.nrows = 0; }
        else { this.nrows = col_len($cols[$names[0]]); }
    }

    def shape()   { return [this.nrows, this.ncols]; }
    def columns() { return this.names; }
    def width()   { return this.ncols; }
    def height()  { return this.nrows; }

    def has($name) {
        each ($n in this.names) { if ($n == $name) { return true; } }
        return false;
    }

    // a single column as a Series
    def get($name) {
        if (!this.has($name)) { throw "arctic: column '" + $name + "' not found"; }
        return new Series($name, this.cols[$name]);
    }

    // choose a subset of columns (in the given order) -> DataFrame
    def select($names) {
        $c = {};
        each ($n in $names) {
            if (!this.has($n)) { throw "arctic: column '" + $n + "' not found"; }
            $c[$n] = this.cols[$n];
        }
        return new DataFrame($names, $c);
    }

    // drop columns -> DataFrame
    def drop($names) {
        $keep = [];
        each ($n in this.names) {
            $drop = false;
            each ($d in $names) { if ($d == $n) { $drop = true; } }
            if (!$drop) { $keep[len($keep)] = $n; }
        }
        return this.select($keep);
    }

    // rename via a {old: new} dict -> DataFrame
    def rename($map) {
        $newNames = [];
        $c = {};
        each ($n in this.names) {
            $nn = $n;
            if (keyIn($map, $n)) { $nn = $map[$n]; }
            $newNames[len($newNames)] = $nn;
            $c[$nn] = this.cols[$n];
        }
        return new DataFrame($newNames, $c);
    }

    // add or replace a column (accepts a Series or a native column) -> DataFrame
    def with_column($name, $value) {
        $col = $value;
        if (type($value) == "instance") { $col = $value.col; }
        $newNames = [];
        each ($n in this.names) { $newNames[len($newNames)] = $n; }
        if (!this.has($name)) { $newNames[len($newNames)] = $name; }
        $c = {};
        each ($n in this.names) { $c[$n] = this.cols[$n]; }
        $c[$name] = $col;
        return new DataFrame($newNames, $c);
    }

    // keep rows where the mask (Series or column) is true -> DataFrame
    def filter($mask) {
        $m = $mask;
        if (type($mask) == "instance") { $m = $mask.col; }
        $c = {};
        each ($n in this.names) { $c[$n] = col_filter(this.cols[$n], $m); }
        return new DataFrame(this.names, $c);
    }

    // sort all columns by one column -> DataFrame
    def sort($name, $desc) {
        if (!this.has($name)) { throw "arctic: column '" + $name + "' not found"; }
        $order = col_argsort(this.cols[$name], $desc);
        $c = {};
        each ($n in this.names) { $c[$n] = col_take(this.cols[$n], $order); }
        return new DataFrame(this.names, $c);
    }

    def head($n) {
        $k = 5;
        if ($n != null) { $k = $n; }
        $c = {};
        each ($col in this.names) { $c[$col] = col_head(this.cols[$col], $k); }
        return new DataFrame(this.names, $c);
    }
    def tail($n) {
        $k = 5;
        if ($n != null) { $k = $n; }
        $c = {};
        each ($col in this.names) { $c[$col] = col_tail(this.cols[$col], $k); }
        return new DataFrame(this.names, $c);
    }

    def groupby($keys) {
        $klist = $keys;
        if (type($keys) == "string") { $klist = [$keys]; }
        return new GroupBy(this, $klist);
    }

    // join on a shared key column -> DataFrame. $how ∈ inner/left/right/outer.
    def join($other, $on, $how) {
        if (!this.has($on)) { throw "arctic: join key '" + $on + "' not in left frame"; }
        if (!$other.has($on)) { throw "arctic: join key '" + $on + "' not in right frame"; }
        $j = col_join(this.cols[$on], $other.cols[$on], $how);
        $names = [];
        $c = {};
        each ($n in this.names) {
            $names[len($names)] = $n;
            $c[$n] = col_take(this.cols[$n], $j.left_idx);
        }
        each ($n in $other.names) {
            if ($n != $on) {
                $nn = $n;
                if (this.has($n)) { $nn = $n + "_right"; }
                $names[len($names)] = $nn;
                $c[$nn] = col_take($other.cols[$n], $j.right_idx);
            }
        }
        return new DataFrame($names, $c);
    }

    // summary statistics per numeric column -> DataFrame
    def describe() {
        $stats = ["count", "mean", "std", "min", "median", "max"];
        $names = ["stat"];
        $cols = {};
        $cols["stat"] = col($stats, "utf8");
        each ($n in this.names) {
            $dt = col_dtype(this.cols[$n]);
            $isNum = false;
            if ($dt == "i64") { $isNum = true; }
            else if ($dt == "f64") { $isNum = true; }
            if ($isNum) {
                $cc = this.cols[$n];
                $vals = [
                    col_count($cc), col_mean($cc), col_std($cc),
                    col_min($cc), col_median($cc), col_max($cc)
                ];
                $names[len($names)] = $n;
                $cols[$n] = col($vals, "f64");
            }
        }
        return new DataFrame($names, $cols);
    }

    // plain-English filtering: query("amount > 1000 and region == 'EU'")
    def query($expr) {
        return this.filter(_evalQuery(this, $expr));
    }

    def to_csv($path) {
        return write_csv(this.names, this.cols, $path);
    }

    // a raw {names, cols, shape} frame dict (e.g. to pass to write_csv directly)
    def to_frame() {
        return {"names": this.names, "cols": this.cols, "shape": [this.nrows, this.ncols]};
    }

    // pretty ASCII table (first $n rows; default 10)
    def show($n) {
        return _render(this, $n);
    }
}


// ════════════════════════════════════════════════════════════════════════════
//  GroupBy — produced by DataFrame.groupby(...).
// ════════════════════════════════════════════════════════════════════════════
class GroupBy {
    def init($df, $keys) { this.df = $df; this.keys = $keys; }

    def _keyCols() {
        $kc = [];
        each ($k in this.keys) { $kc[len($kc)] = this.df.cols[$k]; }
        return $kc;
    }

    // agg([[colName, op, outName], ...]) -> DataFrame
    // op ∈ sum mean min max std var median count nunique any all
    def agg($specs) {
        $kc = this._keyCols();
        $result = null;
        $keyNames = this.keys;
        $names = [];
        $cols = {};
        $first = true;
        each ($spec in $specs) {
            $colName = $spec[0];
            $op = $spec[1];
            $outName = $colName + "_" + $op;
            if (len($spec) > 2) { $outName = $spec[2]; }
            $g = col_group_agg($kc, this.df.cols[$colName], $op);
            if ($first) {
                // fill in the key columns once, from the first aggregation
                $i = 0;
                each ($kn in $keyNames) {
                    $names[len($names)] = $kn;
                    $cols[$kn] = $g.keys[$i];
                    $i = $i + 1;
                }
                $first = false;
            }
            $names[len($names)] = $outName;
            $cols[$outName] = $g.values;
        }
        return new DataFrame($names, $cols);
    }

    // convenience: sum("amount") -> DataFrame
    def sum($colName)  { return this.agg([[$colName, "sum", $colName]]); }
    def mean($colName) { return this.agg([[$colName, "mean", $colName]]); }
    def min($colName)  { return this.agg([[$colName, "min", $colName]]); }
    def max($colName)  { return this.agg([[$colName, "max", $colName]]); }
    def count($colName){ return this.agg([[$colName, "count", $colName]]); }
}


// ════════════════════════════════════════════════════════════════════════════
//  Constructors / readers (the arctic.* entry points)
// ════════════════════════════════════════════════════════════════════════════

// read_csv(path, options?) -> DataFrame
def read_csv($path, $options) {
    _need();
    $raw = $_native_read_csv($path, $options);
    return new DataFrame($raw.names, $raw.cols);
}

// read_sqlite(path, query) -> DataFrame
def read_sqlite($path, $sql) {
    _need();
    $raw = $_native_read_sqlite($path, $sql);
    return new DataFrame($raw.names, $raw.cols);
}

// dataframe({name: list, ...}, dtypes?) -> DataFrame
// Build from Bantu lists; dtype inferred per column unless given in $dtypes.
def dataframe($data, $dtypes) {
    _need();
    $names = [];
    $cols = {};
    each ($k in keys($data)) {
        $names[len($names)] = $k;
        $dt = _inferDtype($data[$k]);
        if ($dtypes != null) {
            if (keyIn($dtypes, $k)) { $dt = $dtypes[$k]; }
        }
        $cols[$k] = col($data[$k], $dt);
    }
    return new DataFrame($names, $cols);
}

// series(name, list, dtype?) -> Series
def series($name, $list, $dtype) {
    _need();
    $dt = $dtype;
    if ($dt == null) { $dt = _inferDtype($list); }
    return new Series($name, col($list, $dt));
}


// ════════════════════════════════════════════════════════════════════════════
//  Small helpers (pure Bantu)
// ════════════════════════════════════════════════════════════════════════════

def keyIn($dict, $key) {
    each ($k in keys($dict)) { if ($k == $key) { return true; } }
    return false;
}

// infer a column dtype from a Bantu list of values
def _inferDtype($list) {
    $allInt = true; $allNum = true; $allBool = true; $any = false;
    each ($v in $list) {
        if ($v != null) {
            $any = true;
            $t = type($v);
            if ($t == "number") {
                if ($v != floor($v)) { $allInt = false; }
            } else {
                $allInt = false; $allNum = false;
                if ($t != "bool") { $allBool = false; }
            }
        }
    }
    if (!$any) { return "utf8"; }
    if ($allInt) { return "i64"; }
    if ($allNum) { return "f64"; }
    if ($allBool) { return "bool"; }
    return "utf8";
}


// ─── pretty-printer ─────────────────────────────────────────────────────────
def _repeat($ch, $n) {
    $s = "";
    $i = 0;
    while ($i < $n) { $s = $s + $ch; $i = $i + 1; }
    return $s;
}
def _pad($s, $w) {
    $out = $s;
    while (len($out) < $w) { $out = $out + " "; }
    return $out;
}
def _render($df, $n) {
    $k = 10;
    if ($n != null) { $k = $n; }
    $view = $df.head($k);
    $rows = $view.nrows;
    $widths = {};
    $cells = {};
    each ($name in $view.names) {
        $lst = col_to_list($view.cols[$name]);
        $w = len($name);
        $scol = [];
        each ($v in $lst) {
            $s = "null";
            if ($v != null) { $s = str($v); }
            $scol[len($scol)] = $s;
            if (len($s) > $w) { $w = len($s); }
        }
        $widths[$name] = $w;
        $cells[$name] = $scol;
    }
    $out = "";
    $header = "";
    each ($name in $view.names) { $header = $header + _pad($name, $widths[$name]) + "  "; }
    $out = $header + "\n";
    $sep = "";
    each ($name in $view.names) { $sep = $sep + _repeat("-", $widths[$name]) + "  "; }
    $out = $out + $sep + "\n";
    $r = 0;
    while ($r < $rows) {
        $line = "";
        each ($name in $view.names) { $line = $line + _pad($cells[$name][$r], $widths[$name]) + "  "; }
        $out = $out + $line + "\n";
        $r = $r + 1;
    }
    $out = $out + "[" + str($df.nrows) + " rows x " + str($df.ncols) + " cols]";
    return $out;
}


// ─── query() string DSL ─────────────────────────────────────────────────────
// Grammar (evaluated left-to-right; no operator precedence between and/or):
//   expr      := predicate ( ('and'|'or') predicate )*
//   predicate := IDENT OP VALUE
//   OP        := ==  !=  >  >=  <  <=
//   VALUE     := number | 'text' | "text" | true | false | null
def _isDigitCh($ch) {
    $c = ord($ch);
    if ($c >= 48) { if ($c <= 57) { return true; } }
    return false;
}
def _isAlphaCh($ch) {
    $c = ord($ch);
    if ($c >= 65) { if ($c <= 90) { return true; } }   // A-Z
    if ($c >= 97) { if ($c <= 122) { return true; } }  // a-z
    if ($ch == "_") { return true; }
    return false;
}
def _isOpCh($ch) {
    if ($ch == "=") { return true; }
    if ($ch == "!") { return true; }
    if ($ch == ">") { return true; }
    if ($ch == "<") { return true; }
    return false;
}
def _tokenizeQuery($s) {
    $toks = [];
    $i = 0;
    $n = len($s);
    while ($i < $n) {
        $ch = substr($s, $i, 1);
        if ($ch == " ") { $i = $i + 1; continue; }
        // quoted string
        if ($ch == "'") { $i = $i + 1; $buf = "";
            while ($i < $n) { $c = substr($s, $i, 1); if ($c == "'") { $i = $i + 1; break; } $buf = $buf + $c; $i = $i + 1; }
            $toks[len($toks)] = {"t": "str", "v": $buf}; continue; }
        if ($ch == "\"") { $i = $i + 1; $buf = "";
            while ($i < $n) { $c = substr($s, $i, 1); if ($c == "\"") { $i = $i + 1; break; } $buf = $buf + $c; $i = $i + 1; }
            $toks[len($toks)] = {"t": "str", "v": $buf}; continue; }
        // operator
        if (_isOpCh($ch)) {
            $op = $ch; $i = $i + 1;
            if ($i < $n) { $c2 = substr($s, $i, 1); if ($c2 == "=") { $op = $op + "="; $i = $i + 1; } }
            if ($op == "=") { $op = "=="; }
            $toks[len($toks)] = {"t": "op", "v": $op}; continue;
        }
        // number (optional leading '-' or digit or '.')
        $isNumStart = false;
        if (_isDigitCh($ch)) { $isNumStart = true; }
        else if ($ch == "-") { $isNumStart = true; }
        else if ($ch == ".") { $isNumStart = true; }
        if ($isNumStart) {
            $buf = "";
            $more = true;
            while ($more) {
                if ($i >= $n) { $more = false; }
                else {
                    $c = substr($s, $i, 1);
                    $ok = false;
                    if (_isDigitCh($c)) { $ok = true; }
                    else if ($c == ".") { $ok = true; }
                    else if ($c == "-") { $ok = true; }
                    if ($ok) { $buf = $buf + $c; $i = $i + 1; } else { $more = false; }
                }
            }
            $toks[len($toks)] = {"t": "num", "v": num($buf)}; continue;
        }
        // identifier / keyword
        if (_isAlphaCh($ch)) {
            $buf = "";
            $more = true;
            while ($more) {
                if ($i >= $n) { $more = false; }
                else {
                    $c = substr($s, $i, 1);
                    $ok = false;
                    if (_isAlphaCh($c)) { $ok = true; }
                    else if (_isDigitCh($c)) { $ok = true; }
                    if ($ok) { $buf = $buf + $c; $i = $i + 1; } else { $more = false; }
                }
            }
            if ($buf == "and") { $toks[len($toks)] = {"t": "and", "v": "and"}; }
            else if ($buf == "or") { $toks[len($toks)] = {"t": "or", "v": "or"}; }
            else if ($buf == "true") { $toks[len($toks)] = {"t": "bool", "v": true}; }
            else if ($buf == "false") { $toks[len($toks)] = {"t": "bool", "v": false}; }
            else if ($buf == "null") { $toks[len($toks)] = {"t": "null", "v": null}; }
            else { $toks[len($toks)] = {"t": "id", "v": $buf}; }
            continue;
        }
        throw "arctic.query: unexpected character '" + $ch + "'";
    }
    return $toks;
}
def _qPredicate($df, $toks, $pos) {
    $nt = len($toks);
    if (($pos.i + 2) >= $nt) { throw "arctic.query: incomplete condition"; }
    $idTok = $toks[$pos.i];
    $opTok = $toks[$pos.i + 1];
    $valTok = $toks[$pos.i + 2];
    if ($idTok.t != "id") { throw "arctic.query: expected a column name"; }
    if ($opTok.t != "op") { throw "arctic.query: expected a comparison operator after '" + $idTok.v + "'"; }
    $pos.i = $pos.i + 3;
    if (!$df.has($idTok.v)) { throw "arctic.query: column '" + $idTok.v + "' not found"; }
    $c = $df.cols[$idTok.v];
    $op = $opTok.v;
    if ($valTok.t == "null") {
        if ($op == "==") { return col_is_null($c); }
        if ($op == "!=") { return col_not(col_is_null($c)); }
        throw "arctic.query: only == / != can be used with null";
    }
    $v = $valTok.v;
    if ($op == "==") { return col_eq($c, $v); }
    if ($op == "!=") { return col_ne($c, $v); }
    if ($op == ">")  { return col_gt($c, $v); }
    if ($op == ">=") { return col_ge($c, $v); }
    if ($op == "<")  { return col_lt($c, $v); }
    if ($op == "<=") { return col_le($c, $v); }
    throw "arctic.query: unknown operator '" + $op + "'";
}
def _evalQuery($df, $expr) {
    $toks = _tokenizeQuery($expr);
    $nt = len($toks);
    if ($nt == 0) { throw "arctic.query: empty expression"; }
    $pos = {"i": 0};
    $mask = _qPredicate($df, $toks, $pos);
    while ($pos.i < $nt) {
        $t = $toks[$pos.i];
        if ($t.t == "and") { $pos.i = $pos.i + 1; $mask = col_and($mask, _qPredicate($df, $toks, $pos)); }
        else if ($t.t == "or") { $pos.i = $pos.i + 1; $mask = col_or($mask, _qPredicate($df, $toks, $pos)); }
        else { throw "arctic.query: expected 'and' or 'or'"; }
    }
    return $mask;
}
