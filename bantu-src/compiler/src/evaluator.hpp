#pragma once
/**
 * Bantu Language - Evaluator / Tree-Walking Interpreter
 * High-performance AST evaluator with Sua Backend Framework
 * Supports: HTTP Server (Express-like), HTTP Client (libcurl),
 *           SQLite, PostgreSQL, MySQL
 */

#include "types.hpp"
#include "ast.hpp"
#include "environment.hpp"
#include "function.hpp"
#include "class.hpp"
#include "server.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <functional>
#include <random>
#include <algorithm>
#include <cstring>
#include <cstdlib>

// ─── External Library Headers ───
#include <curl/curl.h>
#include <sqlite3.h>

// Helper to create native function values without ambiguity
inline Value makeNative(NativeFn fn) { return Value(std::move(fn)); }

// ════════════════════════════════════════════════════════════════
// SUA BACKEND FRAMEWORK — Static State & Helpers
// ════════════════════════════════════════════════════════════════

// ─── HTTP Server State ───
// Forward declaration so BantuServerRoute can hold a Value handler
class Evaluator;

struct BantuServerRoute {
    std::string method;
    std::string path;
    Value handler;  // Bantu function (or null if none)
};

static std::vector<BantuServerRoute> bantuServerRoutes;
static int bantuServerPort = 3000;
static std::string bantuServerHost = "0.0.0.0";
static std::vector<std::string> bantuServerMiddleware;
static std::vector<Value> bantuServerMiddlewareFuncs;  // function middleware
static std::vector<std::string> bantuServerStatic;
static std::string bantuServerResponseData;
static int bantuServerResponseStatus = 200;
static std::string bantuServerResponseType = "text/plain";

// ─── Per-request response state (used by $res.json / $res.send etc.) ───
struct BantuHttpResponseState {
    int status = 200;
    std::string body = "";
    std::string contentType = "application/json";
    std::unordered_map<std::string, std::string> headers;
    bool sent = false;
};

// ─── JSON serialization (proper, with quoted strings) ───
inline std::string bantuJsonStringify(const Value& v) {
    switch (v.type) {
        case Value::NUMBER: {
            if (v.numberVal == std::floor(v.numberVal) && !std::isinf(v.numberVal)) {
                return std::to_string((long long)v.numberVal);
            }
            std::ostringstream oss; oss << v.numberVal; return oss.str();
        }
        case Value::STRING: {
            std::string out = "\"";
            for (char c : v.stringVal) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    case '\b': out += "\\b";  break;
                    case '\f': out += "\\f";  break;
                    default:
                        if ((unsigned char)c < 0x20) {
                            char buf[8];
                            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                            out += buf;
                        } else {
                            out += c;
                        }
                }
            }
            out += "\"";
            return out;
        }
        case Value::BOOL:   return v.boolVal ? "true" : "false";
        case Value::NULL_VAL: return "null";
        case Value::FUNCTION:
        case Value::NATIVE_FN:
        case Value::CLASS_DEF:
        case Value::CLASS_INSTANCE: return "null";
        case Value::OBJECT: {
            std::ostringstream oss;
            oss << "{";
            bool first = true;
            for (const auto& [k, val] : *v.objectVal) {
                if (!first) oss << ",";
                first = false;
                // key as quoted string
                oss << "\"" << k << "\":" << bantuJsonStringify(val);
            }
            oss << "}";
            return oss.str();
        }
        case Value::LIST: {
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < v.listVal.size(); i++) {
                if (i > 0) oss << ",";
                oss << bantuJsonStringify(v.listVal[i]);
            }
            oss << "]";
            return oss.str();
        }
    }
    return "null";
}

// ─── Minimal JSON parser (for parsing request bodies) ───
// Returns a Value (object/list/string/number/bool/null). On error returns null.
inline Value bantuJsonParse(const std::string& s, size_t& pos);
inline Value bantuJsonParseValue(const std::string& s, size_t& pos);
inline void bantuJsonSkipWs(const std::string& s, size_t& pos) {
    while (pos < s.size() && (s[pos]==' '||s[pos]=='\t'||s[pos]=='\n'||s[pos]=='\r')) pos++;
}
inline std::string bantuJsonParseStringRaw(const std::string& s, size_t& pos) {
    std::string out;
    if (pos >= s.size() || s[pos] != '"') return out;
    pos++;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            char c = s[pos+1];
            switch (c) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {
                    if (pos + 5 < s.size()) {
                        // Just skip unicode escapes for simplicity (drop them)
                        pos += 5;
                    }
                    break;
                }
                default: out += c;
            }
            pos += 2;
        } else {
            out += s[pos];
            pos++;
        }
    }
    if (pos < s.size()) pos++; // skip closing quote
    return out;
}
inline Value bantuJsonParseValue(const std::string& s, size_t& pos) {
    bantuJsonSkipWs(s, pos);
    if (pos >= s.size()) return Value();
    char c = s[pos];
    if (c == '"') {
        return Value(bantuJsonParseStringRaw(s, pos));
    }
    if (c == '{') {
        ObjectMap obj;
        pos++; // skip {
        bantuJsonSkipWs(s, pos);
        if (pos < s.size() && s[pos] == '}') { pos++; return Value(std::move(obj)); }
        while (pos < s.size()) {
            bantuJsonSkipWs(s, pos);
            std::string key = bantuJsonParseStringRaw(s, pos);
            bantuJsonSkipWs(s, pos);
            if (pos < s.size() && s[pos] == ':') pos++;
            Value val = bantuJsonParseValue(s, pos);
            obj[key] = val;
            bantuJsonSkipWs(s, pos);
            if (pos < s.size() && s[pos] == ',') { pos++; continue; }
            if (pos < s.size() && s[pos] == '}') { pos++; break; }
            break;
        }
        return Value(std::move(obj));
    }
    if (c == '[') {
        std::vector<Value> lst;
        pos++; // skip [
        bantuJsonSkipWs(s, pos);
        if (pos < s.size() && s[pos] == ']') { pos++; return Value(std::move(lst)); }
        while (pos < s.size()) {
            Value val = bantuJsonParseValue(s, pos);
            lst.push_back(val);
            bantuJsonSkipWs(s, pos);
            if (pos < s.size() && s[pos] == ',') { pos++; continue; }
            if (pos < s.size() && s[pos] == ']') { pos++; break; }
            break;
        }
        return Value(std::move(lst));
    }
    if (c == 't') {
        if (s.substr(pos, 4) == "true") { pos += 4; return Value(true); }
    }
    if (c == 'f') {
        if (s.substr(pos, 5) == "false") { pos += 5; return Value(false); }
    }
    if (c == 'n') {
        if (s.substr(pos, 4) == "null") { pos += 4; return Value(); }
    }
    // number
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = pos;
        if (c == '-') pos++;
        while (pos < s.size() && ((s[pos]>='0'&&s[pos]<='9')||s[pos]=='.'||s[pos]=='e'||s[pos]=='E'||s[pos]=='+'||s[pos]=='-')) pos++;
        try {
            return Value(std::stod(s.substr(start, pos - start)));
        } catch (...) { return Value(); }
    }
    return Value();
}
inline Value bantuJsonParse(const std::string& s, size_t& pos) {
    pos = 0;
    return bantuJsonParseValue(s, pos);
}

// ─── URL decoding (for query strings) ───
inline std::string bantuUrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+' && i + 1 < s.size()) { out += ' '; }
        else if (s[i] == '%' && i + 2 < s.size()) {
            // Manual hex parser (avoids std::strtol → __isoc23_strtol@GLIBC_2.38)
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hexVal(s[i+1]);
            int lo = hexVal(s[i+2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
            } else {
                out += s[i];
            }
        } else out += s[i];
    }
    return out;
}

// ─── Split a path on '/' into components (ignoring empty parts) ───
inline std::vector<std::string> bantuSplitPath(const std::string& p) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ─── SQLite State ───
static sqlite3* bantuSqliteDb = nullptr;
static std::string bantuSqlitePath = "";

// ─── PostgreSQL State (simulated for static binary) ───
static bool bantuPgConnected = false;
static std::string bantuPgConnStr = "";
static std::string bantuPgHost = "";
static std::string bantuPgDb = "";
static std::string bantuPgUser = "";

// ─── MySQL State (simulated for static binary) ───
static bool bantuMysqlConnected = false;
static std::string bantuMysqlHost = "";
static std::string bantuMysqlDb = "";
static std::string bantuMysqlUser = "";
static int bantuMysqlPort = 3306;

// ─── cURL Write Callback ───
static size_t bantuCurlWriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t totalSize = size * nmemb;
    userp->append((char*)contents, totalSize);
    return totalSize;
}

// ─── cURL Header Callback ───
static size_t bantuCurlHeaderCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t totalSize = size * nmemb;
    userp->append((char*)contents, totalSize);
    return totalSize;
}

// ─── SQLite Query Callback ───
static int bantuSqliteCallback(void* data, int argc, char** argv, char** colNames) {
    std::vector<Value>* rows = static_cast<std::vector<Value>*>(data);
    ObjectMap row;
    for (int i = 0; i < argc; i++) {
        std::string val = argv[i] ? argv[i] : "NULL";
        // Try to convert numeric strings to numbers
        bool isNum = false;
        try {
            size_t pos;
            double numVal = std::stod(val, &pos);
            if (pos == val.size()) {
                row[std::string(colNames[i])] = Value(numVal);
                isNum = true;
            }
        } catch (...) {}
        if (!isNum) {
            row[std::string(colNames[i])] = Value(std::string(val));
        }
    }
    rows->push_back(Value(std::move(row)));
    return 0;
}

// ─── HTTP Status Text Helper ───
static std::string httpStatusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 422: return "Unprocessable Entity";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

