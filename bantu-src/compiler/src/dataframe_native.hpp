#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  dataframe_native.hpp — native columnar primitives for Bantu (the `arctic`
//  data-science foundations).
//
//  WHY THIS EXISTS
//  ---------------
//  A DataFrame is fast because a *column* is a contiguous typed buffer, not a
//  list of boxed values. Bantu lists are `vector<Value>` (pointer-chased, ~24-32
//  bytes per number), so vectorized data work is impossible in pure Bantu at any
//  useful speed. This header adds a native `Column` (typed buffer + null mask)
//  and the conversion glue; the `col_*` builtins in evaluator.hpp expose it, and
//  the pure-Bantu `arctic` library is built on top. Same split as the crypto
//  suite: native atoms, library written in Bantu.
//
//  LIFETIME
//  --------
//  A Column is held by a `std::shared_ptr` inside a Bantu `NATIVE_HANDLE` Value
//  (see types.hpp), so C++ RAII frees it when the last Bantu reference drops —
//  no manual free, no leak, even across long query chains.
//
//  DTYPES
//  ------
//  f64 (double), i64 (true 64-bit integer — exact beyond 2^53 while it stays in
//  the engine; materializing to a Bantu number is float64, documented), bool,
//  utf8 (string). Every column carries a per-element null mask (1 = present).
//
//  This file owns all column logic in one place; it is Value-aware (includes
//  types.hpp) because converting Bantu lists <-> columns is column-specific.
//  Functions throw std::runtime_error with a plain-language message; the builtin
//  wrappers translate that into a Bantu error.
// ════════════════════════════════════════════════════════════════════════════

#include "types.hpp"
#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <charconv>
#include <cstdio>

namespace arctic {

// The tag stored in a NATIVE_HANDLE Value for columns.
static const char* COLUMN_TAG = "column";

enum class DType { F64, I64, BOOL, UTF8 };

inline std::string dtypeName(DType d) {
    switch (d) {
        case DType::F64:  return "f64";
        case DType::I64:  return "i64";
        case DType::BOOL: return "bool";
        case DType::UTF8: return "utf8";
    }
    return "?";
}

inline DType dtypeFromName(const std::string& s) {
    if (s == "f64")  return DType::F64;
    if (s == "i64")  return DType::I64;
    if (s == "bool") return DType::BOOL;
    if (s == "utf8" || s == "str" || s == "string") return DType::UTF8;
    throw std::runtime_error("unknown dtype '" + s + "' (use f64, i64, bool, or utf8)");
}

// A "logical" type layered over the physical storage (DECISIONS: keeps every
// numeric kernel working unchanged on a production interpreter). DATETIME/DATE
// ride on the i64 buffer (epoch ms / days-since-epoch, UTC); CAT rides on the
// i64 buffer as dictionary codes with the strings in `cats`.
enum class Logical { NONE, DATE, DATETIME, CAT };

// A typed column. Only the buffer matching `dtype` is populated; `valid[i]==0`
// marks element i as null (the buffer still holds a harmless default there).
struct Column {
    DType dtype = DType::F64;
    size_t n = 0;
    std::vector<double>      f64;
    std::vector<int64_t>     i64;
    std::vector<uint8_t>     b;     // 0/1
    std::vector<std::string> s;
    std::vector<uint8_t>     valid; // 1 = present, 0 = null
    Logical logical = Logical::NONE; // semantic overlay (see above)
    std::vector<std::string> cats;   // CAT dictionary: code -> category string
};
using ColumnPtr = std::shared_ptr<Column>;

// The user-facing type name: the logical name when set, else the physical dtype.
inline std::string columnTypeName(const Column& c) {
    switch (c.logical) {
        case Logical::DATE:     return "date";
        case Logical::DATETIME: return "datetime";
        case Logical::CAT:      return "cat";
        default:                return dtypeName(c.dtype);
    }
}
// Copy the semantic overlay from src to dst (used by value-preserving ops like
// slice/filter/take/head/tail and group-key reconstruction).
inline void carryMeta(Column& dst, const Column& src) { dst.logical = src.logical; dst.cats = src.cats; }

// ── Calendar math (Howard Hinnant's public-domain civil algorithms) ──────────
// Fast, locale-free, no struct tm: correct for the full proleptic Gregorian range.
inline int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
inline void civilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
}
// 0=Sunday..6=Saturday for a days-since-epoch count.
inline unsigned weekdayFromDays(int64_t z) { return (unsigned)((z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6)); }

// Parse "YYYY-MM-DD", "YYYY-MM-DD[ T]HH:MM:SS[.fff]" (optional trailing 'Z').
// Returns epoch milliseconds (UTC) on success; sets hasTime for datetime-vs-date.
inline bool parseIsoMs(const std::string& s, int64_t& outMs, bool& hasTime) {
    const char* p = s.c_str(); const char* e = p + s.size();
    auto num = [&](int digits, int& out) -> bool {
        out = 0; if (p + digits > e) return false;
        for (int k = 0; k < digits; k++) { char c = p[k]; if (c < '0' || c > '9') return false; out = out * 10 + (c - '0'); }
        p += digits; return true;
    };
    int y, mo, da, h = 0, mi = 0, se = 0, ms = 0; hasTime = false;
    if (!num(4, y)) return false;
    if (p >= e || *p++ != '-') return false;
    if (!num(2, mo)) return false;
    if (p >= e || *p++ != '-') return false;
    if (!num(2, da)) return false;
    if (mo < 1 || mo > 12 || da < 1 || da > 31) return false;
    if (p < e && (*p == ' ' || *p == 'T')) {
        p++; hasTime = true;
        if (!num(2, h)) return false;
        if (p >= e || *p++ != ':') return false;
        if (!num(2, mi)) return false;
        if (p < e && *p == ':') { p++; if (!num(2, se)) return false; }
        if (p < e && *p == '.') { p++; int mult = 100; while (p < e && *p >= '0' && *p <= '9') { ms += (*p - '0') * mult; mult /= 10; p++; } }
        if (h > 23 || mi > 59 || se > 60) return false;
    }
    if (p < e && *p == 'Z') p++;
    if (p != e) return false;   // trailing junk → not a clean timestamp
    int64_t days = daysFromCivil(y, (unsigned)mo, (unsigned)da);
    outMs = days * 86400000LL + (int64_t)h * 3600000LL + (int64_t)mi * 60000LL + (int64_t)se * 1000LL + ms;
    return true;
}

