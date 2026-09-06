#pragma once
/**
 * Bantu Language - Core Type Definitions
 * High-performance C++ interpreter with native types
 */

#include <string>
#include <variant>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <sstream>
#include <cmath>
#include <exception>
#include <algorithm>

// ============================================================
// VALUE TYPE
// ============================================================

class BantuFunction;
class ClassInstance;
class ClassDefinition;

class Value;

// ObjectMap is the canonical map type used to back Value's "object" form.
// It is also used as a local-variable type throughout the codebase
// (where Value is already complete, so the instantiation is legal).
//
// IMPORTANT (Ubuntu 22.04 / GCC 11 compatibility):
//   `std::unordered_map<std::string, Value>` CANNOT be a non-static data
//   member of `Value` itself, because Value is incomplete inside its own
//   class body and unordered_map requires its value type to be complete
//   (it instantiates `std::pair<const std::string, Value>` which stores
//   Value by value). GCC 14 (libstdc++ 14) tolerates this; GCC 11
//   (libstdc++ 11, Ubuntu 22.04) does not — it fails with
//       error: 'std::pair<_T1, _T2>::second' has incomplete type
//   See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=90893 for background.
//
//   Fix: store the map behind a `std::shared_ptr<ObjectMap>`. shared_ptr<T>
//   is specifically designed to allow incomplete T (used in pimpl patterns),
//   so the member declaration compiles even while Value is incomplete.
//   The map is only instantiated lazily, by which time Value is complete.
using ObjectMap = std::unordered_map<std::string, Value>;
using NativeFn = std::function<Value(std::vector<Value>)>;

class Value {
public:
    enum Type { NUMBER, STRING, BOOL, NULL_VAL, FUNCTION, CLASS_INSTANCE, CLASS_DEF, OBJECT, NATIVE_FN, LIST };

    Type type;

    double numberVal = 0;
    std::string stringVal;
    bool boolVal = false;
    BantuFunction* functionVal = nullptr;
    std::shared_ptr<BantuFunction> functionPtr;
    ClassInstance* classInstanceVal = nullptr;
    ClassDefinition* classDefVal = nullptr;
    // Object form — shared_ptr so the unordered_map instantiation is
    // deferred until Value is complete. Left null for non-OBJECT values;
    // all access sites already gate on `isObject()` before dereferencing.
    std::shared_ptr<ObjectMap> objectVal;
    std::vector<Value> listVal;
    std::function<Value(std::vector<Value>)> nativeFn;

    Value() : type(NULL_VAL) {}
    explicit Value(double n) : type(NUMBER), numberVal(n) {}
    explicit Value(const std::string& s) : type(STRING), stringVal(s) {}
    explicit Value(bool b) : type(BOOL), boolVal(b) {}
    explicit Value(std::nullptr_t) : type(NULL_VAL) {}
    explicit Value(BantuFunction* fn) : type(FUNCTION), functionVal(fn) {}
    explicit Value(std::shared_ptr<BantuFunction> fn) : type(FUNCTION), functionVal(fn.get()), functionPtr(std::move(fn)) {}
    explicit Value(ClassInstance* ci) : type(CLASS_INSTANCE), classInstanceVal(ci) {}
    explicit Value(ClassDefinition* cd) : type(CLASS_DEF), classDefVal(cd) {}
    // ObjectMap is taken by const-ref (NOT by value) to avoid requiring
    // `ObjectMap` to be complete at the parameter declaration site —
    // parameter declarations are NOT complete-class contexts per
    // [class.mem]/6, so a by-value ObjectMap parameter would fail to
    // compile on GCC 11 (libstdc++ 11) for the same reason as the data
    // member above. A const reference only requires ObjectMap to be
    // declared, not complete.
    explicit Value(const ObjectMap& obj) : type(OBJECT), objectVal(std::make_shared<ObjectMap>(obj)) {}
    explicit Value(std::vector<Value> lst) : type(LIST), listVal(std::move(lst)) {}
    explicit Value(NativeFn fn) : type(NATIVE_FN), nativeFn(std::move(fn)) {}

    bool isNumber() const { return type == NUMBER; }
    bool isString() const { return type == STRING; }
    bool isBool() const { return type == BOOL; }
    bool isNull() const { return type == NULL_VAL; }
    bool isFunction() const { return type == FUNCTION || type == NATIVE_FN; }
    bool isClassInstance() const { return type == CLASS_INSTANCE; }
    bool isClassDef() const { return type == CLASS_DEF; }
    bool isObject() const { return type == OBJECT; }
    bool isList() const { return type == LIST; }
    bool isNativeFn() const { return type == NATIVE_FN; }