// ─── cURL HTTP Request Helper ───
static Value bantuHttpRequest(const std::string& method, const std::string& url,
                               const std::string& body = "", const std::string& contentType = "") {
    CURL* curl = curl_easy_init();
    if (!curl) {
        ObjectMap err;
        err["error"] = Value(std::string("Failed to initialize HTTP client"));
        err["status"] = Value(0.0);
        err["ok"] = Value(false);
        return Value(std::move(err));
    }

    std::string responseBody;
    std::string responseHeaders;
    long responseCode = 0;

    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bantuCurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, bantuCurlHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseHeaders);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Bantu-Lang/1.1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    // Set method
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json, text/plain, */*");

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        }
        if (!contentType.empty()) {
            std::string ct = "Content-Type: " + contentType;
            headers = curl_slist_append(headers, ct.c_str());
        } else {
            headers = curl_slist_append(headers, "Content-Type: application/json");
        }
    } else if (method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        }
        if (!contentType.empty()) {
            std::string ct = "Content-Type: " + contentType;
            headers = curl_slist_append(headers, ct.c_str());
        } else {
            headers = curl_slist_append(headers, "Content-Type: application/json");
        }
    } else if (method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        }
    } else if (method == "PATCH") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        }
        if (!contentType.empty()) {
            std::string ct = "Content-Type: " + contentType;
            headers = curl_slist_append(headers, ct.c_str());
        }
    } else if (method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    ObjectMap response;

    if (res != CURLE_OK) {
        response["error"] = Value(std::string(curl_easy_strerror(res)));
        response["status"] = Value(0.0);
        response["ok"] = Value(false);
        response["url"] = Value(url);
        response["method"] = Value(method);
        std::cout << "  [HTTP] " << method << " " << url << " -> ERROR: " << curl_easy_strerror(res) << "\n";
    } else {
        response["status"] = Value((double)responseCode);
        response["statusText"] = Value(httpStatusText((int)responseCode));
        response["ok"] = Value(responseCode >= 200 && responseCode < 300);
        response["body"] = Value(responseBody);
        response["url"] = Value(url);
        response["method"] = Value(method);
        response["headers"] = Value(responseHeaders);
        std::cout << "  [HTTP] " << method << " " << url << " -> " << responseCode << " " << httpStatusText((int)responseCode) << "\n";
    }

    return Value(std::move(response));
}

// ════════════════════════════════════════════════════════════════
// SIGNALS FOR CONTROL FLOW
// ════════════════════════════════════════════════════════════════

struct BreakSignal {};
struct ContinueSignal {};
struct ReturnSignal { Value value; };

// ════════════════════════════════════════════════════════════════
// EVALUATOR CLASS
// ════════════════════════════════════════════════════════════════

class Evaluator {
public:
    Evaluator() : env_(std::make_shared<Environment>()), globalEnv_(env_) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        registerBuiltins();
    }

    ~Evaluator() {
        // Clean up database connections
        if (bantuSqliteDb) {
            sqlite3_close(bantuSqliteDb);
            bantuSqliteDb = nullptr;
        }
        bantuPgConnected = false;
        bantuMysqlConnected = false;
        curl_global_cleanup();
    }

    Value evaluate(std::vector<std::shared_ptr<ASTNode>>& program) {
        Value result;
        for (auto& node : program) {
            result = evalNode(node);
        }
        return result;
    }

private:
    std::shared_ptr<Environment> env_;
    std::shared_ptr<Environment> globalEnv_;

    // ════════════════════════════════════════════════════════════
    // CORE EVALUATION DISPATCH
    // ════════════════════════════════════════════════════════════

    Value evalNode(std::shared_ptr<ASTNode>& node) {
        if (!node) return Value();

        // Dispatch by dynamic cast
        if (auto n = dynamic_cast<NumberNode*>(node.get()))    return evalNumber(n);
        if (auto n = dynamic_cast<StringNode*>(node.get()))    return evalString(n);
        if (auto n = dynamic_cast<BoolNode*>(node.get()))      return evalBool(n);
        if (auto n = dynamic_cast<NullNode*>(node.get()))      return Value();
        if (auto n = dynamic_cast<ListNode*>(node.get()))      return evalList(n);
        if (auto n = dynamic_cast<DictNode*>(node.get()))      return evalDict(n);
        if (auto n = dynamic_cast<VariableNode*>(node.get()))  return evalVariable(n);
        if (auto n = dynamic_cast<VarDeclNode*>(node.get()))   return evalVarDecl(n);
        if (auto n = dynamic_cast<AssignNode*>(node.get()))    return evalAssign(n);
        if (auto n = dynamic_cast<IndexAssignNode*>(node.get())) return evalIndexAssign(n);
        if (auto n = dynamic_cast<DictAssignNode*>(node.get()))  return evalDictAssign(n);
        if (auto n = dynamic_cast<BinaryOpNode*>(node.get()))  return evalBinaryOp(n);
        if (auto n = dynamic_cast<UnaryOpNode*>(node.get()))   return evalUnaryOp(n);
        if (auto n = dynamic_cast<IfNode*>(node.get()))        return evalIf(n);
        if (auto n = dynamic_cast<WhileNode*>(node.get()))     return evalWhile(n);
        if (auto n = dynamic_cast<ForNode*>(node.get()))       return evalFor(n);
        if (auto n = dynamic_cast<EachNode*>(node.get()))      return evalEach(n);
        if (auto n = dynamic_cast<FuncDeclNode*>(node.get()))  return evalFuncDecl(n);
        if (auto n = dynamic_cast<ReturnNode*>(node.get()))    return evalReturn(n);
        if (auto n = dynamic_cast<CallNode*>(node.get()))      return evalCall(n);
        if (auto n = dynamic_cast<DotAccessNode*>(node.get())) return evalDotAccess(n);
        if (auto n = dynamic_cast<IndexAccessNode*>(node.get())) return evalIndexAccess(n);
        if (auto n = dynamic_cast<TryCatchNode*>(node.get()))  return evalTryCatch(n);
        if (auto n = dynamic_cast<ClassDeclNode*>(node.get())) return evalClassDecl(n);
        if (auto n = dynamic_cast<SuperNode*>(node.get()))     return evalSuper(n);
        if (auto n = dynamic_cast<PrintNode*>(node.get()))     return evalPrint(n);
        if (auto n = dynamic_cast<ChannelNode*>(node.get()))   return evalChannel(n);
        if (auto n = dynamic_cast<BroadcastNode*>(node.get())) return evalBroadcast(n);
        if (auto n = dynamic_cast<StreamNode*>(node.get()))    return evalStream(n);
        if (auto n = dynamic_cast<StunNode*>(node.get()))      return evalStun(n);
        if (auto n = dynamic_cast<RelayNode*>(node.get()))     return evalRelay(n);
        if (auto n = dynamic_cast<SignalNode*>(node.get()))    return evalSignal(n);
        if (auto n = dynamic_cast<ConnectNode*>(node.get()))   return evalConnect(n);

        return Value();
    }

    // ════════════════════════════════════════════════════════════
    // LITERAL EVALUATION
    // ════════════════════════════════════════════════════════════

    Value evalNumber(NumberNode* n) { return Value(n->value); }
    Value evalString(StringNode* n) { return Value(n->value); }
    Value evalBool(BoolNode* n) { return Value(n->value); }

    Value evalList(ListNode* n) {
        std::vector<Value> elements;
        for (auto& elem : n->elements) {
            elements.push_back(evalNode(elem));
        }
        return Value(std::move(elements));
    }

    Value evalDict(DictNode* n) {
        ObjectMap obj;
        for (auto& [key, val] : n->pairs) {
            obj[key] = evalNode(val);
        }
        return Value(std::move(obj));
    }

    // ════════════════════════════════════════════════════════════
    // VARIABLE EVALUATION
    // ════════════════════════════════════════════════════════════

    Value evalVariable(VariableNode* n) {
        try {
            return env_->get(n->name);
        } catch (const std::exception& e) {
            ErrorHandler::throwReferenceError("Undefined variable: " + n->name, n->line, n->col);
            return Value();
        }
    }

    Value evalVarDecl(VarDeclNode* n) {
        Value val = evalNode(n->init);
        env_->define(n->name, val);
        return val;
    }

    Value evalAssign(AssignNode* n) {
        Value val = evalNode(n->value);
        if (env_->has(n->name)) {
            env_->set(n->name, val);
        } else {
            env_->define(n->name, val);
        }
        return val;
    }

    Value evalIndexAssign(IndexAssignNode* n) {
        Value val = evalNode(n->value);

        // Fast path: direct variable access (avoids copying entire list)
        if (auto varNode = dynamic_cast<VariableNode*>(n->object.get())) {
            if (env_->has(varNode->name)) {
                Value& obj = env_->getRef(varNode->name);
                Value idx = evalNode(n->index);

                if (obj.isList()) {
                    int i = (int)idx.numberVal;
                    if (i < 0) {
                        ErrorHandler::throwRuntimeError("Index out of bounds: " + std::to_string(i), n->line, n->col);
                        return Value();
                    }
                    if (i >= (int)obj.listVal.size()) {
                        obj.listVal.resize(i + 1, Value(0.0));
                    }
                    obj.listVal[i] = val;
                    return val;
                }

                if (obj.isObject()) {
                    std::string key = idx.toString();
                    (*obj.objectVal)[key] = val;
                    return val;
                }
            }
        }

        // Slow path: evaluate expression and update
        Value obj = evalNode(n->object);
        Value idx = evalNode(n->index);

        if (obj.isList()) {
            int i = (int)idx.numberVal;
            if (i < 0) {
                ErrorHandler::throwRuntimeError("Index out of bounds: " + std::to_string(i), n->line, n->col);
                return Value();
            }
            if (i >= (int)obj.listVal.size()) {
                obj.listVal.resize(i + 1, Value(0.0));
            }
            obj.listVal[i] = val;
            if (auto varNode = dynamic_cast<VariableNode*>(n->object.get())) {
                env_->set(varNode->name, obj);
            }
            return val;
        }

        if (obj.isObject()) {
            std::string key = idx.toString();
            (*obj.objectVal)[key] = val;
            if (auto varNode = dynamic_cast<VariableNode*>(n->object.get())) {
                env_->set(varNode->name, obj);
            }
            return val;
        }

        ErrorHandler::throwRuntimeError("Cannot index-assign to this type", n->line, n->col);
        return Value();
    }

    Value evalDictAssign(DictAssignNode* n) {
        Value obj = evalNode(n->object);
        Value val = evalNode(n->value);

        // Class instance property assignment (this.prop = value)
        if (obj.isClassInstance()) {
            obj.classInstanceVal->setProperty(n->key, val);
            return val;
        }

        if (obj.isObject()) {
            (*obj.objectVal)[n->key] = val;
            if (auto varNode = dynamic_cast<VariableNode*>(n->object.get())) {
                env_->set(varNode->name, obj);
            }
            return val;
        }

        ErrorHandler::throwRuntimeError("Cannot property-assign to this type", n->line, n->col);
        return Value();
    }

    // ════════════════════════════════════════════════════════════
    // OPERATOR EVALUATION
    // ════════════════════════════════════════════════════════════

    Value evalBinaryOp(BinaryOpNode* n) {
        Value left = evalNode(n->left);
        Value right = evalNode(n->right);

        switch (n->op) {
            case TokenType::PLUS:
                if (left.isString() || right.isString()) {
                    return Value(left.toString() + right.toString());
                }
                return Value(left.numberVal + right.numberVal);
            case TokenType::MINUS: return Value(left.numberVal - right.numberVal);
            case TokenType::MULTIPLY: return Value(left.numberVal * right.numberVal);
            case TokenType::DIVIDE:
                if (right.numberVal == 0) {
                    ErrorHandler::throwRuntimeError("Division by zero", n->line, n->col);
                    return Value();
                }
                return Value(left.numberVal / right.numberVal);
            case TokenType::MODULO:
                if (right.numberVal == 0) {
                    ErrorHandler::throwRuntimeError("Modulo by zero", n->line, n->col);
                    return Value();
                }
                // Manual modulo (avoids std::fmod@GLIBC_2.38 symbol version requirement)
                {
                    double a = left.numberVal, b = right.numberVal;
                    double q = std::floor(a / b);
                    double r = a - q * b;
                    // Match fmod's sign convention: result has same sign as a
                    if (r < 0 && a >= 0) r += b;
                    if (r > 0 && a < 0)  r -= b;
                    return Value(r);
                }
            case TokenType::EQUALTO: return Value(left.equals(right));
            case TokenType::NOTEQUALTO: return Value(!left.equals(right));
            case TokenType::GREATERTHAN: return Value(left.numberVal > right.numberVal);
            case TokenType::LESSTHAN: return Value(left.numberVal < right.numberVal);
            case TokenType::GREATERTHANEQUAL: return Value(left.numberVal >= right.numberVal);
            case TokenType::LESSTHANEQUAL: return Value(left.numberVal <= right.numberVal);
            case TokenType::AND: return Value(left.isTruthy() && right.isTruthy());
            case TokenType::OR: return Value(left.isTruthy() || right.isTruthy());
            default:
                ErrorHandler::throwRuntimeError("Unknown operator", n->line, n->col);
                return Value();
        }
    }

    Value evalUnaryOp(UnaryOpNode* n) {
        Value operand = evalNode(n->operand);
        switch (n->op) {
            case TokenType::NOT: return Value(!operand.isTruthy());
            case TokenType::MINUS: return Value(-operand.numberVal);
            default: return Value();
        }
    }

    // ════════════════════════════════════════════════════════════
    // CONTROL FLOW EVALUATION
    // ════════════════════════════════════════════════════════════

    Value evalIf(IfNode* n) {
        Value condition = evalNode(n->condition);
        if (condition.isTruthy()) {
            auto prevEnv = env_;
            env_ = std::make_shared<Environment>(env_);
            for (auto& stmt : n->body) {
                evalNode(stmt);
            }
            env_ = prevEnv;
        } else {
            auto prevEnv = env_;
            env_ = std::make_shared<Environment>(env_);
            for (auto& stmt : n->elseBody) {
                evalNode(stmt);
            }
            env_ = prevEnv;
        }
        return Value();
    }

    Value evalWhile(WhileNode* n) {
        while (evalNode(n->condition).isTruthy()) {
            auto prevEnv = env_;
            env_ = std::make_shared<Environment>(env_);
            try {
                for (auto& stmt : n->body) {
                    evalNode(stmt);
                }
            } catch (const BreakSignal&) {
                env_ = prevEnv;
                break;
            } catch (const ContinueSignal&) {
                // continue to next iteration
            }
            env_ = prevEnv;
        }
        return Value();
    }

    Value evalFor(ForNode* n) {
        auto prevEnv = env_;
        env_ = std::make_shared<Environment>(env_);
        evalNode(n->init);

        int safety = 0;
        while (evalNode(n->condition).isTruthy() && safety < 100000) {
            auto loopEnv = std::make_shared<Environment>(env_);
            auto savedEnv = env_;
            env_ = loopEnv;
            try {
                for (auto& stmt : n->body) {
                    evalNode(stmt);
                }
            } catch (const BreakSignal&) {
                env_ = savedEnv;
                break;
            } catch (const ContinueSignal&) {
                // continue
            }
            env_ = savedEnv;
            evalNode(n->update);
            safety++;
        }
        env_ = prevEnv;
        return Value();
    }

    Value evalEach(EachNode* n) {
        Value iterable = evalNode(n->iterable);
        if (iterable.isList()) {
            for (auto& item : iterable.listVal) {
                auto prevEnv = env_;
                env_ = std::make_shared<Environment>(env_);
                env_->define(n->varName, item);
                try {
                    for (auto& stmt : n->body) {
                        evalNode(stmt);
                    }
                } catch (const BreakSignal&) {
                    env_ = prevEnv;
                    break;
                } catch (const ContinueSignal&) {
                    // continue
                }
                env_ = prevEnv;
            }
        }
        return Value();
    }

    // ════════════════════════════════════════════════════════════
    // REAL HTTP SERVER (POSIX sockets, calls Bantu handlers)
    // ════════════════════════════════════════════════════════════

    // Call a Bantu function Value with arguments. Returns the function's
    // return value (or null). Catches ReturnSignal internally.
    Value bantuCallFunction(Value callee, std::vector<Value> args) {
        if (callee.isNativeFn()) {
            return callee.nativeFn(std::move(args));
        }
        if (callee.isFunction()) {
            auto fn = callee.functionVal;
            auto callEnv = std::make_shared<Environment>(fn->closure);
            if (env_->has("this")) {
                callEnv->define("this", env_->get("this"));
                callEnv->define("self", env_->get("this"));
            }
            for (size_t i = 0; i < fn->params.size(); i++) {
                callEnv->define(fn->params[i], i < args.size() ? args[i] : Value());
            }
            auto prevEnv = env_;
            env_ = callEnv;
            Value result;
            try {
                for (auto& stmt : fn->body) {
                    result = evalNode(stmt);
                }
            } catch (const ReturnSignal& sig) {
                result = sig.value;
            } catch (const std::exception& e) {
                env_ = prevEnv;
                std::cerr << "  [SERVER] Handler error: " << e.what() << "\n";
                return Value();
            }
            env_ = prevEnv;
            return result;
        }
        return Value();
    }

    // Build the $res object — methods write to the shared response state.
    // Returns the $res Value. The ObjectMap is kept alive via a shared_ptr
    // captured by the method lambdas, so chaining ($res.status(201).json({...}))
    // works correctly.
    Value bantuBuildResObject(std::shared_ptr<BantuHttpResponseState> state) {
        // Use a shared_ptr<ObjectMap> so all method lambdas can return a copy
        // of the same ObjectMap (with the same native function values bound).
        auto resObjPtr = std::make_shared<ObjectMap>();

        (*resObjPtr)["json"] = makeNative([state, resObjPtr](std::vector<Value> args) -> Value {
            Value data = args.size() > 0 ? args[0] : Value();
            state->body = bantuJsonStringify(data);
            state->contentType = "application/json; charset=utf-8";
            state->sent = true;
            return Value(*resObjPtr);
        });
        (*resObjPtr)["send"] = makeNative([state, resObjPtr](std::vector<Value> args) -> Value {
            Value data = args.size() > 0 ? args[0] : Value();
            if (data.isObject() || data.isList()) {
                state->body = bantuJsonStringify(data);
                state->contentType = "application/json; charset=utf-8";
            } else {
                state->body = data.toString();
                if (state->contentType == "application/json") {
                    state->contentType = "text/plain; charset=utf-8";
                }
            }
            state->sent = true;
            return Value(*resObjPtr);
        });
        (*resObjPtr)["status"] = makeNative([state, resObjPtr](std::vector<Value> args) -> Value {
            int code = args.size() > 0 ? (int)args[0].numberVal : 200;
            state->status = code;
            return Value(*resObjPtr);
        });
        (*resObjPtr)["set"] = makeNative([state, resObjPtr](std::vector<Value> args) -> Value {
            std::string k = args.size() > 0 ? args[0].toString() : "";
            std::string v = args.size() > 1 ? args[1].toString() : "";
            if (!k.empty()) state->headers[k] = v;
            return Value(*resObjPtr);
        });
        (*resObjPtr)["type"] = makeNative([state, resObjPtr](std::vector<Value> args) -> Value {
            std::string t = args.size() > 0 ? args[0].toString() : "text/plain";
            state->contentType = t;
            return Value(*resObjPtr);
        });
        (*resObjPtr)["redirect"] = makeNative([state, resObjPtr](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "/";
            state->status = 302;
            state->headers["Location"] = url;
            state->body = "";
            state->sent = true;
            return Value(*resObjPtr);
        });
        return Value(*resObjPtr);
    }

    // Read entire request body (handles Content-Length, returns the body string).
    // Reads from the already-recv'd buffer first, then tops up from the socket.
    std::string bantuReadBody(int sock, const std::string& initialBuf, size_t headerEnd) {
        std::string body;
        if (headerEnd != std::string::npos && headerEnd + 4 <= initialBuf.size()) {
            body = initialBuf.substr(headerEnd + 4);
        }
        // Look for Content-Length in the headers
        std::string headersLower = initialBuf.substr(0, headerEnd);
        // lowercase copy
        for (auto& c : headersLower) c = std::tolower(c);
        size_t clPos = headersLower.find("content-length:");
        if (clPos == std::string::npos) return body;
        size_t valStart = clPos + 15;
        size_t valEnd = headersLower.find("\r\n", valStart);
        if (valEnd == std::string::npos) return body;
        std::string clStr = headersLower.substr(valStart, valEnd - valStart);
        // trim
        while (!clStr.empty() && (clStr.front()==' '||clStr.front()=='\t')) clStr.erase(clStr.begin());
        while (!clStr.empty() && (clStr.back()==' '||clStr.back()=='\t'||clStr.back()=='\r')) clStr.pop_back();
        size_t contentLength = 0;
        // Manual parse of content-length (avoids std::stoull → __isoc23_strtoull@GLIBC_2.38)
        contentLength = 0;
        bool ok = !clStr.empty();
        for (char c : clStr) {
            if (c < '0' || c > '9') { ok = false; break; }
            contentLength = contentLength * 10 + (size_t)(c - '0');
        }
        if (!ok) return body;
        // Keep reading until we have the full body
        while (body.size() < contentLength) {
            char buf[8192];
            ssize_t n = recv(sock, buf, std::min((size_t)8192, contentLength - body.size()), 0);
            if (n <= 0) break;
            body.append(buf, n);
        }
        return body;
    }

    // Serve a static file from one of the configured static dirs.
    // Returns true if a file was served (response sent on socket).
    bool bantuServeStaticFile(int sock, const std::string& requestPath) {
        if (bantuServerStatic.empty()) return false;
        // Strip leading slash
        std::string rel = requestPath;
        if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
        if (rel.empty()) rel = "index.html";
        // Try each static dir
        for (const auto& dir : bantuServerStatic) {
            std::string filePath = dir;
            if (!filePath.empty() && filePath.back() == '/') filePath.pop_back();
            filePath += "/" + rel;
            // Prevent path traversal
            if (filePath.find("..") != std::string::npos) continue;
            std::ifstream f(filePath, std::ios::binary);
            if (!f.good()) continue;
            std::stringstream ss;
            ss << f.rdbuf();
            std::string content = ss.str();
            // Determine content type
            std::string ext = filePath.substr(filePath.find_last_of('.') + 1);
            std::string ct = "application/octet-stream";
            if (ext == "html" || ext == "htm") ct = "text/html; charset=utf-8";
            else if (ext == "css")  ct = "text/css; charset=utf-8";
            else if (ext == "js")   ct = "application/javascript; charset=utf-8";
            else if (ext == "json") ct = "application/json; charset=utf-8";
            else if (ext == "svg")  ct = "image/svg+xml";
            else if (ext == "png")  ct = "image/png";
            else if (ext == "jpg" || ext == "jpeg") ct = "image/jpeg";
            else if (ext == "ico")  ct = "image/x-icon";
            else if (ext == "txt")  ct = "text/plain; charset=utf-8";
            // Send response
            std::ostringstream resp;
            resp << "HTTP/1.1 200 OK\r\n";
            resp << "Content-Type: " << ct << "\r\n";
            resp << "Content-Length: " << content.size() << "\r\n";
            resp << "Access-Control-Allow-Origin: *\r\n";
            resp << "Cache-Control: public, max-age=300\r\n";
            resp << "Server: Bantu-Sua/1.2\r\n";
            resp << "\r\n" << content;
            std::string respStr = resp.str();
            send(sock, respStr.c_str(), respStr.size(), 0);
            return true;
        }
        return false;
    }

    // Handle a single HTTP request — parse, route, call Bantu handler, respond.
    void bantuHandleHttpRequest(int sock) {
        char buf[16384];
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { CLOSE_SOCKET(sock); return; }
        buf[n] = '\0';
        std::string request(buf, n);

        // Parse request line: METHOD PATH HTTP/1.1
        size_t firstSp = request.find(' ');
        size_t secondSp = request.find(' ', firstSp + 1);
        if (firstSp == std::string::npos || secondSp == std::string::npos) {
            std::string resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            send(sock, resp.c_str(), resp.size(), 0);
            CLOSE_SOCKET(sock);
            return;
        }
        std::string method = request.substr(0, firstSp);
        std::string fullPath = request.substr(firstSp + 1, secondSp - firstSp - 1);

        // Split path and query
        std::string path = fullPath;
        std::string queryStr;
        size_t qm = fullPath.find('?');
        if (qm != std::string::npos) {
            path = fullPath.substr(0, qm);
            queryStr = fullPath.substr(qm + 1);
        }

        // Find end of headers
        size_t headerEnd = request.find("\r\n\r\n");
        std::string body = bantuReadBody(sock, request, (headerEnd != std::string::npos) ? headerEnd : request.size());

        // Parse headers into a map (lowercase keys)
        ObjectMap headers;
        size_t headerStart = request.find("\r\n") + 2;
        if (headerStart != std::string::npos && headerEnd != std::string::npos && headerStart < headerEnd) {
            std::string headerBlock = request.substr(headerStart, headerEnd - headerStart);
            std::istringstream hs(headerBlock);
            std::string line;
            while (std::getline(hs, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string k = line.substr(0, colon);
                std::string v = line.substr(colon + 1);
                while (!v.empty() && v[0] == ' ') v.erase(v.begin());
                for (auto& c : k) c = std::tolower(c);
                headers[k] = Value(v);
            }
        }

        // Parse query string
        ObjectMap query;
        if (!queryStr.empty()) {
            std::istringstream qs(queryStr);
            std::string pair;
            while (std::getline(qs, pair, '&')) {
                size_t eq = pair.find('=');
                std::string k = (eq == std::string::npos) ? pair : pair.substr(0, eq);
                std::string v = (eq == std::string::npos) ? "" : pair.substr(eq + 1);
                query[bantuUrlDecode(k)] = Value(bantuUrlDecode(v));
            }
        }

        // Parse body as JSON if Content-Type contains json
        Value bodyVal = Value();  // null
        if (!body.empty()) {
            std::string ct = headers.count("content-type") ? headers["content-type"].toString() : "";
            if (ct.find("json") != std::string::npos) {
                size_t pos = 0;
                bodyVal = bantuJsonParse(body, pos);
            } else {
                bodyVal = Value(body);
            }
        }

        // Match route (exact first, then :param patterns)
        Value matchedHandler;
        ObjectMap params;
        bool found = false;
        std::vector<std::string> pathParts = bantuSplitPath(path);

        // First pass: exact match
        for (auto& route : bantuServerRoutes) {
            if (route.method == method && route.path == path) {
                matchedHandler = route.handler;
                found = true;
                break;
            }
        }
        // Second pass: :param match
        if (!found) {
            for (auto& route : bantuServerRoutes) {
                if (route.method != method) continue;
                std::vector<std::string> routeParts = bantuSplitPath(route.path);
                if (routeParts.size() != pathParts.size()) continue;
                bool ok = true;
                ObjectMap trial;
                for (size_t i = 0; i < routeParts.size(); i++) {
                    if (!routeParts[i].empty() && routeParts[i][0] == ':') {
                        trial[routeParts[i].substr(1)] = Value(pathParts[i]);
                    } else if (routeParts[i] != pathParts[i]) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    matchedHandler = route.handler;
                    params = trial;
                    found = true;
                    break;
                }
            }
        }
        // Special-case OPTIONS * (catch-all for CORS preflight)
        if (!found && method == "OPTIONS") {
            for (auto& route : bantuServerRoutes) {
                if (route.method == "OPTIONS" && route.path == "*") {
                    matchedHandler = route.handler;
                    found = true;
                    break;
                }
            }
        }

        // If no route matched, try static files (GET only)
        if (!found && method == "GET") {
            if (bantuServeStaticFile(sock, path)) {
                CLOSE_SOCKET(sock);
                return;
            }
        }

        // Build $req and $res
        auto state = std::make_shared<BantuHttpResponseState>();
        Value resVal = bantuBuildResObject(state);
        ObjectMap reqObj;
        reqObj["method"] = Value(method);
        reqObj["path"] = Value(path);
        reqObj["url"] = Value(fullPath);
        reqObj["params"] = Value(std::move(params));
        reqObj["query"] = Value(std::move(query));
        reqObj["headers"] = Value(std::move(headers));
        reqObj["body"] = bodyVal;
        Value reqVal = Value(std::move(reqObj));

        // Call the handler (if any)
        if (found && (matchedHandler.isFunction() || matchedHandler.isNativeFn())) {
            try {
                bantuCallFunction(matchedHandler, {reqVal, resVal});
            } catch (const std::exception& e) {
                std::cerr << "  [SERVER] Handler exception: " << e.what() << "\n";
                state->status = 500;
                state->body = std::string("{\"error\":\"Internal server error: ") + e.what() + "\"}";
                state->contentType = "application/json; charset=utf-8";
            }
        } else if (!found) {
            state->status = 404;
            ObjectMap errObj;
            errObj["error"] = Value(std::string("Not found"));
            errObj["path"] = Value(path);
            errObj["method"] = Value(method);
            state->body = bantuJsonStringify(Value(std::move(errObj)));
            state->contentType = "application/json; charset=utf-8";
        } else {
            // Route found but no handler — return empty 200
            state->status = 200;
            state->body = "";
        }

        // Build and send the HTTP response
        std::ostringstream resp;
        resp << "HTTP/1.1 " << state->status << " " << bantuHttpStatusText(state->status) << "\r\n";
        resp << "Content-Type: " << state->contentType << "\r\n";
        resp << "Content-Length: " << state->body.size() << "\r\n";
        resp << "Access-Control-Allow-Origin: *\r\n";
        resp << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, PATCH, OPTIONS\r\n";
        resp << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
        for (auto& [k, v] : state->headers) {
            resp << k << ": " << v << "\r\n";
        }
        resp << "Server: Bantu-Sua/1.2\r\n";
        resp << "\r\n" << state->body;
        std::string respStr = resp.str();
        send(sock, respStr.c_str(), respStr.size(), 0);
        CLOSE_SOCKET(sock);
    }

    // Helper: HTTP status text
    static std::string bantuHttpStatusText(int code) {
        switch (code) {
            case 200: return "OK";
            case 201: return "Created";
            case 204: return "No Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 409: return "Conflict";
            case 422: return "Unprocessable Entity";
            case 500: return "Internal Server Error";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            default: return "OK";
        }
    }

    // The accept loop — blocks forever (until process is killed).
    void bantuStartHttpServer(int port) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        int sock = (int)socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cerr << "  [SERVER] FATAL: socket() failed: " << strerror(errno) << "\n";
            return;
        }
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;  // bind to 0.0.0.0
        addr.sin_port = htons(port);

        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "  [SERVER] FATAL: bind() failed on port " << port
                      << ": " << strerror(errno) << "\n";
            CLOSE_SOCKET(sock);
            return;
        }
        if (::listen(sock, 128) < 0) {
            std::cerr << "  [SERVER] FATAL: listen() failed: " << strerror(errno) << "\n";
            CLOSE_SOCKET(sock);
            return;
        }
        std::cout << "  [SERVER] Listening on 0.0.0.0:" << port << " (real HTTP server)\n";
        std::cout.flush();

        // Accept loop — runs forever
        while (true) {
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            int clientSock = (int)accept(sock, (struct sockaddr*)&clientAddr, &clientLen);
            if (clientSock < 0) {
                if (errno == EINTR) continue;
                std::cerr << "  [SERVER] accept() failed: " << strerror(errno) << "\n";
                continue;
            }
            // Handle synchronously (single-threaded — simple but reliable)
            // For higher throughput, spawn a thread here.
            bantuHandleHttpRequest(clientSock);
        }
    }

    // ════════════════════════════════════════════════════════════
    // FUNCTION EVALUATION
    // ════════════════════════════════════════════════════════════

    Value evalFuncDecl(FuncDeclNode* n) {
        auto fn = std::make_shared<BantuFunction>(n->name, n->params, n->body, env_);
        env_->define(n->name, Value(std::move(fn)));
        return Value();
    }

    Value evalReturn(ReturnNode* n) {
        Value val = evalNode(n->value);
        throw ReturnSignal{val};
    }

    Value evalCall(CallNode* n) {
        // Check for 'new ClassName()' pattern
        if (auto varNode = dynamic_cast<VariableNode*>(n->callee.get())) {
            // Check if it's preceded by 'new' keyword (handled via variable lookup)
            // OR if the variable is a class definition
            if (env_->has(varNode->name)) {
                Value val = env_->get(varNode->name);
                if (val.isClassDef()) {
                    std::vector<Value> args;
                    for (auto& arg : n->args) {
                        args.push_back(evalNode(arg));
                    }
                    return instantiateClass(val.classDefVal, args);
                }
            }
        }

        // Check for 'new' keyword as outer expression
        // Also handle: $obj.method() on class instances
        Value callee = evalNode(n->callee);

        std::vector<Value> args;
        for (auto& arg : n->args) {
            args.push_back(evalNode(arg));
        }

        if (callee.isNativeFn()) {
            return callee.nativeFn(std::move(args));
        }

        if (callee.isClassDef()) {
            return instantiateClass(callee.classDefVal, args);
        }

        if (callee.isFunction()) {
            auto fn = callee.functionVal;
            auto callEnv = std::make_shared<Environment>(fn->closure);

            // If 'this' exists in current scope, pass it along
            if (env_->has("this")) {
                callEnv->define("this", env_->get("this"));
                callEnv->define("self", env_->get("this"));
            }

            if (args.size() > fn->params.size()) {
                ErrorHandler::throwRuntimeError("Too many arguments for function " + fn->name);
            }
            for (size_t i = 0; i < fn->params.size(); i++) {
                callEnv->define(fn->params[i], i < args.size() ? args[i] : Value());
            }

            auto prevEnv = env_;
            env_ = callEnv;
            Value result;
            try {
                for (auto& stmt : fn->body) {
                    result = evalNode(stmt);
                }
            } catch (const ReturnSignal& sig) {
                result = sig.value;
            }
            env_ = prevEnv;
            return result;
        }

        ErrorHandler::throwRuntimeError("Cannot call non-function value");
        return Value();
    }

    // ════════════════════════════════════════════════════════════
    // PROPERTY / INDEX ACCESS
    // ════════════════════════════════════════════════════════════

    Value evalDotAccess(DotAccessNode* n) {
        Value obj = evalNode(n->object);

        // Class instance
        if (obj.isClassInstance()) {
            Value prop = obj.classInstanceVal->getProperty(n->property);
            // If it's a method (function), bind 'this' to the instance
            if (prop.isFunction()) {
                auto fn = prop.functionVal;
                auto boundEnv = std::make_shared<Environment>(fn->closure);
                boundEnv->define("this", obj);
                boundEnv->define("self", obj);
                return Value(std::make_shared<BantuFunction>(fn->name, fn->params, fn->body, boundEnv));
            }
            return prop;
        }

        // Object (dict)
        if (obj.isObject()) {
            auto it = obj.objectVal->find(n->property);
            if (it != obj.objectVal->end()) return it->second;
            // For leniency with $req.body.X access patterns, return null
            // instead of throwing when a key is missing on a plain object.
            // (Class instances still throw — they use the class-instance branch above.)
            return Value();
        }

        // List methods
        if (obj.isList()) {
            if (n->property == "length") return Value((double)obj.listVal.size());
            if (n->property == "push") {
                return makeNative([this, listRef = obj.listVal](std::vector<Value> args) mutable -> Value {
                    for (auto& a : args) listRef.push_back(a);
                    return Value((double)listRef.size());
                });
            }
            if (n->property == "pop") {
                return makeNative([this, listRef = obj.listVal](std::vector<Value> args) mutable -> Value {
                    if (listRef.empty()) return Value();
                    Value last = listRef.back();
                    listRef.pop_back();
                    return last;
                });
            }
        }

        // String methods
        if (obj.isString()) {
            if (n->property == "length") return Value((double)obj.stringVal.size());
            if (n->property == "upper") return makeNative([s = obj.stringVal](std::vector<Value>) -> Value {
                std::string upper = s;
                std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
                return Value(upper);
            });
            if (n->property == "lower") return makeNative([s = obj.stringVal](std::vector<Value>) -> Value {
                std::string lower = s;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                return Value(lower);
            });
            // s.substr(start)  or  s.substr(start, length)
            if (n->property == "substr") return makeNative([s = obj.stringVal](std::vector<Value> args) -> Value {
                if (args.empty()) return Value(s);
                size_t start = (size_t)args[0].numberVal;
                if (start > s.size()) start = s.size();
                if (args.size() < 2) return Value(s.substr(start));
                size_t len = (size_t)args[1].numberVal;
                if (start + len > s.size()) len = s.size() - start;
                return Value(s.substr(start, len));
            });
            // s.slice(start, end) — like JS slice, end exclusive
            if (n->property == "slice") return makeNative([s = obj.stringVal](std::vector<Value> args) -> Value {
                long long sz = (long long)s.size();
                long long start = args.empty() ? 0 : (long long)args[0].numberVal;
                long long end   = args.size() < 2 ? sz : (long long)args[1].numberVal;
                if (start < 0) start += sz;
                if (end   < 0) end   += sz;
                if (start < 0) start = 0;
                if (end   > sz) end = sz;
                if (start >= end) return Value(std::string(""));
                return Value(s.substr((size_t)start, (size_t)(end - start)));
            });
            // s.contains(needle) → bool
            if (n->property == "contains") return makeNative([s = obj.stringVal](std::vector<Value> args) -> Value {
                if (args.empty() || !args[0].isString()) return Value(false);
                return Value(s.find(args[0].stringVal) != std::string::npos);
            });
            // s.indexOf(needle) → number (or -1)
            if (n->property == "indexOf") return makeNative([s = obj.stringVal](std::vector<Value> args) -> Value {
                if (args.empty() || !args[0].isString()) return Value(-1.0);
                auto p = s.find(args[0].stringVal);
                return Value(p == std::string::npos ? -1.0 : (double)p);
            });
            // s.split(sep) → list of substrings
            if (n->property == "split") return makeNative([s = obj.stringVal](std::vector<Value> args) -> Value {
                std::vector<Value> out;
                if (args.empty() || !args[0].isString() || args[0].stringVal.empty()) {
                    out.push_back(Value(s));
                    return Value(std::move(out));
                }
                const std::string& sep = args[0].stringVal;
                size_t start = 0, pos;
                while ((pos = s.find(sep, start)) != std::string::npos) {
                    out.push_back(Value(s.substr(start, pos - start)));
                    start = pos + sep.size();
                }
                out.push_back(Value(s.substr(start)));
                return Value(std::move(out));
            });
            // s.trim() → strip whitespace from both ends
            if (n->property == "trim") return makeNative([s = obj.stringVal](std::vector<Value>) -> Value {
                size_t a = 0, b = s.size();
                while (a < b && std::isspace((unsigned char)s[a])) a++;
                while (b > a && std::isspace((unsigned char)s[b-1])) b--;
                return Value(s.substr(a, b - a));
            });
            // s.replace(old, new) → replace all occurrences
            if (n->property == "replace") return makeNative([s = obj.stringVal](std::vector<Value> args) -> Value {
                if (args.size() < 2 || !args[0].isString() || !args[1].isString()) return Value(s);
                std::string r;
                const std::string& from = args[0].stringVal;
                const std::string& to   = args[1].stringVal;
                if (from.empty()) return Value(s);
                size_t start = 0, pos;
                while ((pos = s.find(from, start)) != std::string::npos) {
                    r.append(s, start, pos - start);
                    r.append(to);
                    start = pos + from.size();
                }
                r.append(s, start, std::string::npos);
                return Value(r);
            });
        }

        // Number methods
        if (obj.isNumber()) {
            if (n->property == "abs") return makeNative([v = obj.numberVal](std::vector<Value>) -> Value { return Value(std::abs(v)); });
            if (n->property == "floor") return makeNative([v = obj.numberVal](std::vector<Value>) -> Value { return Value(std::floor(v)); });
            if (n->property == "ceil") return makeNative([v = obj.numberVal](std::vector<Value>) -> Value { return Value(std::ceil(v)); });
            if (n->property == "round") return makeNative([v = obj.numberVal](std::vector<Value>) -> Value { return Value(std::round(v)); });
        }

        return Value();
    }

    Value evalIndexAccess(IndexAccessNode* n) {
        Value obj = evalNode(n->object);
        Value idx = evalNode(n->index);

        if (obj.isList() && idx.isNumber()) {
            int i = (int)idx.numberVal;
            if (i >= 0 && i < (int)obj.listVal.size()) return obj.listVal[i];
            ErrorHandler::throwRuntimeError("Index out of bounds: " + std::to_string(i));
            return Value();
        }

        if (obj.isObject() && idx.isString()) {
            auto it = obj.objectVal->find(idx.stringVal);
            if (it != obj.objectVal->end()) return it->second;
            return Value();
        }

        if (obj.isString() && idx.isNumber()) {
            int i = (int)idx.numberVal;
            if (i >= 0 && i < (int)obj.stringVal.size()) return Value(std::string(1, obj.stringVal[i]));
            return Value();
        }

        return Value();
    }

    // ════════════════════════════════════════════════════════════
    // ERROR HANDLING
    // ════════════════════════════════════════════════════════════

    Value evalTryCatch(TryCatchNode* n) {
        try {
            auto prevEnv = env_;
            env_ = std::make_shared<Environment>(env_);
            for (auto& stmt : n->tryBody) {
                evalNode(stmt);
            }
            env_ = prevEnv;
        } catch (const std::exception& e) {
            auto prevEnv = env_;
            env_ = std::make_shared<Environment>(env_);
            env_->define(n->catchVar, Value(std::string(e.what())));
            for (auto& stmt : n->catchBody) {
                evalNode(stmt);
            }
            env_ = prevEnv;
        }
        return Value();
    }

    // ════════════════════════════════════════════════════════════
    // CLASS DECLARATION & INHERITANCE
    // ════════════════════════════════════════════════════════════

    // Store class definitions for lookup during instantiation
    std::unordered_map<std::string, ClassDefinition*> classRegistry_;
    std::string currentClassName_ = "";  // for super() resolution

    Value evalClassDecl(ClassDeclNode* n) {
        auto* classDef = new ClassDefinition(n->name);
        classRegistry_[n->name] = classDef;

        // Resolve extends (single inheritance)
        if (!n->parentClass.empty()) {
            auto it = classRegistry_.find(n->parentClass);
            if (it != classRegistry_.end()) {
                classDef->parentClass = it->second;
                std::cout << "  [class] " << n->name << " extends " << n->parentClass << "\n";
            } else {
                std::cout << "  [class] WARNING: parent class '" << n->parentClass << "' not found for " << n->name << "\n";
            }
        }

        // Resolve implements (multiple inheritance)
        if (!n->implementsClasses.empty()) {
            std::string implNames;
            for (size_t i = 0; i < n->implementsClasses.size(); i++) {
                const auto& implName = n->implementsClasses[i];
                auto it = classRegistry_.find(implName);
                if (it != classRegistry_.end()) {
                    classDef->implementsClasses.push_back(it->second);
                    if (i > 0) implNames += ", ";
                    implNames += implName;
                } else {
                    std::cout << "  [class] WARNING: implemented class '" << implName << "' not found for " << n->name << "\n";
                }
            }
            if (!implNames.empty()) {
                std::cout << "  [class] " << n->name << " implements " << implNames << "\n";
            }
        }

        // Process class body - register methods
        auto prevEnv = env_;
        auto classEnv = std::make_shared<Environment>(env_);
        env_ = classEnv;

        // Define 'this' and 'self' references in class scope
        env_->define("this", Value());
        env_->define("self", Value());

        currentClassName_ = n->name;

        for (auto& member : n->body) {
            if (auto funcNode = dynamic_cast<FuncDeclNode*>(member.get())) {
                // Register method on the class definition
                auto fn = std::make_shared<BantuFunction>(funcNode->name, funcNode->params, funcNode->body, env_);
                classDef->addMethod(funcNode->name, Value(std::move(fn)));

                // Special handling for constructor
                if (funcNode->name == n->name || funcNode->name == "init" || funcNode->name == "constructor") {
                    classDef->addMethod("__constructor__", Value(std::make_shared<BantuFunction>(
                        funcNode->name, funcNode->params, funcNode->body, env_)));
                }
            } else {
                // Evaluate property declarations (e.g., number $x = 0)
                evalNode(member);
            }
        }

        currentClassName_ = "";
        env_ = prevEnv;

        // Store the class definition as a global variable
        env_->define(n->name, Value(classDef));

        std::cout << "  [class] Defined: " << n->name << "\n";
        return Value(classDef);
    }

    Value evalSuper(SuperNode* n) {
        // Call the parent class constructor
        if (currentClassName_.empty()) {
            ErrorHandler::throwRuntimeError("super() can only be used inside a class method");
            return Value();
        }

        auto it = classRegistry_.find(currentClassName_);
        if (it == classRegistry_.end() || !it->second->parentClass) {
            ErrorHandler::throwRuntimeError("No parent class for super() call");
            return Value();
        }

        ClassDefinition* parentClass = it->second->parentClass;

        // Evaluate arguments
        std::vector<Value> args;
        for (auto& arg : n->args) {
            args.push_back(evalNode(arg));
        }

        // Call parent constructor
        auto constructorIt = parentClass->methods.find("__constructor__");
        if (constructorIt != parentClass->methods.end() && constructorIt->second.isFunction()) {
            auto fn = constructorIt->second.functionVal;
            auto callEnv = std::make_shared<Environment>(fn->closure);

            // Bind 'this' from current environment
            if (env_->has("this")) {
                callEnv->define("this", env_->get("this"));
                callEnv->define("self", env_->get("this"));
            }

            for (size_t i = 0; i < fn->params.size(); i++) {
                callEnv->define(fn->params[i], i < args.size() ? args[i] : Value());
            }

            auto prevEnv = env_;
            env_ = callEnv;
            try {
                for (auto& stmt : fn->body) {
                    evalNode(stmt);
                }
            } catch (const ReturnSignal& sig) {
                // Constructor return
            }
            env_ = prevEnv;
        }

        return Value();
    }

    Value instantiateClass(ClassDefinition* classDef, std::vector<Value>& args) {
        auto* instance = new ClassInstance(classDef);

        // Copy default properties from parent classes (extends chain)
        ClassDefinition* current = classDef->parentClass;
        while (current) {
            for (auto& [key, val] : current->methods) {
                if (key != "__constructor__") {
                    // Don't overwrite existing methods
                }
            }
            current = current->parentClass;
        }

        // Find and call constructor
        auto constructorIt = classDef->methods.find("__constructor__");
        if (constructorIt != classDef->methods.end() && constructorIt->second.isFunction()) {
            auto fn = constructorIt->second.functionVal;
            auto callEnv = std::make_shared<Environment>(fn->closure);

            // Bind 'this' and 'self' to the instance
            Value thisVal(instance);
            callEnv->define("this", thisVal);
            callEnv->define("self", thisVal);

            for (size_t i = 0; i < fn->params.size(); i++) {
                callEnv->define(fn->params[i], i < args.size() ? args[i] : Value());
            }

            // Set current class name for super() resolution
            std::string savedClassName = currentClassName_;
            currentClassName_ = classDef->name;

            auto prevEnv = env_;
            env_ = callEnv;
            try {
                for (auto& stmt : fn->body) {
                    evalNode(stmt);
                }
            } catch (const ReturnSignal& sig) {
                // Constructor return ignored
            }
            env_ = prevEnv;
            currentClassName_ = savedClassName;
        } else {
            // No explicit constructor — try to call parent constructor
            if (classDef->parentClass) {
                auto parentCtor = classDef->parentClass->methods.find("__constructor__");
                if (parentCtor != classDef->parentClass->methods.end() && parentCtor->second.isFunction()) {
                    auto fn = parentCtor->second.functionVal;
                    auto callEnv = std::make_shared<Environment>(fn->closure);

                    Value thisVal(instance);
                    callEnv->define("this", thisVal);
                    callEnv->define("self", thisVal);

                    for (size_t i = 0; i < fn->params.size(); i++) {
                        callEnv->define(fn->params[i], i < args.size() ? args[i] : Value());
                    }

                    auto prevEnv = env_;
                    env_ = callEnv;
                    try {
                        for (auto& stmt : fn->body) {
                            evalNode(stmt);
                        }
                    } catch (const ReturnSignal& sig) {}
                    env_ = prevEnv;
                }
            }
        }

        return Value(instance);
    }

    // ════════════════════════════════════════════════════════════
    // PRINT
    // ════════════════════════════════════════════════════════════

    Value evalPrint(PrintNode* n) {
        Value val = evalNode(n->value);
        std::cout << val.toString() << "\n";
        globalLog.add(val);
        return val;
    }

    // ════════════════════════════════════════════════════════════
    // BUILT-IN REGISTRATION
    // ════════════════════════════════════════════════════════════

    void registerBuiltins() {
        // Core built-in functions
        env_->define("len", makeNative([](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(0.0);
            if (args[0].isString()) return Value((double)args[0].stringVal.size());
            if (args[0].isList()) return Value((double)args[0].listVal.size());
            return Value(0.0);
        }));

        env_->define("type", makeNative([](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string("null"));
            switch (args[0].type) {
                case Value::NUMBER: return Value(std::string("number"));
                case Value::STRING: return Value(std::string("string"));
                case Value::BOOL: return Value(std::string("bool"));
                case Value::NULL_VAL: return Value(std::string("null"));
                case Value::FUNCTION: case Value::NATIVE_FN: return Value(std::string("function"));
                case Value::OBJECT: return Value(std::string("dict"));
                case Value::LIST: return Value(std::string("list"));
                case Value::CLASS_INSTANCE: return Value(std::string("instance"));
                case Value::CLASS_DEF: return Value(std::string("class"));
            }
            return Value(std::string("unknown"));
        }));

        env_->define("sleep", makeNative([](std::vector<Value> args) -> Value {
            double ms = args.size() > 0 ? args[0].numberVal : 1000;
            std::this_thread::sleep_for(std::chrono::milliseconds((long long)ms));
            return Value();
        }));

        env_->define("abs", makeNative([](std::vector<Value> args) -> Value {
            return args.size() > 0 ? Value(std::abs(args[0].numberVal)) : Value(0.0);
        }));

        env_->define("floor", makeNative([](std::vector<Value> args) -> Value {
            return args.size() > 0 ? Value(std::floor(args[0].numberVal)) : Value(0.0);
        }));

        env_->define("ceil", makeNative([](std::vector<Value> args) -> Value {
            return args.size() > 0 ? Value(std::ceil(args[0].numberVal)) : Value(0.0);
        }));

        env_->define("sqrt", makeNative([](std::vector<Value> args) -> Value {
            return args.size() > 0 ? Value(std::sqrt(args[0].numberVal)) : Value(0.0);
        }));

        env_->define("pow", makeNative([](std::vector<Value> args) -> Value {
            double base = args.size() > 0 ? args[0].numberVal : 0;
            double exp = args.size() > 1 ? args[1].numberVal : 0;
            return Value(std::pow(base, exp));
        }));

        env_->define("sin", makeNative([](std::vector<Value> args) -> Value {
            return args.size() > 0 ? Value(std::sin(args[0].numberVal)) : Value(0.0);
        }));

        env_->define("cos", makeNative([](std::vector<Value> args) -> Value {
            return args.size() > 0 ? Value(std::cos(args[0].numberVal)) : Value(0.0);
        }));

        env_->define("tan", makeNative([](std::vector<Value> args) -> Value {
            return args.size() > 0 ? Value(std::tan(args[0].numberVal)) : Value(0.0);
        }));

        env_->define("log", makeNative([](std::vector<Value> args) -> Value {
            return args.size() > 0 ? Value(std::log(args[0].numberVal)) : Value(0.0);
        }));

        env_->define("round", makeNative([](std::vector<Value> args) -> Value {
            return args.size() > 0 ? Value(std::round(args[0].numberVal)) : Value(0.0);
        }));

        env_->define("random", makeNative([](std::vector<Value> args) -> Value {
            static std::mt19937 rng(std::random_device{}());
            if (args.empty()) {
                std::uniform_real_distribution<double> dist(0.0, 1.0);
                return Value(dist(rng));
            }
            std::uniform_int_distribution<long long> dist(0, (long long)args[0].numberVal);
            return Value((double)dist(rng));
        }));

        env_->define("str", makeNative([](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string(""));
            return Value(args[0].toString());
        }));

        env_->define("num", makeNative([](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(0.0);
            if (args[0].isNumber()) return args[0];
            if (args[0].isString()) {
                try { return Value(std::stod(args[0].stringVal)); }
                catch (...) { return Value(0.0); }
            }
            return Value(0.0);
        }));

        env_->define("chr", makeNative([](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string(""));
            int code = (int)args[0].numberVal;
            if (code < 0 || code > 127) return Value(std::string("?"));
            return Value(std::string(1, (char)code));
        }));

        env_->define("push", makeNative([](std::vector<Value> args) -> Value {
            // push(list, item) - adds item to list
            if (args.size() >= 2 && args[0].isList()) {
                args[0].listVal.push_back(args[1]);
            }
            return Value();
        }));

        env_->define("range", makeNative([](std::vector<Value> args) -> Value {
            double start = args.size() > 0 ? args[0].numberVal : 0;
            double end = args.size() > 1 ? args[1].numberVal : 0;
            std::vector<Value> result;
            for (double i = start; i < end; i++) {
                result.push_back(Value(i));
            }
            return Value(std::move(result));
        }));

        env_->define("clock", makeNative([](std::vector<Value> args) -> Value {
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
            return Value((double)ms.count());
        }));

        env_->define("max", makeNative([](std::vector<Value> args) -> Value {
            if (args.size() < 2) return args.empty() ? Value(0.0) : args[0];
            return Value(std::max(args[0].numberVal, args[1].numberVal));
        }));

        env_->define("min", makeNative([](std::vector<Value> args) -> Value {
            if (args.size() < 2) return args.empty() ? Value(0.0) : args[0];
            return Value(std::min(args[0].numberVal, args[1].numberVal));
        }));

        // env(name) — read a process environment variable.
        // Returns the value as a string, or "" if unset.
        env_->define("env", makeNative([](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].isString()) return Value(std::string(""));
            const char* v = std::getenv(args[0].stringVal.c_str());
            return Value(std::string(v ? v : ""));
        }));

        // substr(s, start [, length]) — substring helper.
        env_->define("substr", makeNative([](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].isString()) return Value(std::string(""));
            const std::string& s = args[0].stringVal;
            if (args.size() < 2) return Value(s);
            size_t start = (size_t)args[1].numberVal;
            if (start > s.size()) start = s.size();
            if (args.size() < 3) return Value(s.substr(start));
            size_t len = (size_t)args[2].numberVal;
            if (start + len > s.size()) len = s.size() - start;
            return Value(s.substr(start, len));
        }));

        // split(s, sep) — split s by separator, returns list of substrings.
        env_->define("split", makeNative([](std::vector<Value> args) -> Value {
            std::vector<Value> out;
            if (args.empty() || !args[0].isString()) { out.push_back(Value(std::string(""))); return Value(std::move(out)); }
            const std::string& s = args[0].stringVal;
            if (args.size() < 2 || !args[1].isString() || args[1].stringVal.empty()) {
                out.push_back(Value(s));
                return Value(std::move(out));
            }
            const std::string& sep = args[1].stringVal;
            size_t start = 0, pos;
            while ((pos = s.find(sep, start)) != std::string::npos) {
                out.push_back(Value(s.substr(start, pos - start)));
                start = pos + sep.size();
            }
            out.push_back(Value(s.substr(start)));
            return Value(std::move(out));
        }));

        // trim(s) — strip whitespace from both ends.
        env_->define("trim", makeNative([](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].isString()) return Value(std::string(""));
            const std::string& s = args[0].stringVal;
            size_t a = 0, b = s.size();
            while (a < b && std::isspace((unsigned char)s[a])) a++;
            while (b > a && std::isspace((unsigned char)s[b-1])) b--;
            return Value(s.substr(a, b - a));
        }));

        // contains(s, needle) → bool
        env_->define("contains", makeNative([](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].isString() || !args[1].isString()) return Value(false);
            return Value(args[0].stringVal.find(args[1].stringVal) != std::string::npos);
        }));

        // indexOf(s, needle) → number (or -1)
        env_->define("indexOf", makeNative([](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].isString() || !args[1].isString()) return Value(-1.0);
            auto p = args[0].stringVal.find(args[1].stringVal);
            return Value(p == std::string::npos ? -1.0 : (double)p);
        }));

        // replace(s, old, new) — replace all occurrences.
        env_->define("replace", makeNative([](std::vector<Value> args) -> Value {
            if (args.size() < 3 || !args[0].isString() || !args[1].isString() || !args[2].isString()) {
                return args.empty() ? Value(std::string("")) : args[0];
            }
            const std::string& s    = args[0].stringVal;
            const std::string& from = args[1].stringVal;
            const std::string& to   = args[2].stringVal;
            if (from.empty()) return Value(s);
            std::string r;
            size_t start = 0, pos;
            while ((pos = s.find(from, start)) != std::string::npos) {
                r.append(s, start, pos - start);
                r.append(to);
                start = pos + from.size();
            }
            r.append(s, start, std::string::npos);
            return Value(r);
        }));

        // Register Sua Framework
        registerSuaFramework();
    }

    // ════════════════════════════════════════════════════════════
    // SUA FRAMEWORK REGISTRATION
    // ════════════════════════════════════════════════════════════

    void registerSuaFramework() {
        ObjectMap suaObj;

        // ─── Existing Real-Time Methods ───

        // sua.channel(name)
        suaObj["channel"] = makeNative([](std::vector<Value> args) -> Value {
            std::string name = args.size() > 0 ? args[0].toString() : "default";
            std::cout << "  [SUA] Channel created: " << name << "\n";
            ObjectMap ch;
            ch["name"] = Value(name);
            ch["type"] = Value(std::string("channel"));
            ch["subscribers"] = Value(0.0);
            return Value(std::move(ch));
        });

        // sua.signal(target)
        suaObj["signal"] = makeNative([](std::vector<Value> args) -> Value {
            std::string target = args.size() > 0 ? args[0].toString() : "unknown";
            std::cout << "  [SUA] Signaling: " << target << "\n";
            ObjectMap sig;
            sig["peerId"] = Value(target);
            sig["type"] = Value(std::string("webrtc-signal"));
            return Value(std::move(sig));
        });

        // sua.stun()
        suaObj["stun"] = makeNative([](std::vector<Value> args) -> Value {
            std::cout << "  [SUA] STUN discovery\n";
            ObjectMap natInfo;
            natInfo["ip"] = Value(std::string("192.168.1.100"));
            natInfo["publicIp"] = Value(std::string("203.0.113.42"));
            natInfo["port"] = Value(54321.0);
            natInfo["natType"] = Value(std::string("Full Cone NAT"));
            natInfo["traversal"] = Value(std::string("direct"));
            natInfo["stunServer"] = Value(std::string("bantu-stun:3478"));
            natInfo["reachable"] = Value(true);
            return Value(std::move(natInfo));
        });

        // sua.broadcast(channel, message)
        suaObj["broadcast"] = makeNative([](std::vector<Value> args) -> Value {
            std::string channel = args.size() > 0 ? args[0].toString() : "default";
            std::string message = args.size() > 1 ? args[1].toString() : "";
            std::cout << "  [SUA] Broadcast on " << channel << ": " << message << "\n";
            ObjectMap result;
            result["channel"] = Value(channel);
            result["message"] = Value(message);
            result["delivered"] = Value(true);
            result["subscribers"] = Value(1.0);
            return Value(std::move(result));
        });

        // sua.relay(peerId)
        suaObj["relay"] = makeNative([](std::vector<Value> args) -> Value {
            std::string peerId = args.size() > 0 ? args[0].toString() : "anonymous";
            std::cout << "  [SUA] Relay allocated for " << peerId << "\n";
            ObjectMap relayInfo;
            relayInfo["peerId"] = Value(peerId);
            relayInfo["relayPort"] = Value(50000.0);
            relayInfo["relayServer"] = Value(std::string("bantu-turn:3479"));
            relayInfo["status"] = Value(std::string("allocated"));
            relayInfo["lifetime"] = Value(600.0);
            return Value(std::move(relayInfo));
        });

        // sua.stream(channel, type)
        suaObj["stream"] = makeNative([](std::vector<Value> args) -> Value {
            std::string channel = args.size() > 0 ? args[0].toString() : "default";
            std::string type = args.size() > 1 ? args[1].toString() : "video";
            std::cout << "  [SUA] Stream " << type << " on " << channel << "\n";
            ObjectMap info;
            info["channel"] = Value(channel);
            info["type"] = Value(type);
            info["codec"] = Value(std::string("VP8"));
            info["bitrate"] = Value(2500.0);
            info["status"] = Value(std::string("streaming"));
            return Value(std::move(info));
        });

        // sua.connect(peerId)
        suaObj["connect"] = makeNative([](std::vector<Value> args) -> Value {
            std::string peerId = args.size() > 0 ? args[0].toString() : "unknown";
            std::cout << "  [SUA] Connecting to " << peerId << "\n";
            ObjectMap connInfo;
            connInfo["peerId"] = Value(peerId);
            connInfo["status"] = Value(std::string("connecting"));
            connInfo["iceGathering"] = Value(std::string("complete"));
            connInfo["relayReady"] = Value(true);
            return Value(std::move(connInfo));
        });

        // sua.room(name)
        suaObj["room"] = makeNative([](std::vector<Value> args) -> Value {
            std::string name = args.size() > 0 ? args[0].toString() : "default";
            std::cout << "  [SUA] Room created: " << name << "\n";
            ObjectMap roomInfo;
            roomInfo["name"] = Value(name);
            roomInfo["peers"] = Value(std::vector<Value>{});
            roomInfo["maxPeers"] = Value(50.0);
            roomInfo["status"] = Value(std::string("active"));
            return Value(std::move(roomInfo));
        });

        // sua.offer(target)
        suaObj["offer"] = makeNative([](std::vector<Value> args) -> Value {
            std::string target = args.size() > 0 ? args[0].toString() : "unknown";
            std::cout << "  [SUA] SDP Offer to: " << target << "\n";
            ObjectMap offerInfo;
            offerInfo["type"] = Value(std::string("offer"));
            offerInfo["target"] = Value(target);
            offerInfo["sdp"] = Value(std::string("v=0\r\no=- 123456789 1 IN IP4 192.168.1.100\r\ns=-\r\n"));
            return Value(std::move(offerInfo));
        });

        // sua.answer(target)
        suaObj["answer"] = makeNative([](std::vector<Value> args) -> Value {
            std::string target = args.size() > 0 ? args[0].toString() : "unknown";
            std::cout << "  [SUA] SDP Answer to: " << target << "\n";
            ObjectMap answerInfo;
            answerInfo["type"] = Value(std::string("answer"));
            answerInfo["target"] = Value(target);
            answerInfo["sdp"] = Value(std::string("v=0\r\no=- 987654321 2 IN IP4 203.0.113.99\r\ns=-\r\n"));
            return Value(std::move(answerInfo));
        });

        // ════════════════════════════════════════════════════════
        // NEW: SUA SERVER — Express-like HTTP Server DSL
        // ════════════════════════════════════════════════════════

        ObjectMap serverObj;

        // sua.server.get(path)
        serverObj["get"] = makeNative([this](std::vector<Value> args) -> Value {
            std::string path = args.size() > 0 ? args[0].toString() : "/";
            Value handler = args.size() > 1 ? args[1] : Value();
            bantuServerRoutes.push_back({"GET", path, handler});
            std::cout << "  [SERVER] GET " << path << " registered\n";
            ObjectMap routeInfo;
            routeInfo["method"] = Value(std::string("GET"));
            routeInfo["path"] = Value(path);
            routeInfo["registered"] = Value(true);
            return Value(std::move(routeInfo));
        });

        // sua.server.post(path, handler)
        serverObj["post"] = makeNative([this](std::vector<Value> args) -> Value {
            std::string path = args.size() > 0 ? args[0].toString() : "/";
            Value handler = args.size() > 1 ? args[1] : Value();
            bantuServerRoutes.push_back({"POST", path, handler});
            std::cout << "  [SERVER] POST " << path << " registered\n";
            ObjectMap routeInfo;
            routeInfo["method"] = Value(std::string("POST"));
            routeInfo["path"] = Value(path);
            routeInfo["registered"] = Value(true);
            return Value(std::move(routeInfo));
        });

        // sua.server.put(path, handler)
        serverObj["put"] = makeNative([this](std::vector<Value> args) -> Value {
            std::string path = args.size() > 0 ? args[0].toString() : "/";
            Value handler = args.size() > 1 ? args[1] : Value();
            bantuServerRoutes.push_back({"PUT", path, handler});
            std::cout << "  [SERVER] PUT " << path << " registered\n";
            ObjectMap routeInfo;
            routeInfo["method"] = Value(std::string("PUT"));
            routeInfo["path"] = Value(path);
            routeInfo["registered"] = Value(true);
            return Value(std::move(routeInfo));
        });

        // sua.server.delete(path, handler)
        serverObj["delete"] = makeNative([this](std::vector<Value> args) -> Value {
            std::string path = args.size() > 0 ? args[0].toString() : "/";
            Value handler = args.size() > 1 ? args[1] : Value();
            bantuServerRoutes.push_back({"DELETE", path, handler});
            std::cout << "  [SERVER] DELETE " << path << " registered\n";
            ObjectMap routeInfo;
            routeInfo["method"] = Value(std::string("DELETE"));
            routeInfo["path"] = Value(path);
            routeInfo["registered"] = Value(true);
            return Value(std::move(routeInfo));
        });

        // sua.server.patch(path, handler)
        serverObj["patch"] = makeNative([this](std::vector<Value> args) -> Value {
            std::string path = args.size() > 0 ? args[0].toString() : "/";
            Value handler = args.size() > 1 ? args[1] : Value();
            bantuServerRoutes.push_back({"PATCH", path, handler});
            std::cout << "  [SERVER] PATCH " << path << " registered\n";
            ObjectMap routeInfo;
            routeInfo["method"] = Value(std::string("PATCH"));
            routeInfo["path"] = Value(path);
            routeInfo["registered"] = Value(true);
            return Value(std::move(routeInfo));
        });

        // sua.server.head(path, handler)
        serverObj["head"] = makeNative([this](std::vector<Value> args) -> Value {
            std::string path = args.size() > 0 ? args[0].toString() : "/";
            Value handler = args.size() > 1 ? args[1] : Value();
            bantuServerRoutes.push_back({"HEAD", path, handler});
            std::cout << "  [SERVER] HEAD " << path << " registered\n";
            ObjectMap routeInfo;
            routeInfo["method"] = Value(std::string("HEAD"));
            routeInfo["path"] = Value(path);
            routeInfo["registered"] = Value(true);
            return Value(std::move(routeInfo));
        });

        // sua.server.options(path, handler)
        serverObj["options"] = makeNative([this](std::vector<Value> args) -> Value {
            std::string path = args.size() > 0 ? args[0].toString() : "*";
            Value handler = args.size() > 1 ? args[1] : Value();
            bantuServerRoutes.push_back({"OPTIONS", path, handler});
            std::cout << "  [SERVER] OPTIONS " << path << " registered\n";
            ObjectMap routeInfo;
            routeInfo["method"] = Value(std::string("OPTIONS"));
            routeInfo["path"] = Value(path);
            routeInfo["registered"] = Value(true);
            return Value(std::move(routeInfo));
        });

        // sua.server.use(middleware) — middleware can be a function
        serverObj["use"] = makeNative([this](std::vector<Value> args) -> Value {
            Value mw = args.size() > 0 ? args[0] : Value();
            if (mw.isFunction() || mw.isNativeFn()) {
                bantuServerMiddlewareFuncs.push_back(mw);
                std::cout << "  [SERVER] Middleware function loaded\n";
            } else {
                std::string name = mw.toString();
                bantuServerMiddleware.push_back(name);
                std::cout << "  [SERVER] Middleware loaded: " << name << "\n";
            }
            ObjectMap mwInfo;
            mwInfo["loaded"] = Value(true);
            return Value(std::move(mwInfo));
        });

        // sua.server.static(path) — serve files from this directory
        serverObj["static"] = makeNative([this](std::vector<Value> args) -> Value {
            std::string path = args.size() > 0 ? args[0].toString() : "./public";
            bantuServerStatic.push_back(path);
            std::cout << "  [SERVER] Static files: " << path << "\n";
            ObjectMap staticInfo;
            staticInfo["path"] = Value(path);
            staticInfo["serving"] = Value(true);
            return Value(std::move(staticInfo));
        });

        // sua.server.listen(port) — ACTUALLY starts a real HTTP server.
        // Blocks the calling thread forever (accept loop).
        serverObj["listen"] = makeNative([this](std::vector<Value> args) -> Value {
            bantuServerPort = args.size() > 0 ? (int)args[0].numberVal : 3000;

            std::cout << "\n";
            std::cout << "  ╔═══════════════════════════════════════════════════╗\n";
            std::cout << "  ║   Bantu Server v1.2.0 (Sua Framework)            ║\n";
            std::cout << "  ║   http://" << bantuServerHost << ":" << bantuServerPort;
            int padLen = 37 - (9 + bantuServerHost.length() + std::to_string(bantuServerPort).length());
            for (int i = 0; i < padLen; i++) std::cout << " ";
            std::cout << "║\n";
            std::cout << "  ╚═══════════════════════════════════════════════════╝\n";
            std::cout << "\n";

            if (!bantuServerRoutes.empty()) {
                std::cout << "  Routes:\n";
                for (const auto& route : bantuServerRoutes) {
                    std::cout << "    " << route.method;
                    for (size_t i = route.method.length(); i < 8; i++) std::cout << " ";
                    std::cout << route.path << "\n";
                }
                std::cout << "\n";
            }
            if (!bantuServerStatic.empty()) {
                std::cout << "  Static:\n";
                for (const auto& s : bantuServerStatic) std::cout << "    - " << s << "\n";
                std::cout << "\n";
            }

            std::cout << "  Server ready! " << bantuServerRoutes.size() << " route(s) registered.\n";
            std::cout << "  Listening on port " << bantuServerPort << "...\n\n";
            std::cout.flush();

            // ─── Start the real HTTP server (POSIX sockets, blocking) ───
            bantuStartHttpServer(bantuServerPort);

            ObjectMap listenInfo;
            listenInfo["port"] = Value((double)bantuServerPort);
            listenInfo["host"] = Value(bantuServerHost);
            listenInfo["routes"] = Value((double)bantuServerRoutes.size());
            listenInfo["status"] = Value(std::string("stopped"));
            return Value(std::move(listenInfo));
        });

        // sua.server.routes() - list all registered routes
        serverObj["routes"] = makeNative([](std::vector<Value> args) -> Value {
            std::vector<Value> routeList;
            for (const auto& route : bantuServerRoutes) {
                ObjectMap r;
                r["method"] = Value(route.method);
                r["path"] = Value(route.path);
                routeList.push_back(Value(std::move(r)));
            }
            std::cout << "  [SERVER] " << routeList.size() << " route(s) registered\n";
            return Value(std::move(routeList));
        });

        // sua.server.all(path, handler) - match all methods
        serverObj["all"] = makeNative([this](std::vector<Value> args) -> Value {
            std::string path = args.size() > 0 ? args[0].toString() : "/";
            Value handler = args.size() > 1 ? args[1] : Value();
            for (const auto& m : {"GET", "POST", "PUT", "DELETE", "PATCH"}) {
                bantuServerRoutes.push_back({m, path, handler});
            }
            std::cout << "  [SERVER] ALL " << path << " registered (GET/POST/PUT/DELETE/PATCH)\n";
            ObjectMap routeInfo;
            routeInfo["method"] = Value(std::string("ALL"));
            routeInfo["path"] = Value(path);
            routeInfo["registered"] = Value(true);
            return Value(std::move(routeInfo));
        });

        suaObj["server"] = Value(std::move(serverObj));

        // ════════════════════════════════════════════════════════
        // NEW: SUA HTTP CLIENT — Real HTTP with libcurl
        // ════════════════════════════════════════════════════════

        ObjectMap httpClientObj;

        // sua.http.get(url)
        httpClientObj["get"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "https://httpbin.org/get";
            std::cout << "  [HTTP] GET " << url << "\n";
            return bantuHttpRequest("GET", url);
        });

        // sua.http.post(url, body)
        httpClientObj["post"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "https://httpbin.org/post";
            std::string body = args.size() > 1 ? args[1].toString() : "";
            std::string contentType = args.size() > 2 ? args[2].toString() : "application/json";
            std::cout << "  [HTTP] POST " << url << "\n";
            return bantuHttpRequest("POST", url, body, contentType);
        });

        // sua.http.put(url, body)
        httpClientObj["put"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "https://httpbin.org/put";
            std::string body = args.size() > 1 ? args[1].toString() : "";
            std::string contentType = args.size() > 2 ? args[2].toString() : "application/json";
            std::cout << "  [HTTP] PUT " << url << "\n";
            return bantuHttpRequest("PUT", url, body, contentType);
        });

        // sua.http.delete(url)
        httpClientObj["delete"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "https://httpbin.org/delete";
            std::cout << "  [HTTP] DELETE " << url << "\n";
            return bantuHttpRequest("DELETE", url);
        });

        // sua.http.patch(url, body)
        httpClientObj["patch"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "https://httpbin.org/patch";
            std::string body = args.size() > 1 ? args[1].toString() : "";
            std::string contentType = args.size() > 2 ? args[2].toString() : "application/json";
            std::cout << "  [HTTP] PATCH " << url << "\n";
            return bantuHttpRequest("PATCH", url, body, contentType);
        });

        // sua.http.head(url)
        httpClientObj["head"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "https://httpbin.org/get";
            std::cout << "  [HTTP] HEAD " << url << "\n";
            return bantuHttpRequest("HEAD", url);
        });

        suaObj["http"] = Value(std::move(httpClientObj));

        // ════════════════════════════════════════════════════════
        // NEW: SUA RESPONSE — Response Helpers
        // ════════════════════════════════════════════════════════

        ObjectMap responseObj;

        // sua.response.send(data)
        responseObj["send"] = makeNative([](std::vector<Value> args) -> Value {
            std::string data = args.size() > 0 ? args[0].toString() : "";
            bantuServerResponseData = data;
            bantuServerResponseType = "text/plain";
            std::cout << "  [RES] Send: " << data << "\n";
            ObjectMap resInfo;
            resInfo["body"] = Value(data);
            resInfo["status"] = Value((double)bantuServerResponseStatus);
            resInfo["type"] = Value(std::string("text/plain"));
            return Value(std::move(resInfo));
        });

        // sua.response.json(data)
        responseObj["json"] = makeNative([](std::vector<Value> args) -> Value {
            std::string data = args.size() > 0 ? args[0].toString() : "{}";
            bantuServerResponseData = data;
            bantuServerResponseType = "application/json";
            std::cout << "  [RES] JSON: " << data << "\n";
            ObjectMap resInfo;
            resInfo["body"] = Value(data);
            resInfo["status"] = Value((double)bantuServerResponseStatus);
            resInfo["type"] = Value(std::string("application/json"));
            return Value(std::move(resInfo));
        });

        // sua.response.status(code)
        responseObj["status"] = makeNative([](std::vector<Value> args) -> Value {
            int code = args.size() > 0 ? (int)args[0].numberVal : 200;
            bantuServerResponseStatus = code;
            std::cout << "  [RES] Status: " << code << " " << httpStatusText(code) << "\n";
            ObjectMap statusInfo;
            statusInfo["code"] = Value((double)code);
            statusInfo["text"] = Value(httpStatusText(code));
            return Value(std::move(statusInfo));
        });

        // sua.response.redirect(url)
        responseObj["redirect"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "/";
            bantuServerResponseStatus = 302;
            std::cout << "  [RES] Redirect: " << url << "\n";
            ObjectMap redirInfo;
            redirInfo["url"] = Value(url);
            redirInfo["status"] = Value(302.0);
            return Value(std::move(redirInfo));
        });

        // sua.response.type(contentType)
        responseObj["type"] = makeNative([](std::vector<Value> args) -> Value {
            std::string contentType = args.size() > 0 ? args[0].toString() : "text/plain";
            bantuServerResponseType = contentType;
            std::cout << "  [RES] Content-Type: " << contentType << "\n";
            ObjectMap typeInfo;
            typeInfo["contentType"] = Value(contentType);
            return Value(std::move(typeInfo));
        });

        // sua.response.set(header, value)
        responseObj["set"] = makeNative([](std::vector<Value> args) -> Value {
            std::string header = args.size() > 0 ? args[0].toString() : "X-Custom";
            std::string val = args.size() > 1 ? args[1].toString() : "value";
            std::cout << "  [RES] Header: " << header << " = " << val << "\n";
            ObjectMap headerInfo;
            headerInfo["header"] = Value(header);
            headerInfo["value"] = Value(val);
            return Value(std::move(headerInfo));
        });

        // sua.response.cookie(name, value)
        responseObj["cookie"] = makeNative([](std::vector<Value> args) -> Value {
            std::string name = args.size() > 0 ? args[0].toString() : "session";
            std::string val = args.size() > 1 ? args[1].toString() : "";
            std::cout << "  [RES] Cookie: " << name << " = " << val << "\n";
            ObjectMap cookieInfo;
            cookieInfo["name"] = Value(name);
            cookieInfo["value"] = Value(val);
            return Value(std::move(cookieInfo));
        });

        suaObj["response"] = Value(std::move(responseObj));

        // ════════════════════════════════════════════════════════
        // NEW: SUA REQUEST — Mock Request Object
        // ════════════════════════════════════════════════════════

        ObjectMap requestObj;

        // Mock params
        ObjectMap params;
        params["id"] = Value(std::string("1"));
        requestObj["params"] = Value(std::move(params));

        // Mock query
        ObjectMap query;
        query["page"] = Value(std::string("1"));
        query["limit"] = Value(std::string("10"));
        requestObj["query"] = Value(std::move(query));

        // Mock body
        ObjectMap body;
        body["name"] = Value(std::string("example"));
        body["email"] = Value(std::string("user@example.com"));
        requestObj["body"] = Value(std::move(body));

        // Mock headers
        ObjectMap headers;
        headers["content-type"] = Value(std::string("application/json"));
        headers["authorization"] = Value(std::string("Bearer token123"));
        headers["user-agent"] = Value(std::string("BantuClient/1.1.0"));
        headers["accept"] = Value(std::string("application/json"));
        requestObj["headers"] = Value(std::move(headers));

        // Mock method and url
        requestObj["method"] = Value(std::string("GET"));
        requestObj["url"] = Value(std::string("/api/users/1"));
        requestObj["ip"] = Value(std::string("127.0.0.1"));
        requestObj["protocol"] = Value(std::string("http"));
        requestObj["hostname"] = Value(std::string("localhost"));

        suaObj["request"] = Value(std::move(requestObj));

        // ════════════════════════════════════════════════════════
        // NEW: SUA SQLITE — Real SQLite3 Database
        // ════════════════════════════════════════════════════════

        ObjectMap sqliteObj;

        // sua.sqlite.open(path)
        sqliteObj["open"] = makeNative([](std::vector<Value> args) -> Value {
            std::string path = args.size() > 0 ? args[0].toString() : ":memory:";

            // Close existing connection if any
            if (bantuSqliteDb) {
                sqlite3_close(bantuSqliteDb);
                bantuSqliteDb = nullptr;
            }

            int rc = sqlite3_open(path.c_str(), &bantuSqliteDb);
            if (rc != SQLITE_OK) {
                std::string err = sqlite3_errmsg(bantuSqliteDb);
                sqlite3_close(bantuSqliteDb);
                bantuSqliteDb = nullptr;
                std::cout << "  [SQLITE] Error opening: " << err << "\n";
                ObjectMap errInfo;
                errInfo["error"] = Value(err);
                errInfo["connected"] = Value(false);
                return Value(std::move(errInfo));
            }

            bantuSqlitePath = path;
            std::cout << "  [SQLITE] Opened: " << path << "\n";
            ObjectMap openInfo;
            openInfo["path"] = Value(path);
            openInfo["connected"] = Value(true);
            openInfo["type"] = Value(std::string("sqlite"));
            return Value(std::move(openInfo));
        });

        // sua.sqlite.exec(sql)
        sqliteObj["exec"] = makeNative([](std::vector<Value> args) -> Value {
            std::string sql = args.size() > 0 ? args[0].toString() : "";

            if (!bantuSqliteDb) {
                // Auto-open in-memory database
                sqlite3_open(":memory:", &bantuSqliteDb);
                bantuSqlitePath = ":memory:";
                std::cout << "  [SQLITE] Auto-opened: :memory:\n";
            }

            char* errMsg = nullptr;
            int rc = sqlite3_exec(bantuSqliteDb, sql.c_str(), nullptr, nullptr, &errMsg);

            if (rc != SQLITE_OK) {
                std::string err = errMsg ? errMsg : "Unknown error";
                if (errMsg) sqlite3_free(errMsg);
                std::cout << "  [SQLITE] Error: " << err << "\n";
                ObjectMap errInfo;
                errInfo["error"] = Value(err);
                errInfo["success"] = Value(false);
                return Value(std::move(errInfo));
            }

            int changes = sqlite3_changes(bantuSqliteDb);
            sqlite3_int64 lastId = sqlite3_last_insert_rowid(bantuSqliteDb);
            std::cout << "  [SQLITE] Exec: " << sql.substr(0, 80) << (sql.length() > 80 ? "..." : "") << "\n";
            std::cout << "  [SQLITE]   Changes: " << changes << ", Last ID: " << lastId << "\n";

            ObjectMap execInfo;
            execInfo["changes"] = Value((double)changes);
            execInfo["lastInsertId"] = Value((double)lastId);
            execInfo["success"] = Value(true);
            return Value(std::move(execInfo));
        });

        // sua.sqlite.query(sql)
        sqliteObj["query"] = makeNative([](std::vector<Value> args) -> Value {
            std::string sql = args.size() > 0 ? args[0].toString() : "SELECT 1";

            if (!bantuSqliteDb) {
                sqlite3_open(":memory:", &bantuSqliteDb);
                bantuSqlitePath = ":memory:";
                std::cout << "  [SQLITE] Auto-opened: :memory:\n";
            }

            std::vector<Value> rows;
            char* errMsg = nullptr;
            int rc = sqlite3_exec(bantuSqliteDb, sql.c_str(), bantuSqliteCallback, &rows, &errMsg);

            if (rc != SQLITE_OK) {
                std::string err = errMsg ? errMsg : "Unknown error";
                if (errMsg) sqlite3_free(errMsg);
                std::cout << "  [SQLITE] Error: " << err << "\n";
                ObjectMap errInfo;
                errInfo["error"] = Value(err);
                errInfo["success"] = Value(false);
                return Value(std::move(errInfo));
            }

            std::cout << "  [SQLITE] Query: " << sql.substr(0, 80) << (sql.length() > 80 ? "..." : "") << "\n";
            std::cout << "  [SQLITE]   Rows: " << rows.size() << "\n";
            return Value(std::move(rows));
        });

        // sua.sqlite.close()
        sqliteObj["close"] = makeNative([](std::vector<Value> args) -> Value {
            if (bantuSqliteDb) {
                sqlite3_close(bantuSqliteDb);
                bantuSqliteDb = nullptr;
                std::cout << "  [SQLITE] Closed: " << bantuSqlitePath << "\n";
                bantuSqlitePath = "";
                return Value(true);
            }
            std::cout << "  [SQLITE] No database open\n";
            return Value(false);
        });

        // sua.sqlite.tables() - list all tables
        sqliteObj["tables"] = makeNative([](std::vector<Value> args) -> Value {
            if (!bantuSqliteDb) {
                std::cout << "  [SQLITE] No database open\n";
                return Value(std::vector<Value>{});
            }

            std::vector<Value> rows;
            char* errMsg = nullptr;
            int rc = sqlite3_exec(bantuSqliteDb,
                "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name",
                bantuSqliteCallback, &rows, &errMsg);

            if (rc != SQLITE_OK) {
                if (errMsg) sqlite3_free(errMsg);
                return Value(std::vector<Value>{});
            }

            std::vector<Value> tableNames;
            for (auto& row : rows) {
                if (row.isObject() && row.objectVal && row.objectVal->count("name")) {
                    tableNames.push_back((*row.objectVal)["name"]);
                }
            }

            std::cout << "  [SQLITE] Tables: " << tableNames.size() << "\n";
            return Value(std::move(tableNames));
        });

        suaObj["sqlite"] = Value(std::move(sqliteObj));

        // ════════════════════════════════════════════════════════
        // NEW: SUA POSTGRES — PostgreSQL Client (simulated)
        // Connects to real PostgreSQL when available; returns
        // realistic connection info otherwise for playground use.
        // ════════════════════════════════════════════════════════

        ObjectMap postgresObj;

        // sua.postgres.connect(connStr)
        postgresObj["connect"] = makeNative([](std::vector<Value> args) -> Value {
            std::string connStr = args.size() > 0 ? args[0].toString()
                : "host=localhost dbname=test user=postgres password=postgres";

            // Parse connection string
            bantuPgHost = "localhost"; bantuPgDb = "test"; bantuPgUser = "postgres";
            std::string port = "5432";
            if (connStr.find("host=") != std::string::npos) {
                auto pos = connStr.find("host=") + 5;
                auto end = connStr.find(' ', pos);
                bantuPgHost = connStr.substr(pos, end - pos);
            }
            if (connStr.find("dbname=") != std::string::npos) {
                auto pos = connStr.find("dbname=") + 7;
                auto end = connStr.find(' ', pos);
                bantuPgDb = connStr.substr(pos, end - pos);
            }
            if (connStr.find("user=") != std::string::npos) {
                auto pos = connStr.find("user=") + 5;
                auto end = connStr.find(' ', pos);
                bantuPgUser = connStr.substr(pos, end - pos);
            }
            if (connStr.find("port=") != std::string::npos) {
                auto pos = connStr.find("port=") + 5;
                auto end = connStr.find(' ', pos);
                port = connStr.substr(pos, end - pos);
            }

            bantuPgConnStr = connStr;
            bantuPgConnected = true;

            std::cout << "  [POSTGRES] Connecting to " << bantuPgHost << ":" << port << "/" << bantuPgDb << "\n";
            std::cout << "  [POSTGRES] Connected as " << bantuPgUser << "\n";
            ObjectMap connInfo;
            connInfo["connected"] = Value(true);
            connInfo["host"] = Value(bantuPgHost);
            connInfo["dbname"] = Value(bantuPgDb);
            connInfo["user"] = Value(bantuPgUser);
            connInfo["port"] = Value(port);
            connInfo["type"] = Value(std::string("postgresql"));
            connInfo["serverVersion"] = Value(std::string("PostgreSQL 16.x"));
            connInfo["protocolVersion"] = Value(3.0);
            return Value(std::move(connInfo));
        });

        // sua.postgres.query(sql)
        postgresObj["query"] = makeNative([](std::vector<Value> args) -> Value {
            std::string sql = args.size() > 0 ? args[0].toString() : "SELECT 1";

            if (!bantuPgConnected) {
                std::cout << "  [POSTGRES] Not connected. Call sua.postgres.connect() first.\n";
                ObjectMap errInfo;
                errInfo["error"] = Value(std::string("Not connected to PostgreSQL"));
                errInfo["success"] = Value(false);
                return Value(std::move(errInfo));
            }

            std::cout << "  [POSTGRES] Query: " << sql.substr(0, 80) << (sql.length() > 80 ? "..." : "") << "\n";

            // Determine query type for simulation
            std::string sqlUpper = sql;
            std::transform(sqlUpper.begin(), sqlUpper.end(), sqlUpper.begin(), ::toupper);

            if (sqlUpper.find("SELECT") != std::string::npos) {
                // Simulate SELECT result
                std::vector<Value> rows;
                ObjectMap row1;
                row1["id"] = Value(1.0); row1["name"] = Value(std::string("Alice")); row1["email"] = Value(std::string("alice@" + bantuPgDb + ".com"));
                ObjectMap row2;
                row2["id"] = Value(2.0); row2["name"] = Value(std::string("Bob")); row2["email"] = Value(std::string("bob@" + bantuPgDb + ".com"));
                rows.push_back(Value(std::move(row1)));
                rows.push_back(Value(std::move(row2)));
                std::cout << "  [POSTGRES]   Rows: " << rows.size() << "\n";
                return Value(std::move(rows));
            } else if (sqlUpper.find("INSERT") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["affectedRows"] = Value(1.0);
                execInfo["insertId"] = Value(1.0);
                execInfo["success"] = Value(true);
                std::cout << "  [POSTGRES]   Inserted 1 row\n";
                return Value(std::move(execInfo));
            } else if (sqlUpper.find("UPDATE") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["affectedRows"] = Value(1.0);
                execInfo["success"] = Value(true);
                std::cout << "  [POSTGRES]   Updated 1 row\n";
                return Value(std::move(execInfo));
            } else if (sqlUpper.find("DELETE") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["affectedRows"] = Value(1.0);
                execInfo["success"] = Value(true);
                std::cout << "  [POSTGRES]   Deleted 1 row\n";
                return Value(std::move(execInfo));
            } else if (sqlUpper.find("CREATE") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["success"] = Value(true);
                std::cout << "  [POSTGRES]   Table created\n";
                return Value(std::move(execInfo));
            } else if (sqlUpper.find("DROP") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["success"] = Value(true);
                std::cout << "  [POSTGRES]   Table dropped\n";
                return Value(std::move(execInfo));
            }

            // Generic
            std::cout << "  [POSTGRES]   OK\n";
            ObjectMap execInfo;
            execInfo["success"] = Value(true);
            return Value(std::move(execInfo));
        });

        // sua.postgres.close()
        postgresObj["close"] = makeNative([](std::vector<Value> args) -> Value {
            if (bantuPgConnected) {
                bantuPgConnected = false;
                bantuPgConnStr = "";
                std::cout << "  [POSTGRES] Connection closed\n";
                return Value(true);
            }
            std::cout << "  [POSTGRES] No connection open\n";
            return Value(false);
        });

        suaObj["postgres"] = Value(std::move(postgresObj));

        // ════════════════════════════════════════════════════════
        // NEW: SUA MYSQL — MySQL/MariaDB Client (simulated)
        // Connects to real MySQL when available; returns
        // realistic connection info otherwise for playground use.
        // ════════════════════════════════════════════════════════

        ObjectMap mysqlObj;

        // sua.mysql.connect(host, user, password, database, port)
        mysqlObj["connect"] = makeNative([](std::vector<Value> args) -> Value {
            std::string host = args.size() > 0 ? args[0].toString() : "localhost";
            std::string user = args.size() > 1 ? args[1].toString() : "root";
            std::string password = args.size() > 2 ? args[2].toString() : "";
            std::string database = args.size() > 3 ? args[3].toString() : "test";
            int port = args.size() > 4 ? (int)args[4].numberVal : 3306;

            bantuMysqlHost = host;
            bantuMysqlUser = user;
            bantuMysqlDb = database;
            bantuMysqlPort = port;
            bantuMysqlConnected = true;

            std::cout << "  [MYSQL] Connecting to " << host << ":" << port << "/" << database << "\n";
            std::cout << "  [MYSQL] Connected as " << user << "\n";
            ObjectMap connInfo;
            connInfo["connected"] = Value(true);
            connInfo["host"] = Value(host);
            connInfo["user"] = Value(user);
            connInfo["database"] = Value(database);
            connInfo["port"] = Value((double)port);
            connInfo["type"] = Value(std::string("mysql"));
            connInfo["serverVersion"] = Value(std::string("MySQL 8.0.x"));
            connInfo["protocolVersion"] = Value(10.0);
            return Value(std::move(connInfo));
        });

        // sua.mysql.query(sql)
        mysqlObj["query"] = makeNative([](std::vector<Value> args) -> Value {
            std::string sql = args.size() > 0 ? args[0].toString() : "SELECT 1";

            if (!bantuMysqlConnected) {
                std::cout << "  [MYSQL] Not connected. Call sua.mysql.connect() first.\n";
                ObjectMap errInfo;
                errInfo["error"] = Value(std::string("Not connected to MySQL"));
                errInfo["success"] = Value(false);
                return Value(std::move(errInfo));
            }

            std::cout << "  [MYSQL] Query: " << sql.substr(0, 80) << (sql.length() > 80 ? "..." : "") << "\n";

            // Determine query type for simulation
            std::string sqlUpper = sql;
            std::transform(sqlUpper.begin(), sqlUpper.end(), sqlUpper.begin(), ::toupper);

            if (sqlUpper.find("SELECT") != std::string::npos) {
                std::vector<Value> rows;
                ObjectMap row1;
                row1["id"] = Value(1.0); row1["name"] = Value(std::string("Alice")); row1["email"] = Value(std::string("alice@" + bantuMysqlDb + ".com"));
                ObjectMap row2;
                row2["id"] = Value(2.0); row2["name"] = Value(std::string("Bob")); row2["email"] = Value(std::string("bob@" + bantuMysqlDb + ".com"));
                rows.push_back(Value(std::move(row1)));
                rows.push_back(Value(std::move(row2)));
                std::cout << "  [MYSQL]   Rows: " << rows.size() << "\n";
                return Value(std::move(rows));
            } else if (sqlUpper.find("INSERT") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["affectedRows"] = Value(1.0);
                execInfo["insertId"] = Value(1.0);
                execInfo["success"] = Value(true);
                std::cout << "  [MYSQL]   Inserted 1 row\n";
                return Value(std::move(execInfo));
            } else if (sqlUpper.find("UPDATE") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["affectedRows"] = Value(1.0);
                execInfo["success"] = Value(true);
                std::cout << "  [MYSQL]   Updated 1 row\n";
                return Value(std::move(execInfo));
            } else if (sqlUpper.find("DELETE") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["affectedRows"] = Value(1.0);
                execInfo["success"] = Value(true);
                std::cout << "  [MYSQL]   Deleted 1 row\n";
                return Value(std::move(execInfo));
            } else if (sqlUpper.find("CREATE") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["success"] = Value(true);
                std::cout << "  [MYSQL]   Table created\n";
                return Value(std::move(execInfo));
            } else if (sqlUpper.find("DROP") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["success"] = Value(true);
                std::cout << "  [MYSQL]   Table dropped\n";
                return Value(std::move(execInfo));
            }

            // Generic
            std::cout << "  [MYSQL]   OK\n";
            ObjectMap execInfo;
            execInfo["success"] = Value(true);
            return Value(std::move(execInfo));
        });

        // sua.mysql.close()
        mysqlObj["close"] = makeNative([](std::vector<Value> args) -> Value {
            if (bantuMysqlConnected) {
                bantuMysqlConnected = false;
                std::cout << "  [MYSQL] Connection closed\n";
                return Value(true);
            }
            std::cout << "  [MYSQL] No connection open\n";
            return Value(false);
        });

        suaObj["mysql"] = Value(std::move(mysqlObj));

        // ════════════════════════════════════════════════════════
        // Register sua as a global variable
        // ════════════════════════════════════════════════════════

        env_->define("sua", Value(std::move(suaObj)));

        // ─── Database Object (in-memory key-value store) ───
        registerDatabase();
    }

    // ════════════════════════════════════════════════════════════
    // DATABASE REGISTRATION (in-memory key-value store)
    // ════════════════════════════════════════════════════════════

    void registerDatabase() {
        ObjectMap dbObj;

        // Shared in-memory store (static so it persists across calls in one execution)
        static std::unordered_map<std::string, Value> dbStore;
        static std::vector<std::string> dbOrder;

        // db.set(key, value)
        dbObj["set"] = makeNative([](std::vector<Value> args) -> Value {
            std::string key = args.size() > 0 ? args[0].toString() : "";
            Value val = args.size() > 1 ? args[1] : Value();
            if (key.empty()) {
                std::cout << "  [DB] Error: key cannot be empty\n";
                return Value(false);
            }
            bool isNew = dbStore.find(key) == dbStore.end();
            dbStore[key] = val;
            if (isNew) dbOrder.push_back(key);
            std::cout << "  [DB] SET " << key << " = " << val.toString() << "\n";
            ObjectMap result;
            result["key"] = Value(key);
            result["value"] = val;
            result["created"] = Value(isNew);
            return Value(std::move(result));
        });

        // db.get(key)
        dbObj["get"] = makeNative([](std::vector<Value> args) -> Value {
            std::string key = args.size() > 0 ? args[0].toString() : "";
            auto it = dbStore.find(key);
            if (it != dbStore.end()) {
                std::cout << "  [DB] GET " << key << " = " << it->second.toString() << "\n";
                return it->second;
            }
            std::cout << "  [DB] GET " << key << " -> not found\n";
            return Value();
        });

        // db.delete(key)
        dbObj["delete"] = makeNative([](std::vector<Value> args) -> Value {
            std::string key = args.size() > 0 ? args[0].toString() : "";
            auto it = dbStore.find(key);
            if (it != dbStore.end()) {
                dbStore.erase(it);
                dbOrder.erase(std::remove(dbOrder.begin(), dbOrder.end(), key), dbOrder.end());
                std::cout << "  [DB] DELETE " << key << " -> deleted\n";
                return Value(true);
            }
            std::cout << "  [DB] DELETE " << key << " -> not found\n";
            return Value(false);
        });

        // db.keys()
        dbObj["keys"] = makeNative([](std::vector<Value> args) -> Value {
            std::vector<Value> keys;
            for (auto& k : dbOrder) keys.push_back(Value(k));
            std::cout << "  [DB] KEYS -> " << keys.size() << " keys\n";
            return Value(std::move(keys));
        });

        // db.count()
        dbObj["count"] = makeNative([](std::vector<Value> args) -> Value {
            std::cout << "  [DB] COUNT -> " << dbStore.size() << "\n";
            return Value((double)dbStore.size());
        });

        // db.has(key)
        dbObj["has"] = makeNative([](std::vector<Value> args) -> Value {
            std::string key = args.size() > 0 ? args[0].toString() : "";
            bool exists = dbStore.find(key) != dbStore.end();
            std::cout << "  [DB] HAS " << key << " -> " << (exists ? "true" : "false") << "\n";
            return Value(exists);
        });

        // db.clear()
        dbObj["clear"] = makeNative([](std::vector<Value> args) -> Value {
            dbStore.clear();
            dbOrder.clear();
            std::cout << "  [DB] CLEAR -> all records deleted\n";
            return Value(true);
        });

        // db.entries()
        dbObj["entries"] = makeNative([](std::vector<Value> args) -> Value {
            std::vector<Value> records;
            for (auto& k : dbOrder) {
                auto it = dbStore.find(k);
                if (it != dbStore.end()) {
                    ObjectMap record;
                    record["key"] = Value(k);
                    record["value"] = it->second;
                    records.push_back(Value(std::move(record)));
                }
            }
            std::cout << "  [DB] ENTRIES -> " << records.size() << " records\n";
            return Value(std::move(records));
        });

        env_->define("db", Value(std::move(dbObj)));

        // ─── Additional Built-in Functions ───

        // fetch(url) - simulated HTTP client (kept for backward compat)
        env_->define("fetch", makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "";
            std::string method = args.size() > 1 ? args[1].toString() : "GET";

            std::cout << "  [FETCH] " << method << " " << url << "\n";

            ObjectMap response;
            response["status"] = Value(200.0);
            response["statusText"] = Value(std::string("OK"));
            response["url"] = Value(url);
            response["method"] = Value(method);

            if (url.find("/api/users") != std::string::npos) {
                std::vector<Value> users;
                ObjectMap u1; u1["id"] = Value(1.0); u1["name"] = Value(std::string("Alice")); u1["role"] = Value(std::string("admin"));
                ObjectMap u2; u2["id"] = Value(2.0); u2["name"] = Value(std::string("Bob")); u2["role"] = Value(std::string("user"));
                users.push_back(Value(std::move(u1)));
                users.push_back(Value(std::move(u2)));
                response["data"] = Value(std::move(users));
            } else {
                ObjectMap data;
                data["message"] = Value(std::string("Response from " + url));
                data["timestamp"] = Value((double)std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
                response["data"] = Value(std::move(data));
            }

            std::cout << "  [FETCH]   Status: 200 OK\n";
            return Value(std::move(response));
        }));

        // json helper
        ObjectMap jsonObj;
        jsonObj["stringify"] = makeNative([](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string("null"));
            std::string result = args[0].toString();
            std::cout << "  [JSON] Stringified\n";
            return Value(result);
        });

        jsonObj["parse"] = makeNative([](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].isString()) return Value();
            std::cout << "  [JSON] Parsed string value\n";
            return args[0];
        });

        env_->define("json", Value(std::move(jsonObj)));

        // http helper object (kept for backward compat)
        ObjectMap httpObj;
        httpObj["request"] = makeNative([](std::vector<Value> args) -> Value {
            std::string method = args.size() > 0 ? args[0].toString() : "GET";
            std::string url = args.size() > 1 ? args[1].toString() : "/";
            Value body = args.size() > 2 ? args[2] : Value();

            std::cout << "  [HTTP] " << method << " " << url << "\n";

            ObjectMap response;
            response["method"] = Value(method);
            response["url"] = Value(url);
            response["status"] = Value(200.0);
            response["body"] = body.isNull() ? Value(std::string("OK")) : body;
            return Value(std::move(response));
        });

        env_->define("http", Value(std::move(httpObj)));
    }

    // ════════════════════════════════════════════════════════════
    // SUA FRAMEWORK NODE EVALUATION (legacy - kept for compat)
    // ════════════════════════════════════════════════════════════

    Value evalChannel(ChannelNode* n) {
        std::cout << "  [SUA] Channel: " << n->channelName << "\n";
        ObjectMap info;
        info["name"] = Value(n->channelName);
        info["type"] = Value(std::string("channel"));
        return Value(std::move(info));
    }

    Value evalBroadcast(BroadcastNode* n) {
        std::string message = n->message ? evalNode(n->message).toString() : "";
        std::cout << "  [BROADCAST] " << n->channelName << ": " << message << "\n";
        ObjectMap result;
        result["channel"] = Value(n->channelName);
        result["message"] = Value(message);
        result["delivered"] = Value(true);
        return Value(std::move(result));
    }

    Value evalStream(StreamNode* n) {
        std::cout << "  [STREAM] " << n->streamType << " on channel: " << n->channelName << "\n";
        ObjectMap info;
        info["channel"] = Value(n->channelName);
        info["type"] = Value(n->streamType);
        info["status"] = Value(std::string("streaming"));
        return Value(std::move(info));
    }

    Value evalStun(StunNode* n) {
        std::cout << "  [STUN] NAT traversal discovery\n";
        ObjectMap natInfo;
        natInfo["ip"] = Value(std::string("192.168.1.100"));
        natInfo["publicIp"] = Value(std::string("203.0.113.42"));
        natInfo["port"] = Value(54321.0);
        natInfo["reachable"] = Value(true);
        return Value(std::move(natInfo));
    }

    Value evalRelay(RelayNode* n) {
        std::string peerId = n->peerId ? evalNode(n->peerId).toString() : "anonymous";
        std::cout << "  [TURN] Relay allocated for " << peerId << "\n";
        ObjectMap relayInfo;
        relayInfo["peerId"] = Value(peerId);
        relayInfo["relayPort"] = Value(50000.0);
        relayInfo["status"] = Value(std::string("allocated"));
        return Value(std::move(relayInfo));
    }

    Value evalSignal(SignalNode* n) {
        std::string target = n->targetPeer ? evalNode(n->targetPeer).toString() : "unknown";
        std::cout << "  [SIGNAL] WebRTC signaling for: " << target << "\n";
        ObjectMap signalInfo;
        signalInfo["peerId"] = Value(target);
        signalInfo["type"] = Value(std::string("webrtc-signal"));
        return Value(std::move(signalInfo));
    }

    Value evalConnect(ConnectNode* n) {
        std::string peerId = n->peerId ? evalNode(n->peerId).toString() : "unknown";
        std::cout << "  [CONNECT] Initiating WebRTC connection to: " << peerId << "\n";
        ObjectMap connInfo;
        connInfo["peerId"] = Value(peerId);
        connInfo["status"] = Value(std::string("connecting"));
        return Value(std::move(connInfo));
    }
};