// Format epoch ms as ISO-8601 ("YYYY-MM-DD" for a date, "YYYY-MM-DD HH:MM:SS" for
// a datetime; a non-zero ms fraction is appended as ".fff").
inline std::string isoFromMs(int64_t ms, bool dateOnly) {
    int64_t days = ms / 86400000LL; int64_t rem = ms % 86400000LL;
    if (rem < 0) { rem += 86400000LL; days -= 1; }
    int64_t y; unsigned mo, da; civilFromDays(days, y, mo, da);
    char buf[40];
    if (dateOnly) { std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u", (long long)y, mo, da); return buf; }
    int h = (int)(rem / 3600000); rem %= 3600000;
    int mi = (int)(rem / 60000); rem %= 60000;
    int se = (int)(rem / 1000); int frac = (int)(rem % 1000);
    if (frac) std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u %02d:%02d:%02d.%03d", (long long)y, mo, da, h, mi, se, frac);
    else      std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u %02d:%02d:%02d", (long long)y, mo, da, h, mi, se);
    return buf;
}

// ── Value <-> Column glue ─────────────────────────────────────────────────────

// Extract a Column from a Bantu value, or throw a friendly error.
inline ColumnPtr asColumn(const Value& v) {
    if (!v.isNativeHandle() || v.handleTag() != COLUMN_TAG || !v.handle) {
        throw std::runtime_error("expected a column (got " +
            (v.isNativeHandle() ? ("<" + v.handleTag() + ">") : v.toString()) + ")");
    }
    return std::static_pointer_cast<Column>(v.handle);
}
inline bool isColumn(const Value& v) {
    return v.isNativeHandle() && v.handleTag() == COLUMN_TAG && v.handle;
}
// Wrap a Column as a Bantu handle Value.
inline Value wrap(ColumnPtr c) { return Value(std::static_pointer_cast<void>(c), COLUMN_TAG); }

// One element of a column as a Bantu Value (null → Bantu null).
inline Value elemToValue(const Column& c, size_t i) {
    if (i >= c.n) throw std::runtime_error("column index out of range");
    if (!c.valid[i]) return Value();  // null
    // Semantic overlays render as readable values (ISO strings / category text).
    if (c.logical == Logical::DATETIME) return Value(isoFromMs(c.i64[i], false));
    if (c.logical == Logical::DATE)     return Value(isoFromMs(c.i64[i] * 86400000LL, true));
    if (c.logical == Logical::CAT) {
        int64_t code = c.i64[i];
        return (code >= 0 && (size_t)code < c.cats.size()) ? Value(c.cats[code]) : Value();
    }
    switch (c.dtype) {
        case DType::F64:  return Value((double)c.f64[i]);
        case DType::I64:  return Value((double)c.i64[i]);   // materialize (2^53 caveat)
        case DType::BOOL: return Value((bool)(c.b[i] != 0));
        case DType::UTF8: return Value(c.s[i]);
    }
    return Value();
}

// Build a column of `dtype` from a Bantu list. Bantu null elements become nulls.
inline ColumnPtr makeColumn(const std::vector<Value>& items, DType dtype) {
    auto c = std::make_shared<Column>();
    c->dtype = dtype;
    c->n = items.size();
    c->valid.assign(c->n, 1);
    switch (dtype) {
        case DType::F64:  c->f64.resize(c->n); break;
        case DType::I64:  c->i64.resize(c->n); break;
        case DType::BOOL: c->b.resize(c->n);   break;
        case DType::UTF8: c->s.resize(c->n);   break;
    }
    for (size_t i = 0; i < c->n; i++) {
        const Value& v = items[i];
        if (v.isNull()) { c->valid[i] = 0; continue; }
        switch (dtype) {
            case DType::F64:
                if (v.isNumber())      c->f64[i] = v.numberVal;
                else if (v.isBool())   c->f64[i] = v.boolVal ? 1.0 : 0.0;
                else throw std::runtime_error("col(f64): element " + std::to_string(i) +
                                              " is not a number");
                break;
            case DType::I64:
                if (v.isNumber())      c->i64[i] = (int64_t)std::llround(v.numberVal);
                else if (v.isBool())   c->i64[i] = v.boolVal ? 1 : 0;
                else throw std::runtime_error("col(i64): element " + std::to_string(i) +
                                              " is not a number");
                break;
            case DType::BOOL:
                c->b[i] = v.isTruthy() ? 1 : 0;
                break;
            case DType::UTF8:
                c->s[i] = v.isString() ? v.stringVal : v.toString();
                break;
        }
    }
    return c;
}

// Whole column → a Bantu list value.
inline Value columnToList(const Column& c) {
    std::vector<Value> out;
    out.reserve(c.n);
    for (size_t i = 0; i < c.n; i++) out.push_back(elemToValue(c, i));
    return Value(std::move(out));
}

// A contiguous slice [start, start+len) as a new column (len clamped to bounds).
inline ColumnPtr sliceColumn(const Column& c, size_t start, size_t len) {
    if (start > c.n) start = c.n;
    if (start + len > c.n) len = c.n - start;
    auto o = std::make_shared<Column>();
    o->dtype = c.dtype;
    o->n = len;
    o->valid.assign(c.valid.begin() + start, c.valid.begin() + start + len);
    switch (c.dtype) {
        case DType::F64:  o->f64.assign(c.f64.begin() + start, c.f64.begin() + start + len); break;
        case DType::I64:  o->i64.assign(c.i64.begin() + start, c.i64.begin() + start + len); break;
        case DType::BOOL: o->b.assign(c.b.begin() + start, c.b.begin() + start + len);       break;
        case DType::UTF8: o->s.assign(c.s.begin() + start, c.s.begin() + start + len);       break;
    }
    carryMeta(*o, c);
    return o;
}

// Materialize a categorical column to a plain utf8 column (code -> category text).
inline ColumnPtr catToUtf8(const Column& c) {
    auto o = std::make_shared<Column>(); o->dtype = DType::UTF8; o->n = c.n; o->valid = c.valid; o->s.resize(c.n);
    for (size_t i = 0; i < c.n; i++) {
        if (!c.valid[i]) continue;
        int64_t code = c.i64[i];
        o->s[i] = (code >= 0 && (size_t)code < c.cats.size()) ? c.cats[code] : "";
    }
    return o;
}

// Cast a column to another dtype (nulls preserved; numeric<->string as sensible).
inline ColumnPtr castColumn(const Column& c, DType to) {
    auto o = std::make_shared<Column>();
    o->dtype = to; o->n = c.n; o->valid = c.valid;
    switch (to) {
        case DType::F64:  o->f64.resize(c.n); break;
        case DType::I64:  o->i64.resize(c.n); break;
        case DType::BOOL: o->b.resize(c.n);   break;
        case DType::UTF8: o->s.resize(c.n);   break;
    }
    for (size_t i = 0; i < c.n; i++) {
        if (!c.valid[i]) continue;
        // read source as a double / string as needed
        double dv = 0; std::string sv; bool haveNum = true;
        switch (c.dtype) {
            case DType::F64:  dv = c.f64[i]; break;
            case DType::I64:  dv = (double)c.i64[i]; break;
            case DType::BOOL: dv = c.b[i] ? 1.0 : 0.0; break;
            case DType::UTF8: sv = c.s[i]; haveNum = false; break;
        }
        switch (to) {
            case DType::F64:
                if (haveNum) o->f64[i] = dv;
                else { try { o->f64[i] = std::stod(sv); } catch (...) { o->valid[i] = 0; } }
                break;
            case DType::I64:
                if (haveNum) o->i64[i] = (int64_t)std::llround(dv);
                else { try { o->i64[i] = (int64_t)std::llround(std::stod(sv)); } catch (...) { o->valid[i] = 0; } }
                break;
            case DType::BOOL:
                o->b[i] = haveNum ? (dv != 0 ? 1 : 0) : (sv.empty() ? 0 : 1);
                break;
            case DType::UTF8:
                if (haveNum) {
                    if (c.dtype == DType::I64) o->s[i] = std::to_string(c.i64[i]);
                    else if (c.dtype == DType::BOOL) o->s[i] = c.b[i] ? "true" : "false";
                    else { std::ostringstream ss; ss << dv; o->s[i] = ss.str(); }
                } else o->s[i] = sv;
                break;
        }
    }
    return o;
}

// Boolean column marking null positions (1 = null).
inline ColumnPtr isNullMask(const Column& c) {
    auto o = std::make_shared<Column>();
    o->dtype = DType::BOOL; o->n = c.n; o->valid.assign(c.n, 1); o->b.resize(c.n);
    for (size_t i = 0; i < c.n; i++) o->b[i] = c.valid[i] ? 0 : 1;
    return o;
}

inline size_t nullCount(const Column& c) {
    size_t k = 0;
    for (size_t i = 0; i < c.n; i++) if (!c.valid[i]) k++;
    return k;
}

// Replace nulls with a fill value (a Bantu Value coerced to the column dtype).
inline ColumnPtr fillNull(const Column& c, const Value& fill) {
    auto o = std::make_shared<Column>(c);   // copy
    for (size_t i = 0; i < o->n; i++) {
        if (o->valid[i]) continue;
        o->valid[i] = 1;
        switch (o->dtype) {
            case DType::F64:  o->f64[i] = fill.isNumber() ? fill.numberVal : 0.0; break;
            case DType::I64:  o->i64[i] = fill.isNumber() ? (int64_t)std::llround(fill.numberVal) : 0; break;
            case DType::BOOL: o->b[i]   = fill.isTruthy() ? 1 : 0; break;
            case DType::UTF8: o->s[i]   = fill.isString() ? fill.stringVal : fill.toString(); break;
        }
    }
    return o;
}

// ════════════════════════════════════════════════════════════════════════════
//  DATETIME / DATE / CATEGORICAL  (logical overlays on the i64 buffer)
// ════════════════════════════════════════════════════════════════════════════

// col_to_datetime(c[, asDate]): utf8 → parsed epoch; numeric → taken as epoch
// (ms for datetime, days for date). Unparseable / null cells become null.
inline ColumnPtr toDatetime(const Column& c, bool asDate) {
    auto o = std::make_shared<Column>(); o->dtype = DType::I64; o->n = c.n; o->valid.assign(c.n, 1); o->i64.resize(c.n);
    o->logical = asDate ? Logical::DATE : Logical::DATETIME;
    for (size_t i = 0; i < c.n; i++) {
        if (!c.valid[i]) { o->valid[i] = 0; continue; }
        if (c.dtype == DType::UTF8) {
            int64_t ms; bool ht;
            if (!parseIsoMs(c.s[i], ms, ht)) { o->valid[i] = 0; continue; }
            o->i64[i] = asDate ? (ms / 86400000LL) : ms;
        } else if (c.dtype == DType::I64) {
            o->i64[i] = c.i64[i];
        } else if (c.dtype == DType::F64) {
            o->i64[i] = (int64_t)std::llround(c.f64[i]);
        } else { o->valid[i] = 0; }
    }
    return o;
}

// Break an element into calendar parts (works for DATE and DATETIME).
inline void dtParts(const Column& c, size_t i, int64_t& y, unsigned& mo, unsigned& da,
                    int& h, int& mi, int& se, unsigned& wd) {
    int64_t ms = (c.logical == Logical::DATE) ? c.i64[i] * 86400000LL : c.i64[i];
    int64_t days = ms / 86400000LL; int64_t rem = ms % 86400000LL;
    if (rem < 0) { rem += 86400000LL; days -= 1; }
    civilFromDays(days, y, mo, da); wd = weekdayFromDays(days);
    h = (int)(rem / 3600000); rem %= 3600000; mi = (int)(rem / 60000); rem %= 60000; se = (int)(rem / 1000);
}

enum class DtPart { YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, WEEKDAY };

// col_dt_*(c) → i64 column of a calendar component.
inline ColumnPtr dtComponent(const Column& c, DtPart part) {
    if (c.logical != Logical::DATE && c.logical != Logical::DATETIME)
        throw std::runtime_error("expected a datetime/date column (use col_to_datetime first)");
    auto o = std::make_shared<Column>(); o->dtype = DType::I64; o->n = c.n; o->valid = c.valid; o->i64.resize(c.n);
    for (size_t i = 0; i < c.n; i++) {
        if (!c.valid[i]) continue;
        int64_t y; unsigned mo, da, wd; int h, mi, se; dtParts(c, i, y, mo, da, h, mi, se, wd);
        switch (part) {
            case DtPart::YEAR:    o->i64[i] = y; break;
            case DtPart::MONTH:   o->i64[i] = mo; break;
            case DtPart::DAY:     o->i64[i] = da; break;
            case DtPart::HOUR:    o->i64[i] = h; break;
            case DtPart::MINUTE:  o->i64[i] = mi; break;
            case DtPart::SECOND:  o->i64[i] = se; break;
            case DtPart::WEEKDAY: o->i64[i] = wd; break;   // 0=Sunday
        }
    }
    return o;
}

// col_strftime(c, fmt) → utf8. Supports %Y %y %m %d %H %M %S %j %% (locale-free).
inline ColumnPtr strftimeCol(const Column& c, const std::string& fmt) {
    if (c.logical != Logical::DATE && c.logical != Logical::DATETIME)
        throw std::runtime_error("col_strftime expects a datetime/date column");
    auto o = std::make_shared<Column>(); o->dtype = DType::UTF8; o->n = c.n; o->valid = c.valid; o->s.resize(c.n);
    char buf[16];
    for (size_t i = 0; i < c.n; i++) {
        if (!c.valid[i]) continue;
        int64_t y; unsigned mo, da, wd; int h, mi, se; dtParts(c, i, y, mo, da, h, mi, se, wd);
        std::string out;
        for (size_t k = 0; k < fmt.size(); k++) {
            if (fmt[k] == '%' && k + 1 < fmt.size()) {
                char f = fmt[++k];
                switch (f) {
                    case 'Y': std::snprintf(buf, sizeof(buf), "%04lld", (long long)y); out += buf; break;
                    case 'y': std::snprintf(buf, sizeof(buf), "%02lld", (long long)(((y % 100) + 100) % 100)); out += buf; break;
                    case 'm': std::snprintf(buf, sizeof(buf), "%02u", mo); out += buf; break;
                    case 'd': std::snprintf(buf, sizeof(buf), "%02u", da); out += buf; break;
                    case 'H': std::snprintf(buf, sizeof(buf), "%02d", h); out += buf; break;
                    case 'M': std::snprintf(buf, sizeof(buf), "%02d", mi); out += buf; break;
                    case 'S': std::snprintf(buf, sizeof(buf), "%02d", se); out += buf; break;
                    case 'j': { int64_t doy = daysFromCivil(y, mo, da) - daysFromCivil(y, 1, 1) + 1;
                                std::snprintf(buf, sizeof(buf), "%03lld", (long long)doy); out += buf; break; }
                    case '%': out.push_back('%'); break;
                    default:  out.push_back('%'); out.push_back(f); break;
                }
            } else out.push_back(fmt[k]);
        }
        o->s[i] = out;
    }
    return o;
}

// col_to_categorical(c): any column → dictionary-encoded CAT (codes + cats).
// Values are keyed by their string rendering (so utf8/number/bool all work).
inline ColumnPtr toCategorical(const Column& c) {
    auto o = std::make_shared<Column>(); o->dtype = DType::I64; o->n = c.n; o->valid.assign(c.n, 1);
    o->i64.resize(c.n); o->logical = Logical::CAT;
    std::unordered_map<std::string, int64_t> dict; dict.reserve(c.n / 4 + 8);
    for (size_t i = 0; i < c.n; i++) {
        if (!c.valid[i]) { o->valid[i] = 0; continue; }
        Value v = elemToValue(c, i);
        std::string key = v.isString() ? v.stringVal : v.toString();
        auto it = dict.find(key);
        int64_t code;
        if (it == dict.end()) { code = (int64_t)o->cats.size(); o->cats.push_back(key); dict.emplace(std::move(key), code); }
        else code = it->second;
        o->i64[i] = code;
    }
    return o;
}

// col_categories(c) → utf8 column of the category strings (index = code).
inline ColumnPtr categoriesOf(const Column& c) {
    if (c.logical != Logical::CAT) throw std::runtime_error("col_categories expects a categorical column");
    auto o = std::make_shared<Column>(); o->dtype = DType::UTF8; o->n = c.cats.size();
    o->valid.assign(o->n, 1); o->s = c.cats;
    return o;
}

// col_codes(c) → i64 column of the raw dictionary codes (null preserved).
inline ColumnPtr codesOf(const Column& c) {
    if (c.logical != Logical::CAT) throw std::runtime_error("col_codes expects a categorical column");
    auto o = std::make_shared<Column>(); o->dtype = DType::I64; o->n = c.n; o->valid = c.valid; o->i64 = c.i64;
    return o;
}

// ════════════════════════════════════════════════════════════════════════════
//  KERNELS (Phase 3) — vectorized compute over contiguous buffers.
//  Design (DECISIONS A8): single-pass O(n) loops on the typed buffers (no
//  per-element Value boxing); integer-exact path for i64⊕i64 +,-,*; Welford
//  variance/std; nth_element median; unordered_map hash groupby/join. Every
//  operand may be a Column OR a plain scalar (broadcast) so the Bantu surface
//  stays simple: col_mul($price, 1.1), col_gt($age, 18) both "just work".
// ════════════════════════════════════════════════════════════════════════════

// ── Operand handling (a column or a broadcast scalar) ─────────────────────────
struct NumOperand {
    bool isCol = false;
    ColumnPtr col;
    bool allInt = false;    // integer-typed (i64 col, bool col, or integral scalar)
    // scalar cache:
    double sd = 0; int64_t si = 0; bool snull = false;
    size_t len = 1;         // 1 = scalar (broadcast)
};

inline NumOperand numOperand(const Value& v) {
    NumOperand o;
    if (isColumn(v)) {
        o.isCol = true; o.col = asColumn(v); o.len = o.col->n;
        if (o.col->dtype == DType::UTF8)
            throw std::runtime_error("expected a numeric column (got utf8)");
        o.allInt = (o.col->dtype == DType::I64 || o.col->dtype == DType::BOOL);
    } else if (v.isNull()) {
        o.snull = true;
    } else if (v.isNumber()) {
        o.sd = v.numberVal; o.si = (int64_t)std::llround(v.numberVal);
        o.allInt = (std::floor(v.numberVal) == v.numberVal && !std::isinf(v.numberVal));
    } else if (v.isBool()) {
        o.sd = v.boolVal ? 1.0 : 0.0; o.si = v.boolVal ? 1 : 0; o.allInt = true;
    } else {
        throw std::runtime_error("arithmetic operand must be numeric");
    }
    return o;
}

// Read element i of an operand as double; sets isnull.
inline double readD(const NumOperand& o, size_t i, bool& isnull) {
    if (!o.isCol) { isnull = o.snull; return o.sd; }
    const Column& c = *o.col;
    if (!c.valid[i]) { isnull = true; return 0; }
    isnull = false;
    switch (c.dtype) {
        case DType::F64:  return c.f64[i];
        case DType::I64:  return (double)c.i64[i];
        case DType::BOOL: return c.b[i] ? 1.0 : 0.0;
        default:          return 0;
    }
}
inline int64_t readI(const NumOperand& o, size_t i, bool& isnull) {
    if (!o.isCol) { isnull = o.snull; return o.si; }
    const Column& c = *o.col;
    if (!c.valid[i]) { isnull = true; return 0; }
    isnull = false;
    switch (c.dtype) {
        case DType::I64:  return c.i64[i];
        case DType::BOOL: return c.b[i] ? 1 : 0;
        case DType::F64:  return (int64_t)std::llround(c.f64[i]);
        default:          return 0;
    }
}

// Result length for two operands (columns must match; scalars broadcast).
inline size_t resultLen(const NumOperand& a, const NumOperand& b) {
    if (a.isCol && b.isCol) {
        if (a.col->n != b.col->n)
            throw std::runtime_error("length mismatch: " + std::to_string(a.col->n) +
                                     " vs " + std::to_string(b.col->n));
        return a.col->n;
    }
    if (a.isCol) return a.col->n;
    if (b.isCol) return b.col->n;
    return 1;
}

enum class Arith { ADD, SUB, MUL, DIV, MOD, POW };

// Elementwise arithmetic. i64-exact for +,-,* when both operands are integer;
// otherwise (and always for / and pow) computed in f64. Null if either is null.
inline ColumnPtr arithOp(const Value& A, const Value& B, Arith op) {
    NumOperand a = numOperand(A), b = numOperand(B);
    size_t n = resultLen(a, b);
    bool intPath = a.allInt && b.allInt && (op == Arith::ADD || op == Arith::SUB || op == Arith::MOD || op == Arith::MUL);
    auto o = std::make_shared<Column>();
    o->n = n; o->valid.assign(n, 1);
    if (intPath) {
        o->dtype = DType::I64; o->i64.resize(n);
        for (size_t i = 0; i < n; i++) {
            bool na, nb; int64_t x = readI(a, i, na), y = readI(b, i, nb);
            if (na || nb) { o->valid[i] = 0; continue; }
            switch (op) {
                case Arith::ADD: o->i64[i] = x + y; break;
                case Arith::SUB: o->i64[i] = x - y; break;
                case Arith::MUL: o->i64[i] = x * y; break;
                case Arith::MOD: if (y == 0) { o->valid[i] = 0; } else o->i64[i] = x % y; break;
                default: break;
            }
        }
    } else {
        o->dtype = DType::F64; o->f64.resize(n);
        for (size_t i = 0; i < n; i++) {
            bool na, nb; double x = readD(a, i, na), y = readD(b, i, nb);
            if (na || nb) { o->valid[i] = 0; continue; }
            switch (op) {
                case Arith::ADD: o->f64[i] = x + y; break;
                case Arith::SUB: o->f64[i] = x - y; break;
                case Arith::MUL: o->f64[i] = x * y; break;
                case Arith::DIV: o->f64[i] = x / y; break;   // x/0 -> inf/nan (IEEE)
                case Arith::MOD: o->f64[i] = std::fmod(x, y); break;
                case Arith::POW: o->f64[i] = std::pow(x, y); break;
            }
        }
    }
    return o;
}

// Unary numeric: negate / abs.
inline ColumnPtr unaryOp(const Value& A, bool absolute) {
    NumOperand a = numOperand(A);
    if (!a.isCol) throw std::runtime_error("expected a column");
    const Column& c = *a.col;
    auto o = std::make_shared<Column>(); o->n = c.n; o->valid = c.valid;
    if (c.dtype == DType::I64 || c.dtype == DType::BOOL) {
        o->dtype = DType::I64; o->i64.resize(c.n);
        for (size_t i = 0; i < c.n; i++) { int64_t v = (c.dtype==DType::I64)?c.i64[i]:(c.b[i]?1:0);
            o->i64[i] = absolute ? (v < 0 ? -v : v) : -v; }
    } else {
        o->dtype = DType::F64; o->f64.resize(c.n);
        for (size_t i = 0; i < c.n; i++) o->f64[i] = absolute ? std::fabs(c.f64[i]) : -c.f64[i];
    }
    return o;
}

enum class Cmp { GT, GE, LT, LE, EQ, NE };

// Elementwise comparison → bool column (null where either side is null).
// utf8 columns compare lexicographically; otherwise numeric.
inline ColumnPtr compareOp(const Value& Ain, const Value& Bin, Cmp op) {
    // Normalize semantic overlays so comparisons "just work":
    //  • a categorical column compares as its category strings;
    //  • a datetime/date column compared to an ISO string parses the string to
    //    the same epoch unit, then compares numerically.
    auto normalize = [](const Value& X, const Value& other) -> Value {
        if (isColumn(X)) {
            ColumnPtr c = asColumn(X);
            if (c->logical == Logical::CAT) return wrap(catToUtf8(*c));
            return X;   // datetime/date stay numeric (i64) → numeric path below
        }
        if (X.isString() && isColumn(other)) {
            ColumnPtr oc = asColumn(other);
            if (oc->logical == Logical::DATETIME || oc->logical == Logical::DATE) {
                int64_t ms; bool ht;
                if (parseIsoMs(X.stringVal, ms, ht))
                    return Value((double)(oc->logical == Logical::DATE ? ms / 86400000LL : ms));
            }
        }
        return X;
    };
    Value A = normalize(Ain, Bin), B = normalize(Bin, Ain);
    // string comparison path when either operand is a utf8 column / string scalar
    bool aStrCol = isColumn(A) && asColumn(A)->dtype == DType::UTF8;
    bool bStrCol = isColumn(B) && asColumn(B)->dtype == DType::UTF8;
    bool aStrScalar = !isColumn(A) && A.isString();
    bool bStrScalar = !isColumn(B) && B.isString();
    if (aStrCol || bStrCol || aStrScalar || bStrScalar) {
        size_t n = 1;
        if (aStrCol) n = asColumn(A)->n;
        if (bStrCol) n = bStrCol && aStrCol ? std::max(n, asColumn(B)->n) : (bStrCol ? asColumn(B)->n : n);
        if (aStrCol && bStrCol && asColumn(A)->n != asColumn(B)->n)
            throw std::runtime_error("length mismatch in comparison");
        ColumnPtr ca = aStrCol ? asColumn(A) : nullptr, cb = bStrCol ? asColumn(B) : nullptr;
        std::string sa = aStrScalar ? A.stringVal : "", sb = bStrScalar ? B.stringVal : "";
        bool saNull = !isColumn(A) && A.isNull(), sbNull = !isColumn(B) && B.isNull();
        auto o = std::make_shared<Column>(); o->dtype = DType::BOOL; o->n = n; o->valid.assign(n,1); o->b.resize(n);
        for (size_t i = 0; i < n; i++) {
            bool na = ca ? !ca->valid[i] : saNull;
            bool nb = cb ? !cb->valid[i] : sbNull;
            if (na || nb) { o->valid[i] = 0; continue; }
            const std::string& x = ca ? ca->s[i] : sa;
            const std::string& y = cb ? cb->s[i] : sb;
            int c = x.compare(y);
            bool r = false;
            switch (op) { case Cmp::GT: r=c>0; break; case Cmp::GE: r=c>=0; break;
                          case Cmp::LT: r=c<0; break; case Cmp::LE: r=c<=0; break;
                          case Cmp::EQ: r=c==0; break; case Cmp::NE: r=c!=0; break; }
            o->b[i] = r ? 1 : 0;
        }
        return o;
    }
    NumOperand a = numOperand(A), b = numOperand(B);
    size_t n = resultLen(a, b);
    auto o = std::make_shared<Column>(); o->dtype = DType::BOOL; o->n = n; o->valid.assign(n,1); o->b.resize(n);
    for (size_t i = 0; i < n; i++) {
        bool na, nb; double x = readD(a,i,na), y = readD(b,i,nb);
        if (na || nb) { o->valid[i] = 0; continue; }
        bool r = false;
        switch (op) { case Cmp::GT: r=x>y; break; case Cmp::GE: r=x>=y; break;
                      case Cmp::LT: r=x<y; break; case Cmp::LE: r=x<=y; break;
                      case Cmp::EQ: r=x==y; break; case Cmp::NE: r=x!=y; break; }
        o->b[i] = r ? 1 : 0;
    }
    return o;
}

// Mask logic on bool columns (null if either null; not() keeps null).
inline ColumnPtr maskBin(const Value& A, const Value& B, bool isAnd) {
    ColumnPtr a = asColumn(A), b = asColumn(B);
    if (a->dtype != DType::BOOL || b->dtype != DType::BOOL)
        throw std::runtime_error("col_and/col_or need boolean columns");
    if (a->n != b->n) throw std::runtime_error("length mismatch");
    auto o = std::make_shared<Column>(); o->dtype = DType::BOOL; o->n = a->n; o->valid.assign(a->n,1); o->b.resize(a->n);
    for (size_t i = 0; i < a->n; i++) {
        if (!a->valid[i] || !b->valid[i]) { o->valid[i] = 0; continue; }
        o->b[i] = isAnd ? (a->b[i] && b->b[i]) : (a->b[i] || b->b[i]);
    }
    return o;
}
inline ColumnPtr maskNot(const Value& A) {
    ColumnPtr a = asColumn(A);
    if (a->dtype != DType::BOOL) throw std::runtime_error("col_not needs a boolean column");
    auto o = std::make_shared<Column>(); o->dtype = DType::BOOL; o->n = a->n; o->valid = a->valid; o->b.resize(a->n);
    for (size_t i = 0; i < a->n; i++) o->b[i] = a->valid[i] ? (a->b[i] ? 0 : 1) : 0;
    return o;
}

// The common dtype for a set of value-operands (for where/case results).
inline DType commonDType(const std::vector<Value>& vals) {
    bool anyStr=false, anyF64=false, anyInt=false, anyBool=false;
    for (auto& v : vals) {
        if (isColumn(v)) { DType d = asColumn(v)->dtype;
            if (d==DType::UTF8) anyStr=true; else if (d==DType::F64) anyF64=true;
            else if (d==DType::I64) anyInt=true; else anyBool=true;
        } else if (v.isString()) anyStr=true;
        else if (v.isNull()) {}
        else if (v.isBool()) anyBool=true;
        else if (v.isNumber()) { if (std::floor(v.numberVal)==v.numberVal) anyInt=true; else anyF64=true; }
    }
    if (anyStr) return DType::UTF8;
    if (anyF64) return DType::F64;
    if (anyInt) return DType::I64;
    if (anyBool) return DType::BOOL;
    return DType::F64;
}

// Set element i of an out column from a value-operand (column or scalar) row i.
inline void setFrom(Column& out, size_t i, const Value& src, size_t srcRow) {
    // returns without marking valid if source is null
    auto put = [&](bool isnull, double d, int64_t iv, bool bv, const std::string& s) {
        if (isnull) { out.valid[i] = 0; return; }
        out.valid[i] = 1;
        switch (out.dtype) {
            case DType::F64:  out.f64[i] = d; break;
            case DType::I64:  out.i64[i] = iv; break;
            case DType::BOOL: out.b[i] = bv ? 1 : 0; break;
            case DType::UTF8: out.s[i] = s; break;
        }
    };
    if (isColumn(src)) {
        const Column& c = *asColumn(src);
        if (!c.valid[srcRow]) { put(true,0,0,false,""); return; }
        switch (c.dtype) {
            case DType::F64:  put(false, c.f64[srcRow], (int64_t)std::llround(c.f64[srcRow]), c.f64[srcRow]!=0, std::to_string(c.f64[srcRow])); break;
            case DType::I64:  put(false, (double)c.i64[srcRow], c.i64[srcRow], c.i64[srcRow]!=0, std::to_string(c.i64[srcRow])); break;
            case DType::BOOL: put(false, c.b[srcRow]?1:0, c.b[srcRow]?1:0, c.b[srcRow]!=0, c.b[srcRow]?"true":"false"); break;
            case DType::UTF8: put(false, 0, 0, !c.s[srcRow].empty(), c.s[srcRow]); break;
        }
    } else {
        if (src.isNull()) { put(true,0,0,false,""); return; }
        if (src.isNumber()) put(false, src.numberVal, (int64_t)std::llround(src.numberVal), src.numberVal!=0, src.toString());
        else if (src.isBool()) put(false, src.boolVal?1:0, src.boolVal?1:0, src.boolVal, src.boolVal?"true":"false");
        else put(false, 0, 0, !src.stringVal.empty(), src.stringVal);
    }
}
inline size_t operandRows(const Value& v) { return isColumn(v) ? asColumn(v)->n : 1; }
inline size_t srcRowOf(const Value& v, size_t i) { return isColumn(v) ? i : 0; }

// col_where(mask, a, b): elementwise if/else. a,b are columns or scalars.
inline ColumnPtr whereOp(const Value& M, const Value& A, const Value& B) {
    ColumnPtr m = asColumn(M);
    if (m->dtype != DType::BOOL) throw std::runtime_error("col_where: first argument must be a boolean mask");
    size_t n = m->n;
    DType rt = commonDType({A, B});
    auto o = std::make_shared<Column>(); o->dtype = rt; o->n = n; o->valid.assign(n,1);
    switch (rt) { case DType::F64:o->f64.resize(n);break; case DType::I64:o->i64.resize(n);break;
                  case DType::BOOL:o->b.resize(n);break; case DType::UTF8:o->s.resize(n);break; }
    for (size_t i = 0; i < n; i++) {
        if (!m->valid[i]) { o->valid[i] = 0; continue; }
        const Value& pick = m->b[i] ? A : B;
        setFrom(*o, i, pick, srcRowOf(pick, i));
    }
    return o;
}

// col_case([mask1,val1, mask2,val2, ...], default): first true mask wins.
inline ColumnPtr caseOp(const std::vector<Value>& pairs, const Value& def) {
    if (pairs.size() % 2 != 0) throw std::runtime_error("col_case: expected [mask, value, ...] pairs");
    size_t n = 0; bool haveN = false;
    for (size_t k = 0; k < pairs.size(); k += 2) {
        ColumnPtr m = asColumn(pairs[k]);
        if (m->dtype != DType::BOOL) throw std::runtime_error("col_case: condition must be a boolean column");
        if (!haveN) { n = m->n; haveN = true; }
        else if (m->n != n) throw std::runtime_error("col_case: masks differ in length");
    }
    if (!haveN) throw std::runtime_error("col_case: needs at least one [mask, value] pair");
    std::vector<Value> allVals;
    for (size_t k = 1; k < pairs.size(); k += 2) allVals.push_back(pairs[k]);
    allVals.push_back(def);
    DType rt = commonDType(allVals);
    auto o = std::make_shared<Column>(); o->dtype = rt; o->n = n; o->valid.assign(n,1);
    switch (rt) { case DType::F64:o->f64.resize(n);break; case DType::I64:o->i64.resize(n);break;
                  case DType::BOOL:o->b.resize(n);break; case DType::UTF8:o->s.resize(n);break; }
    for (size_t i = 0; i < n; i++) {
        bool matched = false;
        for (size_t k = 0; k < pairs.size() && !matched; k += 2) {
            const Column& m = *asColumn(pairs[k]);
            if (m.valid[i] && m.b[i]) { const Value& v = pairs[k+1]; setFrom(*o, i, v, srcRowOf(v,i)); matched = true; }
        }
        if (!matched) setFrom(*o, i, def, srcRowOf(def, i));
    }
    return o;
}

// col_filter(c, mask): keep elements where mask is true (and not null).
inline ColumnPtr filterOp(const Value& C, const Value& M) {
    ColumnPtr c = asColumn(C), m = asColumn(M);
    if (m->dtype != DType::BOOL) throw std::runtime_error("col_filter: mask must be boolean");
    if (m->n != c->n) throw std::runtime_error("col_filter: length mismatch");
    auto o = std::make_shared<Column>(); o->dtype = c->dtype;
    for (size_t i = 0; i < c->n; i++) {
        if (!m->valid[i] || !m->b[i]) continue;
        o->valid.push_back(c->valid[i]);
        switch (c->dtype) {
            case DType::F64:  o->f64.push_back(c->f64[i]); break;
            case DType::I64:  o->i64.push_back(c->i64[i]); break;
            case DType::BOOL: o->b.push_back(c->b[i]); break;
            case DType::UTF8: o->s.push_back(c->s[i]); break;
        }
    }
    o->n = o->valid.size();
    carryMeta(*o, *c);
    return o;
}

// col_take(c, idxCol): gather rows by an integer index column (nulls in idx → null).
inline ColumnPtr takeOp(const Value& C, const Value& Idx) {
    ColumnPtr c = asColumn(C), idx = asColumn(Idx);
    if (idx->dtype != DType::I64 && idx->dtype != DType::F64)
        throw std::runtime_error("col_take: index column must be numeric");
    size_t n = idx->n;
    auto o = std::make_shared<Column>(); o->dtype = c->dtype; o->n = n; o->valid.assign(n,1);
    switch (c->dtype) { case DType::F64:o->f64.resize(n);break; case DType::I64:o->i64.resize(n);break;
                        case DType::BOOL:o->b.resize(n);break; case DType::UTF8:o->s.resize(n);break; }
    for (size_t i = 0; i < n; i++) {
        if (!idx->valid[i]) { o->valid[i] = 0; continue; }
        long long j = (idx->dtype==DType::I64) ? (long long)idx->i64[i] : (long long)std::llround(idx->f64[i]);
        if (j < 0 || (size_t)j >= c->n) { o->valid[i] = 0; continue; }  // out-of-range → null
        if (!c->valid[j]) { o->valid[i] = 0; continue; }
        switch (c->dtype) {
            case DType::F64:  o->f64[i] = c->f64[j]; break;
            case DType::I64:  o->i64[i] = c->i64[j]; break;
            case DType::BOOL: o->b[i] = c->b[j]; break;
            case DType::UTF8: o->s[i] = c->s[j]; break;
        }
    }
    carryMeta(*o, *c);
    return o;
}

// ── Aggregations (→ a scalar Bantu Value; nulls skipped) ──────────────────────
enum class Agg { SUM, MEAN, MIN, MAX, STD, VAR, MEDIAN, COUNT, NUNIQUE, ANY, ALL };

inline Value aggOp(const Column& c, Agg op) {
    // COUNT = non-null count; NUNIQUE/ANY/ALL handled per-type.
    if (op == Agg::COUNT) {
        size_t k = 0; for (size_t i=0;i<c.n;i++) if (c.valid[i]) k++;
        return Value((double)k);
    }
    if (op == Agg::NUNIQUE) {
        if (c.dtype == DType::UTF8) {
            std::unordered_set<std::string> s;
            for (size_t i=0;i<c.n;i++) if (c.valid[i]) s.insert(c.s[i]);
            return Value((double)s.size());
        }
        std::unordered_set<double> s;
        for (size_t i=0;i<c.n;i++) if (c.valid[i]) {
            double d = (c.dtype==DType::F64)?c.f64[i]:(c.dtype==DType::I64?(double)c.i64[i]:(c.b[i]?1:0));
            s.insert(d);
        }
        return Value((double)s.size());
    }
    if (op == Agg::ANY || op == Agg::ALL) {
        bool anyTrue=false, allTrue=true, sawAny=false;
        for (size_t i=0;i<c.n;i++) if (c.valid[i]) {
            sawAny=true;
            bool t = (c.dtype==DType::BOOL)? (c.b[i]!=0)
                   : (c.dtype==DType::UTF8 ? !c.s[i].empty()
                   : (c.dtype==DType::I64 ? c.i64[i]!=0 : c.f64[i]!=0));
            anyTrue = anyTrue || t; allTrue = allTrue && t;
        }
        if (!sawAny) return (op==Agg::ANY)? Value(false) : Value(true);
        return (op==Agg::ANY)? Value(anyTrue) : Value(allTrue);
    }
    // numeric reductions
    if (c.dtype == DType::UTF8) throw std::runtime_error("numeric aggregation on a utf8 column");
    if (op == Agg::MEDIAN) {
        std::vector<double> v;
        for (size_t i=0;i<c.n;i++) if (c.valid[i])
            v.push_back(c.dtype==DType::F64?c.f64[i]:(c.dtype==DType::I64?(double)c.i64[i]:(c.b[i]?1:0)));
        if (v.empty()) return Value();
        size_t mid = v.size()/2;
        std::nth_element(v.begin(), v.begin()+mid, v.end());
        double hi = v[mid];
        if (v.size() % 2 == 1) return Value(hi);
        double lo = *std::max_element(v.begin(), v.begin()+mid);   // largest of lower half
        return Value((lo+hi)/2.0);
    }
    // sum/mean/min/max/var/std in one pass; Welford for var/std (sample, ddof=1)
    double sum=0, mean=0, m2=0, mn=0, mx=0; size_t k=0; bool have=false;
    // exact integer sum when i64
    bool intSum = (c.dtype == DType::I64) && (op == Agg::SUM);
    long long isum = 0;
    for (size_t i=0;i<c.n;i++) {
        if (!c.valid[i]) continue;
        double x = (c.dtype==DType::F64)?c.f64[i]:(c.dtype==DType::I64?(double)c.i64[i]:(c.b[i]?1:0));
        if (intSum) isum += c.i64[i];
        k++;
        sum += x;
        if (!have) { mn = mx = x; have = true; } else { if (x<mn) mn=x; if (x>mx) mx=x; }
        double d = x - mean; mean += d / k; m2 += d * (x - mean);   // Welford
    }
    if (k == 0) return (op==Agg::SUM)? Value(0.0) : Value();
    switch (op) {
        case Agg::SUM:  return intSum ? Value((double)isum) : Value(sum);
        case Agg::MEAN: return Value(sum / (double)k);
        case Agg::MIN:  return Value(mn);
        case Agg::MAX:  return Value(mx);
        case Agg::VAR:  return (k<2)? Value(0.0) : Value(m2 / (double)(k-1));
        case Agg::STD:  return (k<2)? Value(0.0) : Value(std::sqrt(m2 / (double)(k-1)));
        default: return Value();
    }
}

// col_argsort(c, descending) → i64 index column (stable; nulls last).
// Numeric/datetime/date/bool columns pre-materialize a double key array so the
// hot comparator is a single branch (no per-compare dtype/null dispatch); utf8
// (incl. categorical rendered to text) compares by string.
inline ColumnPtr argsortOp(const Column& c, bool desc) {
    std::vector<int64_t> idx(c.n);
    for (size_t i=0;i<c.n;i++) idx[i] = (int64_t)i;
    ColumnPtr strc;   // holds a temp utf8 column when sorting a categorical
    const Column* sc = &c;
    if (c.logical == Logical::CAT) { strc = catToUtf8(c); sc = strc.get(); }

    if (sc->dtype == DType::UTF8) {
        const Column& s = *sc;
        auto less = [&](int64_t a, int64_t b) -> bool {
            bool na=!s.valid[a], nb=!s.valid[b];
            if (na || nb) { if (na && nb) return a<b; return nb; }
            int cmp=s.s[a].compare(s.s[b]); if(cmp!=0) return desc? cmp>0 : cmp<0; return a<b;
        };
        std::sort(idx.begin(), idx.end(), less);
    } else {
        std::vector<double> key(c.n);
        for (size_t i=0;i<c.n;i++)
            key[i] = c.dtype==DType::F64?c.f64[i]:(c.dtype==DType::I64?(double)c.i64[i]:(c.b[i]?1:0));
        const uint8_t* valid = c.valid.data();
        auto less = [&](int64_t a, int64_t b) -> bool {
            bool na=!valid[a], nb=!valid[b];
            if (na || nb) { if (na && nb) return a<b; return nb; }   // nulls last, stable
            double x=key[a], y=key[b];
            if (x != y) return desc? x>y : x<y;
            return a<b;   // stable
        };
        std::sort(idx.begin(), idx.end(), less);
    }
    auto o = std::make_shared<Column>(); o->dtype = DType::I64; o->n = c.n; o->valid.assign(c.n,1); o->i64 = std::move(idx);
    return o;
}

// ── Group & join (hash-based) ─────────────────────────────────────────────────
// A per-row string key across one or more key columns (type-tagged so different
// dtypes never collide; a compact, correct multi-key encoding).
inline std::string rowKey(const std::vector<ColumnPtr>& keys, size_t i) {
    std::string k;
    for (auto& c : keys) {
        if (!c->valid[i]) { k.push_back('\x00'); k.push_back('N'); continue; }
        switch (c->dtype) {
            case DType::F64:  k.push_back('F'); { double d=c->f64[i]; k.append((const char*)&d, sizeof(d)); } break;
            case DType::I64:  k.push_back('I'); { int64_t v=c->i64[i]; k.append((const char*)&v, sizeof(v)); } break;
            case DType::BOOL: k.push_back('B'); k.push_back(c->b[i]?1:0); break;
            case DType::UTF8: k.push_back('S'); k += c->s[i]; break;
        }
        k.push_back('\x01');   // field separator
    }
    return k;
}

inline std::vector<ColumnPtr> asColumnList(const Value& v) {
    std::vector<ColumnPtr> cols;
    if (isColumn(v)) { cols.push_back(asColumn(v)); return cols; }
    if (v.isList()) { for (auto& e : v.listVal) cols.push_back(asColumn(e)); return cols; }
    throw std::runtime_error("expected a column or a list of columns");
}

// Core grouping: fill gid[i] with a dense group id (0..g-1, first-seen order)
// and firstRow[g] with a representative row per group. O(n).
//   • Single key of i64/bool/f64/utf8 → typed hash map (no per-row string alloc).
//   • Multiple keys → a type-tagged combined string key (correct for any dtypes).
// Nulls form their own group (like an explicit NA bucket).
inline void groupRows(const std::vector<ColumnPtr>& keys, size_t n,
                      std::vector<int64_t>& gid, std::vector<size_t>& firstRow) {
    gid.assign(n, 0);
    if (keys.size() == 1) {
        const Column& k = *keys[0];
        int64_t nullG = -1;
        auto newGroup = [&](size_t i) { int64_t g = (int64_t)firstRow.size(); firstRow.push_back(i); return g; };
        if (k.dtype == DType::I64 || k.dtype == DType::BOOL) {
            std::unordered_map<int64_t,int64_t> m; m.reserve(n*2);
            for (size_t i=0;i<n;i++) {
                if (!k.valid[i]) { if (nullG<0) nullG=newGroup(i); gid[i]=nullG; continue; }
                int64_t key = (k.dtype==DType::I64)?k.i64[i]:(k.b[i]?1:0);
                auto it=m.find(key); if (it==m.end()) { int64_t g=newGroup(i); m.emplace(key,g); gid[i]=g; } else gid[i]=it->second;
            }
        } else if (k.dtype == DType::F64) {
            std::unordered_map<uint64_t,int64_t> m; m.reserve(n*2);
            for (size_t i=0;i<n;i++) {
                if (!k.valid[i]) { if (nullG<0) nullG=newGroup(i); gid[i]=nullG; continue; }
                uint64_t key; double d=k.f64[i]; std::memcpy(&key,&d,8);
                auto it=m.find(key); if (it==m.end()) { int64_t g=newGroup(i); m.emplace(key,g); gid[i]=g; } else gid[i]=it->second;
            }
        } else { // utf8
            std::unordered_map<std::string,int64_t> m; m.reserve(n*2);
            for (size_t i=0;i<n;i++) {
                if (!k.valid[i]) { if (nullG<0) nullG=newGroup(i); gid[i]=nullG; continue; }
                auto it=m.find(k.s[i]); if (it==m.end()) { int64_t g=(int64_t)firstRow.size(); firstRow.push_back(i); m.emplace(k.s[i],g); gid[i]=g; } else gid[i]=it->second;
            }
        }
        return;
    }
    std::unordered_map<std::string,int64_t> m; m.reserve(n*2);
    for (size_t i=0;i<n;i++) {
        std::string key = rowKey(keys, i);
        auto it=m.find(key);
        if (it==m.end()) { int64_t g=(int64_t)firstRow.size(); firstRow.push_back(i); m.emplace(std::move(key),g); gid[i]=g; }
        else gid[i]=it->second;
    }
}

// col_group_ids(keycols) → i64 column: a dense group id (0..g-1) per row.
inline ColumnPtr groupIds(const std::vector<ColumnPtr>& keys) {
    if (keys.empty()) throw std::runtime_error("col_group_ids: need at least one key column");
    size_t n = keys[0]->n;
    for (auto& c : keys) if (c->n != n) throw std::runtime_error("col_group_ids: key columns differ in length");
    std::vector<int64_t> gid; std::vector<size_t> firstRow;
    groupRows(keys, n, gid, firstRow);
    auto o = std::make_shared<Column>(); o->dtype = DType::I64; o->n = n; o->valid.assign(n,1); o->i64 = std::move(gid);
    return o;
}

// col_group_agg(keycols, valcol, op) → { "keys": [uniqueKeyCols...], "values": aggCol }
// One hash pass over the rows; group order = first-seen.
inline Value groupAgg(const std::vector<ColumnPtr>& keys, const Column& val, Agg op) {
    if (keys.empty()) throw std::runtime_error("col_group_agg: need at least one key column");
    size_t n = keys[0]->n;
    for (auto& c : keys) if (c->n != n) throw std::runtime_error("col_group_agg: length mismatch");
    if (val.n != n) throw std::runtime_error("col_group_agg: value column length mismatch");
    std::vector<int64_t> gid; std::vector<size_t> firstRow;
    groupRows(keys, n, gid, firstRow);            // typed single-key / string multi-key
    size_t g = firstRow.size();
    // Row indices per group, so we can aggregate any dtype/op correctly (a utf8
    // value column supports count/nunique/any/all; numeric supports all ops).
    std::vector<std::vector<int64_t>> rows(g);
    for (size_t i=0;i<n;i++) rows[gid[i]].push_back((int64_t)i);
    // build output key columns (same dtypes as inputs) from representative rows
    std::vector<Value> keyCols;
    for (auto& kc : keys) {
        auto oc = std::make_shared<Column>(); oc->dtype = kc->dtype; oc->n = g; oc->valid.assign(g,1);
        switch (kc->dtype) { case DType::F64:oc->f64.resize(g);break; case DType::I64:oc->i64.resize(g);break;
                             case DType::BOOL:oc->b.resize(g);break; case DType::UTF8:oc->s.resize(g);break; }
        for (size_t j=0;j<g;j++) {
            size_t r = firstRow[j];
            if (!kc->valid[r]) { oc->valid[j]=0; continue; }
            switch (kc->dtype) { case DType::F64:oc->f64[j]=kc->f64[r];break; case DType::I64:oc->i64[j]=kc->i64[r];break;
                                 case DType::BOOL:oc->b[j]=kc->b[r];break; case DType::UTF8:oc->s[j]=kc->s[r];break; }
        }
        carryMeta(*oc, *kc);   // group keys keep datetime/categorical rendering
        keyCols.push_back(wrap(oc));
    }
    // aggregate each group by gathering the value column for its rows, then
    // reusing aggOp (handles nulls, and utf8 count/nunique/any/all correctly).
    auto out = std::make_shared<Column>(); out->dtype = DType::F64; out->n = g; out->valid.assign(g,1); out->f64.resize(g);
    for (size_t j=0;j<g;j++) {
        const std::vector<int64_t>& rs = rows[j];
        Column sub; sub.dtype = val.dtype; sub.n = rs.size(); sub.valid.resize(sub.n);
        switch (val.dtype) { case DType::F64:sub.f64.resize(sub.n);break; case DType::I64:sub.i64.resize(sub.n);break;
                             case DType::BOOL:sub.b.resize(sub.n);break; case DType::UTF8:sub.s.resize(sub.n);break; }
        for (size_t k=0;k<rs.size();k++) {
            size_t r = (size_t)rs[k];
            sub.valid[k] = val.valid[r];
            switch (val.dtype) {
                case DType::F64:  sub.f64[k] = val.f64[r]; break;
                case DType::I64:  sub.i64[k] = val.i64[r]; break;
                case DType::BOOL: sub.b[k]   = val.b[r]; break;
                case DType::UTF8: sub.s[k]   = val.s[r]; break;
            }
        }
        Value r = aggOp(sub, op);
        if (r.isNull()) out->valid[j] = 0;
        else if (r.isBool()) out->f64[j] = r.boolVal ? 1.0 : 0.0;
        else out->f64[j] = r.numberVal;
    }
    ObjectMap res;
    res["keys"] = Value(std::move(keyCols));
    res["values"] = wrap(out);
    res["ngroups"] = Value((double)g);
    return Value(std::move(res));
}

enum class Join { INNER, LEFT, RIGHT, OUTER };

// Templated hash-join core. KeyOf(side, i, isnull) returns the hashable key for
// row i of side 0(left)/1(right); null keys never match (SQL semantics). Builds
// the table on the SMALLER side → O(n+m). Non-matches on an outer side yield a
// null index (so col_take produces nulls there).
template<class KeyT, class KeyOf>
inline Value joinImpl(size_t ln, size_t rn, KeyOf keyOf, Join how) {
    std::vector<int64_t> li, ri;
    auto pushPair = [&](long long a, long long b){ li.push_back(a); ri.push_back(b); };

    bool buildRight = rn <= ln;         // build on the smaller side
    int buildSide = buildRight ? 1 : 0, probeSide = buildRight ? 0 : 1;
    size_t bn = buildRight ? rn : ln, pn = buildRight ? ln : rn;

    std::unordered_map<KeyT, std::vector<int64_t>> table; table.reserve(bn*2);
    for (size_t i=0;i<bn;i++) { bool isnull; KeyT k = keyOf(buildSide, i, isnull); if (isnull) continue; table[k].push_back((int64_t)i); }

    std::vector<char> buildMatched(bn, 0);
    bool probeIsLeft = buildRight;      // probe is left iff we built on the right
    for (size_t i=0;i<pn;i++) {
        bool isnull; KeyT k = keyOf(probeSide, i, isnull);
        auto it = isnull ? table.end() : table.find(k);
        if (it == table.end()) {
            bool keep = (how==Join::OUTER) ||
                        (how==Join::LEFT && probeIsLeft) ||
                        (how==Join::RIGHT && !probeIsLeft);
            if (keep) { if (probeIsLeft) pushPair((long long)i, -1); else pushPair(-1, (long long)i); }
            continue;
        }
        for (int64_t b : it->second) {
            buildMatched[b] = 1;
            if (buildRight) pushPair((long long)i, (long long)b);   // probe=left, build=right
            else            pushPair((long long)b, (long long)i);
        }
    }
    bool buildIsLeft = !buildRight;
    bool addUnmatchedBuild = (how==Join::OUTER) ||
                             (how==Join::LEFT && buildIsLeft) ||
                             (how==Join::RIGHT && !buildIsLeft);
    if (addUnmatchedBuild)
        for (size_t b=0;b<bn;b++) if (!buildMatched[b]) { if (buildIsLeft) pushPair((long long)b,-1); else pushPair(-1,(long long)b); }

    auto mk = [](std::vector<int64_t>& v){ auto c=std::make_shared<Column>(); c->dtype=DType::I64; c->n=v.size();
        c->valid.assign(v.size(),1); for(size_t i=0;i<v.size();i++) if(v[i]<0) c->valid[i]=0; c->i64=std::move(v); return c; };
    ObjectMap res;
    res["left_idx"] = wrap(mk(li));
    res["right_idx"] = wrap(mk(ri));
    return Value(std::move(res));
}

// col_join(leftKeys, rightKeys, how) → { "left_idx": i64col, "right_idx": i64col }.
// Single typed key → a typed hash (no per-row string). Multi-key → tagged string.
inline Value joinIdx(const std::vector<ColumnPtr>& L, const std::vector<ColumnPtr>& Rk, Join how) {
    if (L.empty() || Rk.empty()) throw std::runtime_error("col_join: need key columns on both sides");
    size_t ln = L[0]->n, rn = Rk[0]->n;
    for (auto& c : L) if (c->n != ln) throw std::runtime_error("col_join: left keys length mismatch");
    for (auto& c : Rk) if (c->n != rn) throw std::runtime_error("col_join: right keys length mismatch");

    if (L.size() == 1 && Rk.size() == 1 && L[0]->dtype == Rk[0]->dtype) {
        const Column& lc = *L[0]; const Column& rc = *Rk[0];
        DType dt = lc.dtype;
        if (dt == DType::I64 || dt == DType::BOOL) {
            auto keyOf = [&](int side, size_t i, bool& isnull) -> int64_t {
                const Column& c = side==0?lc:rc; isnull = !c.valid[i];
                return dt==DType::I64 ? c.i64[i] : (c.b[i]?1:0);
            };
            return joinImpl<int64_t>(ln, rn, keyOf, how);
        } else if (dt == DType::F64) {
            auto keyOf = [&](int side, size_t i, bool& isnull) -> uint64_t {
                const Column& c = side==0?lc:rc; isnull = !c.valid[i];
                uint64_t k; double d=c.f64[i]; std::memcpy(&k,&d,8); return k;
            };
            return joinImpl<uint64_t>(ln, rn, keyOf, how);
        } else { // utf8 single key — use the string directly (no tag overhead)
            auto keyOf = [&](int side, size_t i, bool& isnull) -> std::string {
                const Column& c = side==0?lc:rc; isnull = !c.valid[i];
                return isnull ? std::string() : c.s[i];
            };
            return joinImpl<std::string>(ln, rn, keyOf, how);
        }
    }
    // multi-key: tagged combined string key
    auto keyOf = [&](int side, size_t i, bool& isnull) -> std::string {
        isnull = false; return rowKey(side==0?L:Rk, i);
    };
    return joinImpl<std::string>(ln, rn, keyOf, how);
}

// ════════════════════════════════════════════════════════════════════════════
//  TYPED I/O (Phase 4) — CSV read/write with type inference. The reader builds
//  columns directly (the file bytes never become a giant Bantu list), which is
//  what makes loading millions of rows fast and memory-light.
// ════════════════════════════════════════════════════════════════════════════

// One parsed CSV field: its text, and whether it was quoted (an empty unquoted
// field is treated as null; a quoted "" is an empty string).
struct CsvField { std::string s; bool quoted = false; };

// RFC-4180-ish parser: handles quotes, "" escapes, delimiters and newlines
// inside quotes, and \r\n line endings.
inline std::vector<std::vector<CsvField>> parseCsv(const std::string& t, char delim) {
    std::vector<std::vector<CsvField>> rows;
    std::vector<CsvField> row;
    CsvField cur;
    bool inQuotes = false, fieldStarted = false;
    size_t i = 0, n = t.size();
    auto endField = [&]() { row.push_back(cur); cur = CsvField(); fieldStarted = false; };
    auto endRow = [&]() { endField(); rows.push_back(row); row.clear(); };
    while (i < n) {
        char c = t[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < n && t[i+1] == '"') { cur.s.push_back('"'); i += 2; continue; }
                inQuotes = false; i++; continue;
            }
            cur.s.push_back(c); i++; continue;
        }
        if (c == '"') { inQuotes = true; cur.quoted = true; fieldStarted = true; i++; continue; }
        if (c == delim) { endField(); i++; continue; }
        if (c == '\r') { i++; continue; }
        if (c == '\n') { endRow(); i++; continue; }
        cur.s.push_back(c); fieldStarted = true; i++; continue;
    }
    // trailing field/row if the file didn't end in a newline
    if (fieldStarted || cur.quoted || !row.empty()) endRow();
    return rows;
}