    bool isTruthy() const {
        switch (type) {
            case NUMBER: return numberVal != 0;
            case STRING: return !stringVal.empty();
            case BOOL: return boolVal;
            case NULL_VAL: return false;
            case FUNCTION: case NATIVE_FN: return true;
            case CLASS_INSTANCE: case CLASS_DEF: return true;
            case OBJECT: return objectVal && !objectVal->empty();
            case LIST: return !listVal.empty();
        }
        return false;
    }

    std::string toString() const {
        switch (type) {
            case NUMBER: {
                if (numberVal == std::floor(numberVal) && !std::isinf(numberVal)) {
                    return std::to_string((long long)numberVal);
                }
                std::ostringstream oss;
                oss << numberVal;
                return oss.str();
            }
            case STRING: return stringVal;
            case BOOL: return boolVal ? "true" : "false";
            case NULL_VAL: return "null";
            case FUNCTION: return "<function>";
            case NATIVE_FN: return "<native function>";
            case CLASS_INSTANCE: return "<instance>";
            case CLASS_DEF: return "<class>";
            case OBJECT: {
                std::ostringstream oss;
                oss << "{";
                bool first = true;
                if (objectVal) {
                    for (const auto& [k, v] : *objectVal) {
                        if (!first) oss << ", ";
                        first = false;
                        oss << k << ": " << v.toString();
                    }
                }
                oss << "}";
                return oss.str();
            }
            case LIST: {
                std::ostringstream oss;
                oss << "[";
                for (size_t i = 0; i < listVal.size(); i++) {
                    if (i > 0) oss << ", ";
                    oss << listVal[i].toString();
                }
                oss << "]";
                return oss.str();
            }
        }
        return "null";
    }

    bool equals(const Value& other) const {
        if (type != other.type) {
            if (type == NUMBER && other.type == BOOL) return (numberVal != 0) == other.boolVal;
            if (type == BOOL && other.type == NUMBER) return boolVal == (other.numberVal != 0);
            return false;
        }
        switch (type) {
            case NUMBER: return numberVal == other.numberVal;
            case STRING: return stringVal == other.stringVal;
            case BOOL: return boolVal == other.boolVal;
            case NULL_VAL: return true;
            default: return false;
        }
    }
};

// ============================================================
// TOKEN TYPE
// ============================================================

// Windows headers (pulled in via <curl/curl.h> → <winsock2.h> → <windows.h>)
// define macros named CONST, DELETE, TRUE, FALSE that clash with the
// BantuTokenType enum values below. Undefine them so the enum compiles.
#ifdef CONST
#undef CONST
#endif
#ifdef DELETE
#undef DELETE
#endif
#ifdef TRUE
#undef TRUE
#endif
#ifdef FALSE
#undef FALSE
#endif
// IN is a Windows SAL annotation macro
#ifdef IN
#undef IN
#endif

enum class BantuTokenType {
    // Literals
    NUMBER, STRING, TRUE, FALSE, NULL_T,
    // Identifiers
    IDENTIFIER, DECLARATOR,
    // Operators
    PLUS, MINUS, MULTIPLY, DIVIDE, MODULO,
    EQUALS, GREATERTHAN, LESSTHAN,
    GREATERTHANEQUAL, LESSTHANEQUAL,
    EQUALTO, NOTEQUALTO,
    AND, OR, NOT, FATARROW,
    PLUS_EQUALS, MINUS_EQUALS, MULTIPLY_EQUALS, DIVIDE_EQUALS,
    INCREMENT, DECREMENT,
    // Delimiters
    LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET,
    BAR, PIPE, COMMA, SEMICOLON, DOT, COLON,
    // Keywords
    IF, ELSE, WHILE, FOR, EACH, IN,
    DEF, RETURN,
    PRINT, READ, DB, FETCH, AWAIT,
    CONST, PRIVATE, PUBLIC, FROM,
    TRY, CATCH, THROW,
    BREAK, CONTINUE, SWITCH, CASE, DEFAULT,
    NEW, CREATE, DELETE, UPDATE, CALC,
    CLASS, EXTENDS, IMPLEMENTS, SUPER,
    IMPORT, EXPORT,
    // v1.2.1: module include
    INCLUDE,
    TYPE_NUMBER, TYPE_STRING, TYPE_BOOL, TYPE_LIST, TYPE_DICT, TYPE_ANY, TYPE_FUNC,
    // Real-Time (sua framework)
    CHANNEL, SIGNAL, STUN, STREAM, BROADCAST, RELAY,
    CONNECT, PEER, ICE, CANDIDATE, ROOM, OFFER, ANSWER,
    // Special
    UNRECOGNIZED,
    END_OF_FILE
};