// ── type-inference predicates ─────────────────────────────────────────────────
inline bool looksInt(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr; errno = 0;
    std::strtoll(s.c_str(), &end, 10);
    return errno == 0 && end == s.c_str() + s.size();
}
inline bool looksFloat(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr; errno = 0;
    std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size();
}
inline bool looksBool(const std::string& s) { return s == "true" || s == "false"; }

// Generic typed-column builder with inferred dtype (i64 → f64 → bool → utf8).
// valAt(i) returns the field text (read only when isNullAt(i) is false); this
// lets callers build a column straight from parsed CSV rows with no extra copy.
// Inference short-circuits: an int-looking value is also float-looking, so we
// only test the next-wider type once a narrower one has been ruled out.
template<class ValAt, class IsNull>
inline ColumnPtr buildColumn(size_t n, ValAt valAt, IsNull isNullAt) {
    bool anyVal=false, allInt=true, allFloat=true, allBool=true;
    for (size_t i=0;i<n;i++) {
        if (isNullAt(i)) continue;
        const std::string& v = valAt(i);
        anyVal = true;
        if (allInt)       { if (!looksInt(v))   { allInt=false;   if (!looksFloat(v)) { allFloat=false; if (!looksBool(v)) allBool=false; } } }
        else if (allFloat){ if (!looksFloat(v)) { allFloat=false; if (!looksBool(v)) allBool=false; } }
        else if (allBool) { if (!looksBool(v)) allBool=false; }
        if (!allInt && !allFloat && !allBool) break;   // it's utf8; stop inferring
    }
    DType dt = DType::UTF8;
    if (anyVal) { if (allInt) dt=DType::I64; else if (allFloat) dt=DType::F64; else if (allBool) dt=DType::BOOL; }
    auto c = std::make_shared<Column>(); c->dtype = dt; c->n = n; c->valid.assign(n,1);
    switch (dt) { case DType::F64:c->f64.resize(n);break; case DType::I64:c->i64.resize(n);break;
                  case DType::BOOL:c->b.resize(n);break; case DType::UTF8:c->s.resize(n);break; }
    for (size_t i=0;i<n;i++) {
        if (isNullAt(i)) { c->valid[i] = 0; continue; }
        const std::string& v = valAt(i);
        switch (dt) {
            case DType::I64:  c->i64[i] = std::strtoll(v.c_str(), nullptr, 10); break;
            case DType::F64:  c->f64[i] = std::strtod(v.c_str(), nullptr); break;
            case DType::BOOL: c->b[i]   = (v == "true") ? 1 : 0; break;
            case DType::UTF8: c->s[i]   = v; break;
        }
    }
    return c;
}

// Wrapper over parallel value/null vectors (used by read_sqlite).
inline ColumnPtr buildColumnFromStrings(const std::vector<std::string>& vals,
                                        const std::vector<char>& isNull) {
    return buildColumn(vals.size(),
        [&](size_t i) -> const std::string& { return vals[i]; },
        [&](size_t i) { return isNull[i] != 0; });
}

// ── reference (slow) reader: parse to a row/field matrix, then build columns ──
// Kept for the differential test (fast engine must equal this on well-formed CSV)
// and as a fallback via read_csv(path, {"engine":"slow"}).
inline Value readCsvTextSlow(const std::string& text, char delim, bool header) {
    auto rows = parseCsv(text, delim);
    // Drop blank lines (a single empty, unquoted field) so trailing/among-data
    // newlines don't become spurious null rows.
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const std::vector<CsvField>& r){
        return r.size() == 1 && !r[0].quoted && r[0].s.empty();
    }), rows.end());
    ObjectMap out;
    std::vector<Value> names;
    ObjectMap cols;
    if (rows.empty()) {
        out["names"] = Value(std::move(names));
        out["cols"] = Value(ObjectMap{});
        out["shape"] = Value(std::vector<Value>{ Value(0.0), Value(0.0) });
        return Value(std::move(out));
    }
    size_t ncols = 0;
    for (auto& r : rows) ncols = std::max(ncols, r.size());
    // column names
    std::vector<std::string> colNames(ncols);
    size_t dataStart = 0;
    if (header) {
        for (size_t j=0;j<ncols;j++)
            colNames[j] = (j < rows[0].size() && !rows[0][j].s.empty()) ? rows[0][j].s : ("col" + std::to_string(j));
        dataStart = 1;
    } else {
        for (size_t j=0;j<ncols;j++) colNames[j] = "col" + std::to_string(j);
    }
    size_t nrows = rows.size() - dataStart;
    static const std::string kEmpty;
    for (size_t j=0;j<ncols;j++) {
        // Build the column straight from the parsed rows (no per-column copy).
        auto isNullAt = [&](size_t r) {
            const auto& row = rows[dataStart + r];
            return j >= row.size() || (!row[j].quoted && row[j].s.empty());
        };
        auto valAt = [&](size_t r) -> const std::string& {
            const auto& row = rows[dataStart + r];
            return j < row.size() ? row[j].s : kEmpty;
        };
        names.push_back(Value(colNames[j]));
        cols[colNames[j]] = wrap(buildColumn(nrows, valAt, isNullAt));
    }
    out["names"] = Value(std::move(names));
    out["cols"] = Value(std::move(cols));
    out["shape"] = Value(std::vector<Value>{ Value((double)nrows), Value((double)ncols) });
    return Value(std::move(out));
}