struct Token {
    BantuTokenType type;
    std::string value;
    int line;
    int col;

    Token() : type(BantuTokenType::UNRECOGNIZED), line(0), col(0) {}
    Token(BantuTokenType t, std::string v, int l, int c) : type(t), value(std::move(v)), line(l), col(c) {}
};

// ============================================================
// EXCEPTIONS (real, catchable error propagation)
// ============================================================
//
// Bantu control flow uses two families of C++ exceptions:
//   * BantuError  — a runtime/syntax/type error. Derives from std::exception
//                   so `try { } catch ($e) { }` (evalTryCatch) can catch it and
//                   the top-level/parser can recover instead of looping.
//   * BantuThrow  — a value thrown by a Bantu `throw <expr>;` statement. Carries
//                   the thrown Value so the catch block receives it verbatim.
// (BreakSignal/ContinueSignal/ReturnSignal are deliberately NOT std::exception,
//  so they pass through try/catch untouched — see evaluator.hpp.)

// A structured, position-carrying error. `what()` returns a formatted message.
struct BantuError : std::exception {
    std::string message;   // human-readable text
    std::string typeName;  // e.g. "RUNTIME ERROR", "SYNTAX ERROR"
    int line = 0;
    int col = 0;
    std::string full;      // cached "[TYPE] message (line L, col C)"

    BantuError(std::string msg, std::string type, int l = 0, int c = 0)
        : message(std::move(msg)), typeName(std::move(type)), line(l), col(c) {
        full = "[" + typeName + "] " + message;
        if (line && col) full += " (line " + std::to_string(line) + ", col " + std::to_string(col) + ")";
    }
    const char* what() const noexcept override { return full.c_str(); }
};

// A value thrown by a Bantu `throw` statement.
struct BantuThrow : std::exception {
    Value value;
    std::string rendered;
    explicit BantuThrow(Value v) : value(std::move(v)) { rendered = value.toString(); }
    const char* what() const noexcept override { return rendered.c_str(); }
};

// ============================================================
// ERROR HANDLER
// ============================================================

class ErrorHandler {
public:
    enum ErrorType {
        SYNTAX_ERROR, RUNTIME_ERROR, TYPE_ERROR,
        REFERENCE_ERROR, FILE_ERROR, NETWORK_ERROR
    };

    static std::string errorTypeName(ErrorType t) {
        switch (t) {
            case SYNTAX_ERROR:    return "SYNTAX ERROR";
            case RUNTIME_ERROR:   return "RUNTIME ERROR";
            case TYPE_ERROR:      return "TYPE ERROR";
            case REFERENCE_ERROR: return "REFERENCE ERROR";
            case FILE_ERROR:      return "FILE ERROR";
            case NETWORK_ERROR:   return "NETWORK ERROR";
        }
        return "ERROR";
    }

    // Raise a Bantu error. As of v1.3.0 this THROWS a catchable BantuError
    // instead of merely printing — the linchpin that makes try/catch work and
    // stops the parser from looping on a stuck token. Uncaught errors are
    // reported by the top-level handler in evaluator.hpp / main.cpp.
    [[noreturn]] static void throwError(const std::string& msg, int line = 0, int col = 0, ErrorType etype = RUNTIME_ERROR) {
        throw BantuError(msg, errorTypeName(etype), line, col);
    }

    static void throwSyntaxError(const std::string& msg, int line = 0, int col = 0) {
        throwError(msg, line, col, SYNTAX_ERROR);
    }

    static void throwRuntimeError(const std::string& msg, int line = 0, int col = 0) {
        throwError(msg, line, col, RUNTIME_ERROR);
    }

    static void throwTypeError(const std::string& msg, int line = 0, int col = 0) {
        throwError(msg, line, col, TYPE_ERROR);
    }

    static void throwReferenceError(const std::string& msg, int line = 0, int col = 0) {
        throwError(msg, line, col, REFERENCE_ERROR);
    }
};

class Log {
public:
    std::vector<Value> values;
    void add(const Value& v) { values.push_back(v); }
    void clear() { values.clear(); }
};

extern Log globalLog;