// ════════════════════════════════════════════════════════════════════════════
//  TWO-PASS FAST CSV READER (perf: 1M rows < 1s)
//  ---------------------------------------------------------------------------
//  Pass 1 (index): a single scan records, per field, a raw byte span
//  [offset,offset+len) into the source buffer plus two flag bits — no per-field
//  std::string is allocated. Rows are stored CSR-style (rowStart[r]..rowStart[r+1]
//  are the field indices of row r), which handles ragged rows for free.
//  Pass 2 (materialize): for each *kept* column we infer its dtype and fill the
//  typed buffer directly. Clean fields (no quote, no '\r') use the raw span with
//  zero copying during inference/fill except the final value; only "dirty" fields
//  (quotes/escapes/'\r') are reprocessed through the RFC-4180 rules to reconstruct
//  their exact text — identical to the reference parser above.
//
//  Flag bits per field:  bit0 = quoted (had a real '"' → empty means "", not null)
//                        bit1 = dirty  (quote or '\r' → needs reprocessing)
// ════════════════════════════════════════════════════════════════════════════
inline Value readCsvFast(const std::string& text, char delim, bool header,
                         const std::vector<std::string>& usecols) {
    const char* T = text.data();
    const size_t N = text.size();

    // ── Pass 1: index field spans (CSR by row) ──────────────────────────────
    // One packed record per field (12 bytes) → a single push_back per field, and
    // cache-friendly access in Pass 2.  flag bit0 = quoted, bit1 = dirty.
    struct Field { uint32_t off; uint32_t len; uint8_t flag; };
    std::vector<Field>    fields;
    std::vector<uint32_t> rowStart;        // field-index at the start of each row
    fields.reserve(N / 8 + 16);            // rough reserve to cut reallocations
    rowStart.push_back(0);

    size_t i = 0, fStart = 0, fieldsInRow = 0;
    bool inQ = false; uint8_t flag = 0;
    auto pushField = [&](size_t endPos) {
        fields.push_back(Field{ (uint32_t)fStart, (uint32_t)(endPos - fStart), flag });
        fieldsInRow++;
    };
    auto pushRow = [&]() {
        // Drop a blank line: exactly one empty, unquoted field.
        if (fieldsInRow == 1 && fields.back().len == 0 && (fields.back().flag & 1) == 0) {
            fields.pop_back();
        } else {
            rowStart.push_back((uint32_t)fields.size());
        }
        fieldsInRow = 0;
    };
    while (i < N) {
        char c = T[i];
        if (inQ) {
            if (c == '"') {
                if (i + 1 < N && T[i+1] == '"') { i += 2; continue; }
                inQ = false; i++; continue;
            }
            i++; continue;
        }
        if (c == '"')   { inQ = true; flag |= 0b11; i++; continue; }   // quoted + dirty
        if (c == delim) { pushField(i); i++; fStart = i; flag = 0; continue; }
        if (c == '\r')  { flag |= 0b10; i++; continue; }               // dirty (stripped later)
        if (c == '\n')  { pushField(i); pushRow(); i++; fStart = i; flag = 0; continue; }
        i++;
    }
    // Flush a trailing field/row when the file did not end in a newline.
    if (i > fStart || (flag & 1) || fieldsInRow > 0) { pushField(N); pushRow(); }

    size_t totalRows = rowStart.size() - 1;
    ObjectMap out; std::vector<Value> names; ObjectMap cols;
    if (totalRows == 0) {
        out["names"] = Value(std::move(names));
        out["cols"] = Value(ObjectMap{});
        out["shape"] = Value(std::vector<Value>{ Value(0.0), Value(0.0) });
        return Value(std::move(out));
    }

    // widest row = column count
    size_t ncols = 0;
    for (size_t r = 0; r < totalRows; r++) ncols = std::max(ncols, (size_t)(rowStart[r+1] - rowStart[r]));

    // Reconstruct a field's exact text into `dst` (fast path = raw span copy).
    auto fieldText = [&](size_t fieldIdx, std::string& dst) {
        uint32_t off = fields[fieldIdx].off, len = fields[fieldIdx].len;
        if ((fields[fieldIdx].flag & 0b10) == 0) { dst.assign(T + off, len); return; }
        dst.clear(); dst.reserve(len);
        bool q = false; size_t e = off + len;
        for (size_t k = off; k < e; ) {
            char c = T[k];
            if (q) {
                if (c == '"') { if (k+1 < e && T[k+1] == '"') { dst.push_back('"'); k += 2; continue; } q = false; k++; continue; }
                dst.push_back(c); k++; continue;
            }
            if (c == '"')  { q = true; k++; continue; }
            if (c == '\r') { k++; continue; }
            dst.push_back(c); k++;
        }
    };

    // column names + header handling
    std::vector<std::string> colNames(ncols);
    size_t dataStart = 0;
    std::string scratch;
    if (header) {
        uint32_t hs = rowStart[0], he = rowStart[1];
        for (size_t j = 0; j < ncols; j++) {
            if (hs + j < he) { fieldText(hs + j, scratch); colNames[j] = scratch.empty() ? ("col"+std::to_string(j)) : scratch; }
            else colNames[j] = "col" + std::to_string(j);
        }
        dataStart = 1;
    } else {
        for (size_t j = 0; j < ncols; j++) colNames[j] = "col" + std::to_string(j);
    }
    size_t nrows = totalRows - dataStart;

    // projection: which columns to actually materialize
    std::vector<char> keep(ncols, 1);
    if (!usecols.empty()) {
        std::unordered_set<std::string> want(usecols.begin(), usecols.end());
        for (size_t j = 0; j < ncols; j++) keep[j] = want.count(colNames[j]) ? 1 : 0;
    }

    // ── Pass 2: build each kept column directly from the byte spans ──────────
    // Numbers are parsed in place (from_chars for ints; a small reused buffer +
    // strtod for floats) so a numeric column allocates no per-cell std::string.
    std::string recon;   // reused reconstruction buffer for dirty fields
    std::string numbuf;  // reused, null-terminated, for strtod
    // Return a pointer/length view of a field's exact text (clean = raw span).
    auto viewOf = [&](size_t fi, const char*& p, size_t& l) {
        if ((fields[fi].flag & 0b10) == 0) { p = T + fields[fi].off; l = fields[fi].len; return; }
        fieldText(fi, recon); p = recon.data(); l = recon.size();
    };
    auto isIntSpan = [](const char* p, size_t l) -> bool {
        if (l == 0) return false;
        long long v; auto r = std::from_chars(p, p + l, v);
        return r.ec == std::errc() && r.ptr == p + l;
    };
    auto isFloatSpan = [&](const char* p, size_t l) -> bool {
        if (l == 0) return false;
        numbuf.assign(p, l);
        char* end = nullptr; errno = 0;
        std::strtod(numbuf.c_str(), &end);
        return end == numbuf.c_str() + l;
    };
    auto isBoolSpan = [](const char* p, size_t l) -> bool {
        return (l == 4 && std::memcmp(p, "true", 4) == 0) ||
               (l == 5 && std::memcmp(p, "false", 5) == 0);
    };

    for (size_t j = 0; j < ncols; j++) {
        if (!keep[j]) continue;
        auto fieldOf = [&](size_t r) -> long long {   // field index or -1 if missing
            uint32_t rs = rowStart[dataStart + r], re = rowStart[dataStart + r + 1];
            return (rs + j < re) ? (long long)(rs + j) : -1;
        };
        auto isNullAt = [&](size_t r) -> bool {
            long long fi = fieldOf(r);
            if (fi < 0) return true;
            return fields[fi].len == 0 && (fields[fi].flag & 1) == 0;   // empty & unquoted → null
        };

        // Pass 2a: infer dtype (short-circuit i64 → f64 → bool → utf8).
        bool anyVal = false, allInt = true, allFloat = true, allBool = true;
        for (size_t r = 0; r < nrows; r++) {
            if (isNullAt(r)) continue;
            const char* p; size_t l; viewOf((size_t)fieldOf(r), p, l);
            anyVal = true;
            if (allInt)        { if (!isIntSpan(p,l))   { allInt=false;   if (!isFloatSpan(p,l)) { allFloat=false; if (!isBoolSpan(p,l)) allBool=false; } } }
            else if (allFloat) { if (!isFloatSpan(p,l)) { allFloat=false; if (!isBoolSpan(p,l)) allBool=false; } }
            else if (allBool)  { if (!isBoolSpan(p,l)) allBool=false; }
            if (!allInt && !allFloat && !allBool) break;
        }
        DType dt = DType::UTF8;
        if (anyVal) { if (allInt) dt=DType::I64; else if (allFloat) dt=DType::F64; else if (allBool) dt=DType::BOOL; }

        // Pass 2b: fill the typed buffer.
        auto c = std::make_shared<Column>(); c->dtype = dt; c->n = nrows; c->valid.assign(nrows, 1);
        switch (dt) { case DType::F64:c->f64.resize(nrows);break; case DType::I64:c->i64.resize(nrows);break;
                      case DType::BOOL:c->b.resize(nrows);break; case DType::UTF8:c->s.resize(nrows);break; }
        for (size_t r = 0; r < nrows; r++) {
            if (isNullAt(r)) { c->valid[r] = 0; continue; }
            long long fi = fieldOf(r);
            if (dt == DType::UTF8) { fieldText((size_t)fi, c->s[r]); continue; }
            const char* p; size_t l; viewOf((size_t)fi, p, l);
            switch (dt) {
                case DType::I64:  { long long v=0; std::from_chars(p, p+l, v); c->i64[r] = v; break; }
                case DType::F64:  { numbuf.assign(p, l); c->f64[r] = std::strtod(numbuf.c_str(), nullptr); break; }
                case DType::BOOL: c->b[r] = (l == 4) ? 1 : 0; break;   // "true" vs "false"
                default: break;
            }
        }
        names.push_back(Value(colNames[j]));
        cols[colNames[j]] = wrap(c);
    }
    // If projecting, keep the requested order.
    if (!usecols.empty()) {
        std::vector<Value> ordered;
        for (auto& nm : usecols) if (cols.count(nm)) ordered.push_back(Value(nm));
        names = std::move(ordered);
    }
    size_t ncolsFinal = names.size();
    out["names"] = Value(std::move(names));
    out["cols"] = Value(std::move(cols));
    out["shape"] = Value(std::vector<Value>{ Value((double)nrows), Value((double)ncolsFinal) });
    return Value(std::move(out));
}

// read a CSV string → { "names":[...], "cols":{name:column}, "shape":[rows,cols] }.
// Default engine is the fast two-pass reader; `slow` selects the reference parser.
inline Value readCsvText(const std::string& text, char delim, bool header,
                         const std::vector<std::string>& usecols = {}, bool slow = false) {
    if (slow) return readCsvTextSlow(text, delim, header);
    return readCsvFast(text, delim, header, usecols);
}

// Escape a CSV field if it contains the delimiter, a quote, or a newline.
inline std::string csvEscape(const std::string& s, char delim) {
    bool need = s.find(delim) != std::string::npos || s.find('"') != std::string::npos ||
                s.find('\n') != std::string::npos || s.find('\r') != std::string::npos;
    if (!need) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += "\"\""; else o.push_back(c); }
    o.push_back('"');
    return o;
}

// Serialize ordered columns to CSV text (nulls → empty field).
inline std::string writeCsvText(const std::vector<std::string>& names,
                                const std::vector<ColumnPtr>& cols, char delim) {
    size_t nrows = cols.empty() ? 0 : cols[0]->n;
    for (auto& c : cols) if (c->n != nrows) throw std::runtime_error("write_csv: columns differ in length");
    std::string out;
    for (size_t j=0;j<names.size();j++) { if (j) out.push_back(delim); out += csvEscape(names[j], delim); }
    out.push_back('\n');
    for (size_t r=0;r<nrows;r++) {
        for (size_t j=0;j<cols.size();j++) {
            if (j) out.push_back(delim);
            Value v = elemToValue(*cols[j], r);
            if (!v.isNull()) out += csvEscape(v.toString(), delim);
        }
        out.push_back('\n');
    }
    return out;
}

// Build a column from a vector of Bantu Values, inferring dtype (used by
// read_sqlite, where each cell arrives as a Value).
inline ColumnPtr makeColumnInferred(const std::vector<Value>& items) {
    std::vector<std::string> vals(items.size());
    std::vector<char> isNull(items.size(), 0);
    bool anyStr=false, anyFloat=false;
    for (size_t i=0;i<items.size();i++) {
        const Value& v = items[i];
        if (v.isNull()) { isNull[i]=1; continue; }
        if (v.isString()) { anyStr=true; vals[i]=v.stringVal; }
        else if (v.isBool()) { vals[i]= v.boolVal?"true":"false"; }
        else if (v.isNumber()) { if (std::floor(v.numberVal)!=v.numberVal) anyFloat=true;
            vals[i]= v.toString(); }
    }
    (void)anyStr; (void)anyFloat;
    return buildColumnFromStrings(vals, isNull);
}

} // namespace arctic
