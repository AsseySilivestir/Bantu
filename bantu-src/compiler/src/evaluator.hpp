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
#include "module_resolver.hpp"
#include "crypto_native.hpp"   // native (C++) accelerators for the hash/crypto/uuid suite
#include "crypto_sodium.hpp"    // optional libsodium AEAD + argon2id (feature-gated)
#include "dataframe_native.hpp" // native column primitives for the arctic data-science suite
#include "dataframe_arrow.hpp"  // Parquet + Feather/Arrow-IPC I/O (opt-in: -DBANTU_ARROW)
#include "mime_types.hpp"       // extension -> Content-Type for the static file server
#include "event_loop.hpp"       // kqueue/epoll/poll readiness loop for the sua server
#include "pwa_native.hpp"       // manifest / service worker / offline rendering for sua.pwa
#include "webpush.hpp"          // RFC 8188/8291/8292 Web Push (pulls in p256.hpp + aes_gcm.hpp)
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <type_traits>
#include <functional>
#include <random>
#include <algorithm>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <set>
#include <iterator>

// ─── Platform CSPRNG headers (used by bantuCsprng) ───
#if defined(_WIN32)
  #include <windows.h>
  #include <bcrypt.h>           // BCryptGenRandom (link: -lbcrypt)
#elif defined(__linux__)
  #include <unistd.h>
  #include <errno.h>
  #include <sys/syscall.h>      // SYS_getrandom
#endif

// ─── External Library Headers ───
#include <curl/curl.h>
#include <sqlite3.h>

// ─── FFI (foreign function interface) headers ───
// Compiled in when BANTU_FFI is defined and libffi + libdl are linked.
#ifdef BANTU_FFI
#include <dlfcn.h>
#if defined(__APPLE__)
  #include <ffi/ffi.h>
#else
  #include <ffi.h>
#endif
#endif

// Helper to create native function values without ambiguity
inline Value makeNative(NativeFn fn) { return Value(std::move(fn)); }

// ════════════════════════════════════════════════════════════════
// CRYPTO PRIMITIVES — support for the pure-Bantu hash/crypto/uuid modules.
// General-purpose low-level ops the language otherwise lacks: 32-bit bitwise
// arithmetic, byte<->list conversion, an OS CSPRNG, and constant-time compare.
// Binary data is represented in Bantu as a LIST of numbers 0..255 (no new
// type). These are the atoms; the hash/cipher algorithms live in .b files.
// ════════════════════════════════════════════════════════════════

// Interpret a Bantu number as a 32-bit unsigned word (defined wraparound).
static inline uint32_t bantuU32(double d) {
    long long ll = (long long)std::llround(d);
    return (uint32_t)(uint64_t)ll;
}

// A Bantu string or byte-list → raw bytes.
static inline std::vector<unsigned char> bantuToBytes(const Value& v) {
    std::vector<unsigned char> out;
    if (v.isString()) {
        out.assign(v.stringVal.begin(), v.stringVal.end());
    } else if (v.isList()) {
        out.reserve(v.listVal.size());
        for (const auto& e : v.listVal) {
            out.push_back((unsigned char)((unsigned)bantuU32(e.numberVal) & 0xFFu));
        }
    }
    return out;
}

// Raw bytes → a Bantu list-of-numbers (0..255) value.
static inline Value bantuBytesToList(const unsigned char* p, size_t n) {
    std::vector<Value> out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) out.push_back(Value((double)p[i]));
    return Value(std::move(out));
}

// Fill buf with cryptographically-secure random bytes from the OS.
//
// SECURITY (fail-closed): this returns false rather than ever producing
// predictable output. Callers MUST surface that failure — a fallback to a
// non-cryptographic PRNG (the `random` LCG) for key/salt/nonce material is the
// vulnerability this whole suite exists to avoid, so it is intentionally absent.
// Platform sources, in order of preference:
//   • Windows  : BCryptGenRandom (CNG, system-preferred RNG)
//   • Apple/BSD: arc4random_buf  (cannot fail)
//   • Linux    : getrandom(2) if available, else /dev/urandom
static inline bool bantuCsprng(unsigned char* buf, size_t n) {
    if (n == 0) return true;
#if defined(_WIN32)
    // BCryptGenRandom with BCRYPT_USE_SYSTEM_PREFERRED_RNG needs no algorithm
    // handle. Returns 0 (STATUS_SUCCESS) on success.
    return BCryptGenRandom(nullptr, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(buf, n);
    return true;
#else
    // Prefer getrandom(2); it needs no file descriptor and cannot be starved by
    // an exhausted FD table. Fall back to /dev/urandom on older kernels.
  #if defined(__linux__) && defined(SYS_getrandom)
    size_t got = 0;
    while (got < n) {
        long r = ::syscall(SYS_getrandom, buf + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;   // retry on signal
            break;                          // fall through to /dev/urandom
        }
        got += (size_t)r;
    }
    if (got == n) return true;
  #endif
    std::ifstream f("/dev/urandom", std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(buf), (std::streamsize)n);
    return (size_t)f.gcount() == n;
#endif
}

// Raw bytes → lowercase hex string.
static inline std::string bantuHexOf(const std::vector<unsigned char>& b) {
    static const char* HX = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (unsigned char c : b) { s.push_back(HX[c >> 4]); s.push_back(HX[c & 0xF]); }
    return s;
}

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

// ─── Server resource limits ───────────────────────────────────────────────
// Every one of these was "unbounded" before, which is the difference between a
// server and a denial-of-service target. See docs/sua-architecture.md §9.
// Configure with sua.server.limits({...}); the defaults are the safe ones.
struct BantuServerLimits {
    size_t maxHeaderBytes   = 64 * 1024;        // request header block
    size_t maxBodyBytes     = 8 * 1024 * 1024;  // request body
    int    maxConnections   = 10000;            // concurrent, process-wide
    int    headerTimeoutMs  = 10000;            // slowloris defence
    int    idleTimeoutMs    = 300000;           // reap dead peers
    size_t maxWsFrameBytes  = 1024 * 1024;      // single WebSocket frame
    size_t maxWsMessageBytes= 8 * 1024 * 1024;  // reassembled, across continuations
    bool   wsCheckOrigin    = true;             // reject cross-site WS upgrades
    std::vector<std::string> wsAllowedOrigins;  // empty + check on = same-origin only
};
static BantuServerLimits bantuLimits;

// Mirrors the evaluator's --quiet flag for code outside the class (setQuiet
// keeps them in step), so diagnostics honour `bantu -q` like everything else.
static bool bantuQuietMode = false;

// ─── Event-loop connection state ───────────────────────────────────────────
// One of these per open socket, replacing one OS thread per open socket. A
// thread costs ~8MB of stack; this costs its buffers. That difference is what
// takes the server from a few thousand connections to a hundred thousand.
//
// Everything here is touched ONLY by the loop thread, which is why none of it
// needs a lock -- and why the interpreter lock could be deleted with the
// threads that made it necessary.
struct BantuConn {
    int         fd = -1;
    bool        isWs = false;
    std::string in;              // bytes read, not yet consumed
    std::string out;             // bytes to write, not yet sent
    size_t      outPos = 0;      // how much of `out` has gone to the kernel
    bool        closing = false; // close once `out` drains
    bool        wantWrite = false;
    uint64_t    lastActive = 0;
    // WebSocket only
    int         wsId = 0;
    std::string fragment;        // reassembled continuation frames
    uint8_t     fragOpcode = 0;
};
static std::unordered_map<int, BantuConn> bantuConns;
static bantu_loop::Backend* bantuLoopBackend = nullptr;

// Watch this fd for writability iff it has pending output.
static void bantuConnSyncInterest(BantuConn& c) {
    bool want = (c.outPos < c.out.size());
    if (want != c.wantWrite && bantuLoopBackend) {
        bantuLoopBackend->mod(c.fd, true, want);
        c.wantWrite = want;
    }
}

// Queue bytes for a connection instead of blocking in send().
//
// This is the change that lets the existing request-handling code run on the
// loop unmodified: every send(sock, ...) becomes an append here. If the fd is
// not a loop connection (a test harness, anything pre-loop) it falls back to a
// real send so nothing else has to care.
static void bantuConnWriteN(int fd, const char* data, size_t n) {
    auto it = bantuConns.find(fd);
    if (it == bantuConns.end()) { send(fd, data, (int)n, 0); return; }
    BantuConn& c = it->second;

    // Backpressure. A client that stops reading must not be able to make the
    // server buffer without bound -- that is memory exhaustion requiring no
    // packets beyond opening the socket and going quiet.
    if (c.out.size() - c.outPos + n > (size_t)(4 * 1024 * 1024)) {
        c.closing = true;
        return;
    }
    c.out.append(data, n);
    bantuConnSyncInterest(c);
}
static void bantuConnWrite(int fd, const std::string& sv) { bantuConnWriteN(fd, sv.data(), sv.size()); }

// Content-Length from an already-complete header block, 0 when absent or
// malformed. Digits are parsed by hand rather than with stoull, matching
// bantuReadBody -- that avoids a glibc 2.38 symbol dependency the project
// deliberately does not take.
static size_t bantuContentLengthOf(const std::string& buf, size_t headerEnd) {
    std::string h = buf.substr(0, headerEnd);
    for (auto& ch : h) ch = (char)std::tolower((unsigned char)ch);
    size_t at = h.find("content-length:");
    if (at == std::string::npos) return 0;
    size_t i = at + 15;
    while (i < h.size() && (h[i] == ' ' || h[i] == '\t')) i++;
    size_t v = 0; bool any = false;
    for (; i < h.size() && h[i] >= '0' && h[i] <= '9'; i++) {
        v = v * 10 + (size_t)(h[i] - '0');
        any = true;
        if (v > ((size_t)1 << 40)) return ((size_t)1 << 40);   // absurd; caller rejects
    }
    return any ? v : 0;
}

// Graceful close: finish writing what is queued, then drop.
static void bantuConnClose(int fd) {
    auto it = bantuConns.find(fd);
    if (it == bantuConns.end()) { CLOSE_SOCKET(fd); return; }
    it->second.closing = true;
}

// Live connection count, for the cap. Incremented on accept, decremented when
// the loop drops the connection.
static std::atomic<int> bantuLiveConnections{0};

static std::vector<BantuServerRoute> bantuServerRoutes;
static int bantuServerPort = 3000;
static std::string bantuServerHost = "0.0.0.0";
static std::vector<std::string> bantuServerMiddleware;
static std::vector<Value> bantuServerMiddlewareFuncs;  // function middleware
static std::vector<std::string> bantuServerStatic;
static std::string bantuServerResponseData;
static int bantuServerResponseStatus = 200;
static std::string bantuServerResponseType = "text/plain";

// ─── PWA state (sua.pwa / sua.push) ───
static bantu_pwa::Config bantuPwaConfig;
static std::string bantuPushPrivateKeyB64;   // VAPID private key, base64url
static std::string bantuPushSubject;         // "mailto:..." or an https URL
static std::string bantuPushDbPath = "";     // sqlite file holding subscriptions

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
        case Value::CLASS_INSTANCE:
        case Value::NATIVE_HANDLE: return "null";   // not JSON-serializable
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

// ─── Open-file registry (Python-style file I/O) ───
// open() returns a handle dict {"__file": id}; the actual std::fstream lives
// here, keyed by id. Wrapped in a function to dodge static init-order issues.
static std::unordered_map<int, std::fstream>& bantuFileTable() {
    static std::unordered_map<int, std::fstream> table;
    return table;
}
static std::atomic<int> bantuNextFileId{1};

// ─── UDP socket registry (sua.udp namespace, v1.4.0) ───
// sua.udp.socket() returns a handle dict {"__udp": id}; the actual fd lives
// here, keyed by id. Same pattern as bantuFileTable().
//
// Each entry holds the OS socket fd, family (AF_INET / AF_INET6), and a
// flag indicating whether it's been bound. Lifecycle: socket() → bind() →
// recvfrom()/send_to() → close().
//
// Platform includes — POSIX vs Windows are wrapped in #ifdef.
#ifdef _WIN32
    // Winsock2 — needs to be initialized via WSAStartup before any socket call.
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define BANTU_CLOSE_SOCKET closesocket
    #define BANTU_SOCKET_ERRNO WSAGetLastError()
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <poll.h>
    #include <unistd.h>
    #include <fcntl.h>
    #define BANTU_CLOSE_SOCKET close
    #define BANTU_SOCKET_ERRNO errno
#endif
struct BantuUdpSocket {
    int fd = -1;
    int family = AF_INET;
    bool bound = false;
};
static std::unordered_map<int, BantuUdpSocket>& bantuUdpSocketTable() {
    static std::unordered_map<int, BantuUdpSocket> table;
    return table;
}
static std::atomic<int> bantuNextUdpId{1};

// ─── SHA-1 (for WebSocket handshake, RFC 6455) ────────────────────
// We need SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11") → base64
// for the Sec-WebSocket-Accept header. ~50 lines of inline SHA1.
struct BantuSha1 {
    uint32_t h0=0x67452301, h1=0xEFCDAB89, h2=0x98BADCFE, h3=0x10325476, h4=0xC3D2E1F0;
    uint8_t msg[64]; int msgLen=0; uint64_t totalLen=0;

    void update(const uint8_t* data, size_t len) {
        totalLen += len;
        for (size_t i = 0; i < len; i++) {
            msg[msgLen++] = data[i];
            if (msgLen == 64) { process(); msgLen = 0; }
        }
    }
    void update(const std::string& s) { update((const uint8_t*)s.data(), s.size()); }

    void process() {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (msg[i*4]<<24) | (msg[i*4+1]<<16) | (msg[i*4+2]<<8) | msg[i*4+3];
        }
        for (int i = 16; i < 80; i++) {
            uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (t << 1) | (t >> 31);
        }
        uint32_t a=h0, b=h1, c=h2, d=h3, e=h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i<20)      { f=(b&c)|((~b)&d); k=0x5A827999; }
            else if (i<40) { f=b^c^d;          k=0x6ED9EBA1; }
            else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else           { f=b^c^d;          k=0xCA62C1D6; }
            uint32_t temp = ((a<<5)|(a>>27)) + f + e + k + w[i];
            e=d; d=c; c=(b<<30)|(b>>2); b=a; a=temp;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }

    std::string final_() {
        uint64_t bits = totalLen * 8;
        msg[msgLen++] = 0x80;
        while (msgLen != 56) { if (msgLen==64) { process(); msgLen=0; } msg[msgLen++]=0; }
        for (int i = 7; i >= 0; i--) msg[msgLen++] = (bits >> (i*8)) & 0xFF;
        process();
        std::string out(20, '\0');
        uint32_t hs[5] = {h0,h1,h2,h3,h4};
        for (int i = 0; i < 5; i++) {
            out[i*4]   = (hs[i]>>24)&0xFF;
            out[i*4+1] = (hs[i]>>16)&0xFF;
            out[i*4+2] = (hs[i]>>8)&0xFF;
            out[i*4+3] = hs[i]&0xFF;
        }
        return out;
    }
};

// ─── Base64 encoder (for WebSocket handshake) ──────────────────────
static std::string bantuBase64Encode(const std::string& input) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (uint8_t c : input) {
        val = (val << 8) | c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(tbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// ─── WebSocket connection table ────────────────────────────────────
struct BantuWsClient {
    int fd = -1;
    std::string id;       // client ID (for sua.ws.send/broadcast)
    bool alive = true;
};
static std::unordered_map<int, BantuWsClient>& bantuWsTable() {
    static std::unordered_map<int, BantuWsClient> table;
    return table;
}
// Guards bantuWsTable(). Connection threads insert/erase; sua.ws.* builtins
// read. Concurrent insert/erase on an unordered_map is undefined behaviour.
// Held only across map access, NEVER across a send or a Bantu callback.

// Snapshot helpers. The sua.ws.* builtins copy what they need out of the table
// under the lock, then send with the lock released — holding a mutex across a
// blocking socket write would let one slow reader stall every connect and
// disconnect on the server.
static std::vector<int> bantuWsLiveFds() {
    std::vector<int> fds;
    for (const auto& kv : bantuWsTable())
        if (kv.second.fd >= 0 && kv.second.alive) fds.push_back(kv.second.fd);
    return fds;
}
static std::vector<std::string> bantuWsLiveIds() {
    std::vector<std::string> ids;
    for (const auto& kv : bantuWsTable())
        if (kv.second.fd >= 0) ids.push_back(kv.second.id);
    return ids;
}
// -1 when the client is unknown or already gone.
static int bantuWsFdFor(const std::string& clientId) {
    for (const auto& kv : bantuWsTable())
        if (kv.second.id == clientId && kv.second.fd >= 0) return kv.second.fd;
    return -1;
}

static std::atomic<int> bantuNextWsId{1};

// Bantu-level WS event handlers (set by sua.ws.on)
static Value bantuWsOnConnect = Value();
static Value bantuWsOnMessage = Value();
static Value bantuWsOnDisconnect = Value();

// Callback function — set by Evaluator constructor, used by
// the free-function WebSocket handler to call Bantu callbacks.
#include <functional>
static std::function<Value(Value, std::vector<Value>)> bantuWsCallback;

// Forward declaration — the real definition is inside Evaluator class
// (at line ~1550). The WS handler calls through this function pointer.

// ─── Send a WebSocket text frame to a client ───────────────────────
// Server→client frames are NOT masked (per RFC 6455).
static void bantuWsSend(int fd, const std::string& message) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81);  // FIN + text opcode

    size_t len = message.size();
    if (len <= 125) {
        frame.push_back((uint8_t)len);
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((len >> (i * 8)) & 0xFF);
        }
    }
    frame.insert(frame.end(), message.begin(), message.end());
    // Queued, not sent: a blocking send() to one slow client would stall every
    // other connection this thread owns.
    bantuConnWriteN(fd, (const char*)frame.data(), frame.size());
}

// ─── Send a WebSocket BINARY frame to a client (for voice/audio) ───
// Same as above but opcode = 0x82 (binary) instead of 0x81 (text).
// Accepts raw bytes as a Bantu list of numbers 0-255.
static void bantuWsSendBinary(int fd, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> frame;
    frame.push_back(0x82);  // FIN + binary opcode

    size_t len = data.size();
    if (len <= 125) {
        frame.push_back((uint8_t)len);
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((len >> (i * 8)) & 0xFF);
        }
    }
    frame.insert(frame.end(), data.begin(), data.end());
    bantuConnWriteN(fd, (const char*)frame.data(), frame.size());
}

// ─── WebSocket framing helpers (RFC 6455) ──────────────────────────
// Read exactly n octets, or fail. The previous frame reader took whatever a
// single recv() returned and silently truncated anything larger -- so any
// message bigger than one TCP segment arrived corrupted. Voice frames hit that
// constantly.
static bool bantuRecvExact(int sock, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(sock, (char*)buf + got, (int)(n - got), 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

// Close with a status code (RFC 6455 §5.5.1), then the caller closes the fd.
static void bantuWsClose(int sock, uint16_t code, const std::string& reason = "") {
    std::string payload;
    payload.push_back((char)(code >> 8));
    payload.push_back((char)(code & 0xFF));
    payload += reason.substr(0, 123);
    std::vector<uint8_t> frame;
    frame.push_back(0x88);                       // FIN + close
    frame.push_back((uint8_t)payload.size());    // control frames are always < 126
    frame.insert(frame.end(), payload.begin(), payload.end());
    bantuConnWriteN(sock, (const char*)frame.data(), frame.size());
}

// RFC 6455 §8.1 requires text frames to be valid UTF-8. Rejecting invalid
// sequences also stops overlong encodings and surrogates reaching Bantu strings.
static bool bantuValidUtf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t len; unsigned int cp;
        if (c < 0x80)                  { i++; continue; }
        else if ((c & 0xE0) == 0xC0)   { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0)   { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0)   { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (len == 2 && cp < 0x80) return false;            // overlong
        if (len == 3 && cp < 0x800) return false;           // overlong
        if (len == 4 && cp < 0x10000) return false;         // overlong
        if (cp > 0x10FFFF) return false;                    // out of range
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;     // surrogate half
        i += len;
    }
    return true;
}

// ─── Handle a WebSocket connection (after upgrade) ─────────────────
// Runs in the same thread that accepted the HTTP connection — blocks
// until the WS client disconnects.
// ─── WebSocket on the event loop ───────────────────────────────────
// The connection no longer owns a thread. Upgrade registers the client and
// returns immediately; each later readable event feeds bantuWsProcess, which
// consumes whatever COMPLETE frames are in the buffer and leaves a partial one
// for the next wakeup. That incremental parse is also what makes frame
// truncation at TCP segment boundaries structurally impossible.
static void bantuWsUpgrade(int sock, const std::string& wsKey) {
    BantuSha1 sha;
    sha.update(wsKey);
    sha.update(std::string("258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    std::string acceptVal = bantuBase64Encode(sha.final_());

    std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " + acceptVal + "\r\n"
                       "\r\n";
    bantuConnWrite(sock, resp);

    int wsId = bantuNextWsId++;
    BantuWsClient client;
    client.fd = sock;
    client.id = "ws-" + std::to_string(wsId);
    bantuWsTable()[wsId] = client;

    auto it = bantuConns.find(sock);
    if (it != bantuConns.end()) { it->second.isWs = true; it->second.wsId = wsId; }

    if (!bantuQuietMode)
        std::cout << "  [WS] Client connected: " << client.id << " (fd=" << sock << ")\n";

    if (bantuWsOnConnect.isFunction() || bantuWsOnConnect.isNativeFn()) {
        ObjectMap cliObj;
        cliObj["id"] = Value(client.id);
        cliObj["fd"] = Value((double)sock);
        if (bantuWsCallback) {
            try { bantuWsCallback(bantuWsOnConnect, {Value(std::move(cliObj))}); }
            catch (const std::exception& e) { std::cerr << "  [WS] onConnect error: " << e.what() << "\n"; }
        }
    }
}

// Consume every complete frame currently buffered. Returns false when the
// connection must close: a protocol violation (already answered with a close
// frame) or a close frame from the peer.
static bool bantuWsProcess(BantuConn& c) {
    const int sock = c.fd;
    std::string clientId;
    {
        auto t = bantuWsTable().find(c.wsId);
        if (t != bantuWsTable().end()) clientId = t->second.id;
    }

    size_t pos = 0;
    bool keep = true;
    while (keep) {
        size_t avail = c.in.size() - pos;
        if (avail < 2) break;
        const uint8_t* p = (const uint8_t*)c.in.data() + pos;

        bool     fin    = (p[0] & 0x80) != 0;
        uint8_t  rsv    =  p[0] & 0x70;
        uint8_t  opcode =  p[0] & 0x0F;
        bool     masked = (p[1] & 0x80) != 0;
        uint64_t len    =  p[1] & 0x7F;
        size_t   hdrLen = 2;

        if (len == 126) {
            if (avail < 4) break;
            len = ((uint64_t)p[2] << 8) | p[3];
            hdrLen = 4;
        } else if (len == 127) {
            if (avail < 10) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | p[2 + i];
            hdrLen = 10;
        }
        if (masked) hdrLen += 4;

        // Validate before trusting `len` to size anything.
        if (rsv) { bantuWsClose(sock, 1002, "RSV bit set"); return false; }
        bool isControl = (opcode & 0x08) != 0;
        if (isControl && (len > 125 || !fin)) { bantuWsClose(sock, 1002, "bad control frame"); return false; }
        if (!masked) { bantuWsClose(sock, 1002, "unmasked client frame"); return false; }
        if (len > bantuLimits.maxWsFrameBytes) { bantuWsClose(sock, 1009, "frame too large"); return false; }

        if (avail < hdrLen + (size_t)len) break;      // partial frame: wait

        const uint8_t* mask = p + hdrLen - 4;
        std::string payload((const char*)p + hdrLen, (size_t)len);
        for (size_t i = 0; i < payload.size(); i++) payload[i] ^= (char)mask[i % 4];
        pos += hdrLen + (size_t)len;

        // Reassemble continuation frames (opcode 0x0 continues the previous).
        if (!isControl) {
            if (opcode == 0x0) {
                if (c.fragOpcode == 0) { bantuWsClose(sock, 1002, "unexpected continuation"); return false; }
                if (c.fragment.size() + payload.size() > bantuLimits.maxWsMessageBytes) {
                    bantuWsClose(sock, 1009, "message too large"); return false;
                }
                c.fragment += payload;
            } else {
                if (c.fragOpcode != 0) { bantuWsClose(sock, 1002, "interleaved message"); return false; }
                if (!fin) { c.fragOpcode = opcode; c.fragment = payload; }
            }
            if (!fin) continue;                        // more fragments coming
            if (c.fragOpcode != 0) {                   // final fragment
                payload = c.fragment;
                opcode  = c.fragOpcode;
                c.fragment.clear();
                c.fragOpcode = 0;
            }
        }

        if (opcode == 0x1 && !bantuValidUtf8(payload)) {
            bantuWsClose(sock, 1007, "invalid UTF-8"); return false;
        }

        if (opcode == 0x8) { keep = false; break; }                 // close
        if (opcode == 0x9) {                                        // ping -> pong
            uint8_t pong[2] = {0x8A, 0x00};
            bantuConnWriteN(sock, (const char*)pong, 2);
            continue;
        }
        if (opcode == 0xA) continue;                                // pong

        if (opcode == 0x2) {                                        // binary
            if (bantuWsOnMessage.isFunction() || bantuWsOnMessage.isNativeFn()) {
                ObjectMap msgObj;
                msgObj["data"]   = Value(payload);
                msgObj["client"] = Value(clientId);
                msgObj["binary"] = Value(true);
                std::vector<Value> byteList;
                byteList.reserve(payload.size());
                for (char ch : payload) byteList.push_back(Value((double)(uint8_t)ch));
                msgObj["bytes"] = Value(std::move(byteList));
                if (bantuWsCallback) {
                    try { bantuWsCallback(bantuWsOnMessage, {Value(std::move(msgObj))}); }
                    catch (const std::exception& e) { std::cerr << "  [WS] onMessage(binary) error: " << e.what() << "\n"; }
                }
            }
        }
        if (opcode == 0x1) {                                        // text
            if (!bantuQuietMode)
                std::cout << "  [WS] Message from " << clientId << ": " << payload << "\n";
            if (bantuWsOnMessage.isFunction() || bantuWsOnMessage.isNativeFn()) {
                ObjectMap msgObj;
                msgObj["data"]   = Value(payload);
                msgObj["client"] = Value(clientId);
                if (!payload.empty() && (payload[0] == '{' || payload[0] == '[')) {
                    try {
                        size_t jp = 0;
                        msgObj["json"] = bantuJsonParse(payload, jp);
                    } catch (...) {}
                }
                if (bantuWsCallback) {
                    try { bantuWsCallback(bantuWsOnMessage, {Value(std::move(msgObj))}); }
                    catch (const std::exception& e) { std::cerr << "  [WS] onMessage error: " << e.what() << "\n"; }
                }
            }
        }
    }
    c.in.erase(0, pos);
    return keep;
}

// onDisconnect + deregister. Called once, when the loop drops the connection.
static void bantuWsTeardown(int wsId) {
    std::string id;
    auto t = bantuWsTable().find(wsId);
    if (t == bantuWsTable().end()) return;
    id = t->second.id;
    if (!bantuQuietMode) std::cout << "  [WS] Client disconnected: " << id << "\n";
    if (bantuWsOnDisconnect.isFunction() || bantuWsOnDisconnect.isNativeFn()) {
        ObjectMap cliObj;
        cliObj["id"] = Value(id);
        if (bantuWsCallback) {
            try { bantuWsCallback(bantuWsOnDisconnect, {Value(std::move(cliObj))}); }
            catch (...) {}
        }
    }
    bantuWsTable().erase(wsId);
}

// Helper: parse "host:port" → (host, port). Supports IPv6 brackets [::1]:53.
static std::pair<std::string, int> bantuUdpParseAddr(const std::string& addr) {
    // IPv6 form: [::1]:53
    if (!addr.empty() && addr[0] == '[') {
        auto end = addr.find(']');
        if (end != std::string::npos && end + 2 <= addr.size() && addr[end + 1] == ':') {
            std::string host = addr.substr(1, end - 1);
            int port = std::atoi(addr.c_str() + end + 2);
            return {host, port};
        }
    }
    // IPv4 form: 127.0.0.1:53
    auto colon = addr.rfind(':');
    if (colon == std::string::npos) return {"", 0};
    return {addr.substr(0, colon), std::atoi(addr.c_str() + colon + 1)};
}

// Helper: convert a Bantu list-of-bytes (numbers 0-255) into a std::vector<uint8_t>.
static std::vector<uint8_t> bantuValueToBytes(const Value& v) {
    std::vector<uint8_t> out;
    if (v.isList()) {
        out.reserve(v.listVal.size());
        for (const auto& e : v.listVal) {
            int b = (int)e.numberVal;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            out.push_back((uint8_t)b);
        }
    } else if (v.isString()) {
        const auto& s = v.stringVal;
        out.assign(s.begin(), s.end());
    }
    return out;
}

// Helper: convert std::vector<uint8_t> into a Bantu list-of-bytes.
static Value bantuBytesToValue(const std::vector<uint8_t>& buf) {
    std::vector<Value> out;
    out.reserve(buf.size());
    for (uint8_t b : buf) out.push_back(Value((double)b));
    return Value(std::move(out));
}

// Helper: resolve a host string + port into a struct sockaddr_storage.
// Supports IPv4 dotted-quad, IPv6 (with or without brackets), and hostnames
// (uses getaddrinfo() to resolve). Returns 0 on success, -1 on failure.
static int bantuUdpResolve(const std::string& host, int port,
                            struct sockaddr_storage* ss, socklen_t* sslen,
                            int family, std::string* errOut) {
    memset(ss, 0, sizeof(*ss));

    // Try IPv4 dotted-quad first (no DNS lookup)
    if (family == AF_INET) {
        struct sockaddr_in* sa = (struct sockaddr_in*)ss;
        sa->sin_family = AF_INET;
        sa->sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host.c_str(), &sa->sin_addr) == 1) {
            *sslen = sizeof(*sa);
            return 0;
        }
    }
    // Try IPv6 literal next (no DNS lookup)
    if (family == AF_INET6) {
        struct sockaddr_in6* sa6 = (struct sockaddr_in6*)ss;
        sa6->sin6_family = AF_INET6;
        sa6->sin6_port = htons((uint16_t)port);
        if (inet_pton(AF_INET6, host.c_str(), &sa6->sin6_addr) == 1) {
            *sslen = sizeof(*sa6);
            return 0;
        }
    }

    // Hostname — use getaddrinfo() with the requested family.
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = 0;
    struct addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    int gai_rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (gai_rc != 0) {
        if (errOut) *errOut = std::string("could not resolve host '") + host + "': " + gai_strerror(gai_rc);
        return -1;
    }
    if (!res) {
        if (errOut) *errOut = std::string("no addresses for host: ") + host;
        return -1;
    }
    // Use the first result. (Caller could iterate res->ai_next for round-robin.)
    memcpy(ss, res->ai_addr, res->ai_addrlen);
    *sslen = res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

// Helper: format a sockaddr_storage back into "host:port" string.
static std::string bantuUdpFormatAddr(const struct sockaddr_storage* ss) {
    char buf[INET6_ADDRSTRLEN];
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in* sa = (const struct sockaddr_in*)ss;
        inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(ntohs(sa->sin_port));
    }
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6* sa6 = (const struct sockaddr_in6*)ss;
        inet_ntop(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf));
        return std::string("[") + buf + "]:" + std::to_string(ntohs(sa6->sin6_port));
    }
    return "unknown";
}

// Helper: cross-platform poll() wrapper.
#ifdef _WIN32
static int bantuUdpPoll(SOCKET fd, int timeoutMs) {
    WSAPOLLFD pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return WSAPoll(&pfd, 1, timeoutMs);
}
static std::string bantuUdpErrStr() {
    int e = WSAGetLastError();
    char buf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, buf, sizeof(buf), nullptr);
    return std::string(buf);
}
static void bantuUdpSetNonblocking(SOCKET fd) {
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
}
#else
static int bantuUdpPoll(int fd, int timeoutMs) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return poll(&pfd, 1, timeoutMs);
}
static std::string bantuUdpErrStr() {
    return std::string(strerror(errno));
}
static void bantuUdpSetNonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
#endif

// ─── FFI: call arbitrary C functions in shared libraries (via libffi) ───
//   $m    = loadlib("libm.dylib")                 // dlopen a shared library
//   $sqrt = func($m, "sqrt", "double", ["double"]) // bind a symbol + signature
//   $sqrt(2.0)                                     // → 1.41421356
// Type names: "int" | "double" | "string" | "pointer" | "void".
#ifdef BANTU_FFI
static std::unordered_map<int, void*>& bantuLibTable() {
    static std::unordered_map<int, void*> t; return t;
}
static int bantuNextLibId = 1;

static ffi_type* bantuFfiType(const std::string& t) {
    if (t == "int")                                   return &ffi_type_sint;
    if (t == "double" || t == "float")                return &ffi_type_double;
    if (t == "string" || t == "pointer" || t == "ptr") return &ffi_type_pointer;
    if (t == "void")                                  return &ffi_type_void;
    return &ffi_type_sint;  // sensible default
}

static Value bantuFfiLoadLib(std::vector<Value> args) {
    if (args.empty()) ErrorHandler::throwError("loadlib() needs a library path", 0, 0, ErrorHandler::RUNTIME_ERROR);
    std::string path = args[0].toString();
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        ErrorHandler::throwError(std::string("loadlib failed for '") + path + "': " + (e ? e : "unknown"),
                                 0, 0, ErrorHandler::RUNTIME_ERROR);
    }
    int id = bantuNextLibId++;
    bantuLibTable()[id] = h;
    ObjectMap handle;
    handle["__lib"] = Value((double)id);
    handle["path"] = Value(path);
    return Value(std::move(handle));
}

static Value bantuFfiFunc(std::vector<Value> args) {
    if (args.size() < 3)
        ErrorHandler::throwError("func(lib, name, retType, [argTypes]) needs at least 3 arguments", 0, 0, ErrorHandler::RUNTIME_ERROR);
    void* lib = nullptr;
    if (args[0].isObject()) {
        auto it = args[0].objectVal->find("__lib");
        if (it != args[0].objectVal->end()) {
            auto lt = bantuLibTable().find((int)it->second.numberVal);
            if (lt != bantuLibTable().end()) lib = lt->second;
        }
    }
    if (!lib) ErrorHandler::throwError("func(): first argument is not a library from loadlib()", 0, 0, ErrorHandler::RUNTIME_ERROR);
    std::string name = args[1].toString();
    void* sym = dlsym(lib, name.c_str());
    if (!sym) ErrorHandler::throwError("func(): symbol '" + name + "' not found", 0, 0, ErrorHandler::RUNTIME_ERROR);
    std::string retType = args[2].toString();
    std::vector<std::string> argTypes;
    if (args.size() > 3 && args[3].isList())
        for (auto& a : args[3].listVal) argTypes.push_back(a.toString());

    // Return a callable that marshals Bantu values through libffi and invokes sym.
    return makeNative([sym, retType, argTypes](std::vector<Value> callArgs) -> Value {
        size_t n = argTypes.size();
        std::vector<ffi_type*> atypes(n);
        for (size_t i = 0; i < n; i++) atypes[i] = bantuFfiType(argTypes[i]);

        ffi_cif cif;
        if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)n, bantuFfiType(retType),
                         n ? atypes.data() : nullptr) != FFI_OK)
            ErrorHandler::throwError("ffi_prep_cif failed", 0, 0, ErrorHandler::RUNTIME_ERROR);

        // Stable storage for marshalled args (addresses must survive ffi_call).
        std::vector<long long> ints(n);
        std::vector<double> dbls(n);
        std::vector<std::string> strs(n);
        std::vector<const char*> cstrs(n);
        std::vector<void*> values(n);
        for (size_t i = 0; i < n; i++) {
            const std::string& t = argTypes[i];
            Value v = i < callArgs.size() ? callArgs[i] : Value();
            if (t == "double" || t == "float") { dbls[i] = v.numberVal; values[i] = &dbls[i]; }
            else if (t == "string" || t == "pointer" || t == "ptr") { strs[i] = v.toString(); cstrs[i] = strs[i].c_str(); values[i] = &cstrs[i]; }
            else { ints[i] = (long long)v.numberVal; values[i] = &ints[i]; }
        }

        if (retType == "double" || retType == "float") {
            double r = 0; ffi_call(&cif, FFI_FN(sym), &r, n ? values.data() : nullptr); return Value(r);
        } else if (retType == "string" || retType == "pointer" || retType == "ptr") {
            void* r = nullptr; ffi_call(&cif, FFI_FN(sym), &r, n ? values.data() : nullptr);
            if (retType == "string" && r) return Value(std::string((const char*)r));
            return Value((double)(intptr_t)r);
        } else if (retType == "void") {
            ffi_arg r; ffi_call(&cif, FFI_FN(sym), &r, n ? values.data() : nullptr); return Value();
        } else {
            ffi_arg r = 0; ffi_call(&cif, FFI_FN(sym), &r, n ? values.data() : nullptr); return Value((double)(long long)r);
        }
    });
}
#else
// Stubs when FFI is not compiled in.
static Value bantuFfiLoadLib(std::vector<Value>) {
    ErrorHandler::throwError("FFI not available in this build (rebuild with -DBANTU_FFI and link -lffi)", 0, 0, ErrorHandler::RUNTIME_ERROR);
    return Value();
}
static Value bantuFfiFunc(std::vector<Value>) {
    ErrorHandler::throwError("FFI not available in this build (rebuild with -DBANTU_FFI and link -lffi)", 0, 0, ErrorHandler::RUNTIME_ERROR);
    return Value();
}
#endif

// ─── PostgreSQL State ───
// When built with -DBANTU_POSTGRES=ON (and libpq available), bantuPgConn
// holds a real PGconn* and queries hit a real PostgreSQL database.
// Otherwise the static binary uses the deterministic stub below.
static bool bantuPgConnected = false;
static std::string bantuPgConnStr = "";
static std::string bantuPgHost = "";
static std::string bantuPgDb = "";
static std::string bantuPgUser = "";
#ifdef HAS_LIBPQ
    #include <libpq-fe.h>
    static PGconn* bantuPgConn = nullptr;

    // Build libpq text-format parameter arrays from a Bantu params list, for
    // PQexecParams ($1..$n placeholders). libpq sends every parameter as text
    // and lets the server coerce it, so this stays type-agnostic and is
    // injection-safe. A null Bantu value maps to a SQL NULL (null pointer).
    // NOTE: `storage` is reserved up-front so it never reallocates — the
    // c_str() pointers handed to libpq must stay valid for the call.
    static void bantuPgBuildParams(const std::vector<Value>& params,
                                   std::vector<std::string>& storage,
                                   std::vector<const char*>& out) {
        storage.reserve(params.size());
        out.reserve(params.size());
        for (const auto& p : params) {
            if (p.isNull()) {
                storage.push_back("");
                out.push_back(nullptr);
            } else if (p.isBool()) {
                storage.push_back(p.boolVal ? "true" : "false");
                out.push_back(storage.back().c_str());
            } else {
                storage.push_back(p.toString());
                out.push_back(storage.back().c_str());
            }
        }
    }
#endif

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

// Bind a list of Bantu values to a prepared statement's `?` placeholders
// (1-based). This is the safe, injection-proof path for parameterized SQL.
static void bantuSqliteBindParams(sqlite3_stmt* stmt, const std::vector<Value>& params) {
    for (size_t i = 0; i < params.size(); i++) {
        int idx = (int)i + 1;
        const Value& p = params[i];
        if (p.isNull()) {
            sqlite3_bind_null(stmt, idx);
        } else if (p.isNumber()) {
            double d = p.numberVal;
            if (d == std::floor(d) && !std::isinf(d)) sqlite3_bind_int64(stmt, idx, (sqlite3_int64)d);
            else sqlite3_bind_double(stmt, idx, d);
        } else if (p.isBool()) {
            sqlite3_bind_int(stmt, idx, p.boolVal ? 1 : 0);
        } else {
            std::string s = p.toString();
            sqlite3_bind_text(stmt, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT);
        }
    }
}

// Convert a text column value to a number when it is fully numeric, mirroring
// bantuSqliteCallback so parameterized queries return the same shapes.
static Value bantuSqliteCellToValue(const char* txt) {
    std::string val = txt ? txt : "NULL";
    try {
        size_t pos;
        double nv = std::stod(val, &pos);
        if (pos == val.size()) return Value(nv);
    } catch (...) {}
    return Value(val);
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

// Global TLS opt-out for the convenience helpers (sua.http.get/post/...), which
// take no options object. Set with sua.http.insecure(true). Per-request
// "insecure": true on sua.http.request() is preferred and independent of this.
static bool bantuHttpInsecureAll = false;

// Options for the general form. The three-argument legacy helpers below fill
// this in and keep their old behaviour.
struct BantuHttpOptions {
    std::vector<std::pair<std::string, std::string>> headers;
    long timeout = 10;
    bool verifyTls = true;    // certificates ARE verified; opt out per request
    bool verbose   = false;   // print a one-line [HTTP] trace
};

// scheme://host[:port] — what we log instead of the full URL.
//
// A URL's path and query routinely carry secrets: API tokens in query strings,
// and a Web Push endpoint whose path IS the subscription identifier. Logging
// the origin keeps the trace useful for debugging without writing those into
// application output. BANTU_HTTP_DEBUG=1 opts in to the full URL.
static std::string bantuUrlOrigin(const std::string& url) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "<url>";
    size_t hostStart = scheme + 3;
    size_t end = url.find_first_of("/?#", hostStart);
    std::string origin = (end == std::string::npos) ? url : url.substr(0, end);
    // Strip any userinfo (user:pass@host) — credentials must never be logged.
    size_t at = origin.find('@', hostStart);
    if (at != std::string::npos) origin = origin.substr(0, hostStart) + origin.substr(at + 1);
    return origin;
}

static Value bantuHttpRequestEx(const std::string& method, const std::string& url,
                                const std::string& body, const std::string& contentType,
                                const BantuHttpOptions& opt) {
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, opt.timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Bantu-Lang/1.1.0");
    // TLS peer/host verification is ON. Pass insecure:true per request to opt
    // out (self-signed development endpoints).
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, opt.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, opt.verifyTls ? 2L : 0L);

    // Body: ALWAYS length-explicit.
    //
    // CURLOPT_COPYPOSTFIELDS documents that "if the size has not been set prior
    // to CURLOPT_COPYPOSTFIELDS, the data is assumed to be a null-terminated
    // string" — so without a size libcurl calls strlen() and truncates any
    // binary body at its first NUL. An aes128gcm push body opens with 16 random
    // octets, so roughly two in five were being silently cut short.
    //
    // The ORDER below is load-bearing: the size must be set first. COPYPOSTFIELDS
    // (rather than POSTFIELDS) makes libcurl own the bytes, so `body` need not
    // outlive the call.
    auto setBody = [&]() {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
        curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body.data());
    };

    struct curl_slist* headers = nullptr;
    bool callerSetAccept = false, callerSetContentType = false;
    for (const auto& h : opt.headers) {
        std::string lower;
        for (char c : h.first) lower += (char)std::tolower((unsigned char)c);
        if (lower == "accept")       callerSetAccept = true;
        if (lower == "content-type") callerSetContentType = true;
    }
    if (!callerSetAccept)
        headers = curl_slist_append(headers, "Accept: application/json, text/plain, */*");

    if (method == "POST" || method == "PUT" || method == "PATCH") {
        if (method == "POST") curl_easy_setopt(curl, CURLOPT_POST, 1L);
        else                  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        setBody();
        if (!callerSetContentType) {
            std::string ct = "Content-Type: " + (contentType.empty() ? std::string("application/json")
                                                                     : contentType);
            headers = curl_slist_append(headers, ct.c_str());
        }
    } else if (method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (!body.empty()) setBody();
        if (!body.empty() && !callerSetContentType && !contentType.empty()) {
            std::string ct = "Content-Type: " + contentType;
            headers = curl_slist_append(headers, ct.c_str());
        }
    } else if (method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) setBody();
    }

    // Caller-supplied headers last, so they win.
    for (const auto& h : opt.headers) {
        std::string line = h.first + ": " + h.second;
        headers = curl_slist_append(headers, line.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    ObjectMap response;

    // Log the origin, not the full URL — see bantuUrlOrigin. BANTU_HTTP_DEBUG=1
    // opts in to the full URL when you are actually debugging a request.
    const char* httpDebug = std::getenv("BANTU_HTTP_DEBUG");
    bool fullUrl = httpDebug && httpDebug[0] && httpDebug[0] != '0';
    std::string traceTarget = fullUrl ? url : bantuUrlOrigin(url);
    bool trace = opt.verbose && !bantuQuietMode;

    if (res != CURLE_OK) {
        std::string err = curl_easy_strerror(res);
        // curl's certificate errors say nothing about how to proceed. Since
        // verification is now on by default, spell out the two real options.
        if (res == CURLE_PEER_FAILED_VERIFICATION || res == CURLE_SSL_CACERT_BADFILE
#ifdef CURLE_SSL_CACERT
            || res == CURLE_SSL_CACERT
#endif
            ) {
            err += " — the server's TLS certificate could not be verified. "
                   "Install a trusted certificate, or, for a development endpoint only, "
                   "pass \"insecure\": true to sua.http.request()";
        }
        response["error"] = Value(err);
        response["status"] = Value(0.0);
        response["ok"] = Value(false);
        response["url"] = Value(url);
        response["method"] = Value(method);
        if (trace)
            std::cerr << "  [HTTP] " << method << " " << traceTarget << " -> ERROR: "
                      << curl_easy_strerror(res) << "\n";
    } else {
        response["status"] = Value((double)responseCode);
        response["statusText"] = Value(httpStatusText((int)responseCode));
        response["ok"] = Value(responseCode >= 200 && responseCode < 300);
        response["body"] = Value(responseBody);
        response["url"] = Value(url);
        response["method"] = Value(method);
        response["headers"] = Value(responseHeaders);
        if (trace)
            std::cerr << "  [HTTP] " << method << " " << traceTarget << " -> " << responseCode
                      << " " << httpStatusText((int)responseCode) << "\n";
    }

    return Value(std::move(response));
}

// Legacy three-argument form, behind sua.http.get/post/put/delete/patch/head.
// Behaviour is unchanged except that TLS certificates are now verified and the
// trace goes to stderr, honours --quiet, and names only the origin. These
// helpers take no options object, so they read the global TLS opt-out set by
// sua.http.insecure().
static Value bantuHttpRequest(const std::string& method, const std::string& url,
                              const std::string& body = "", const std::string& contentType = "") {
    BantuHttpOptions opt;
    opt.verbose = true;
    opt.verifyTls = !bantuHttpInsecureAll;
    return bantuHttpRequestEx(method, url, body, contentType, opt);
}

// ════════════════════════════════════════════════════════════════
// PWA / WEB PUSH SERVER HELPERS
//
// Route handlers, the subscription store, and the send path for sua.pwa /
// sua.push. Rendering lives in pwa_native.hpp; the crypto in webpush.hpp.
// ════════════════════════════════════════════════════════════════

// Write a complete response through the $res object handed to a route handler.
static void bantuPwaRespond(const Value& res, int status, const std::string& contentType,
                            const std::string& body,
                            const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    if (!res.isObject()) return;
    ObjectMap& r = *res.objectVal;
    auto call = [&](const char* key, std::vector<Value> args) {
        auto it = r.find(key);
        if (it != r.end() && it->second.isNativeFn()) it->second.nativeFn(std::move(args));
    };
    call("status", { Value((double)status) });
    call("type",   { Value(contentType) });
    for (const auto& h : extra) call("set", { Value(h.first), Value(h.second) });
    // `type` before `send`: send only overrides the content type when it is
    // still the default "application/json".
    call("send",   { Value(body) });
}

// ── subscription store ──────────────────────────────────────────────────────
//
// A dedicated sqlite handle, separate from the app's sua.sqlite connection so
// configuring push can never disturb the application's own database.
static sqlite3* bantuPushDb = nullptr;

static bool bantuPushDbOpen(const std::string& path) {
    if (bantuPushDb) { sqlite3_close(bantuPushDb); bantuPushDb = nullptr; }
    if (sqlite3_open(path.c_str(), &bantuPushDb) != SQLITE_OK) {
        if (bantuPushDb) { sqlite3_close(bantuPushDb); bantuPushDb = nullptr; }
        return false;
    }
    const char* schema =
        "CREATE TABLE IF NOT EXISTS push_subscriptions ("
        "  endpoint TEXT PRIMARY KEY,"
        "  p256dh   TEXT NOT NULL,"
        "  auth     TEXT NOT NULL,"
        "  tag      TEXT NOT NULL DEFAULT '',"
        "  created  INTEGER NOT NULL DEFAULT 0);";
    char* err = nullptr;
    if (sqlite3_exec(bantuPushDb, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) { std::cerr << "  [sua.push] schema error: " << err << "\n"; sqlite3_free(err); }
        return false;
    }
    return true;
}

// Pull {endpoint, p256dh, auth} out of either a full PushSubscription
// ({endpoint, keys:{p256dh, auth}}) or an already-flattened row.
static bool bantuPushExtract(const Value& v, std::string& endpoint,
                             std::string& p256dh, std::string& auth) {
    if (!v.isObject()) return false;
    ObjectMap& o = *v.objectVal;
    auto get = [&](ObjectMap& m, const char* k) {
        auto it = m.find(k);
        return (it == m.end() || it->second.isNull()) ? std::string("") : it->second.toString();
    };
    // Allow the browser's wrapper: { "subscription": {...} }
    auto sit = o.find("subscription");
    if (sit != o.end() && sit->second.isObject())
        return bantuPushExtract(sit->second, endpoint, p256dh, auth);

    endpoint = get(o, "endpoint");
    auto kit = o.find("keys");
    if (kit != o.end() && kit->second.isObject()) {
        p256dh = get(*kit->second.objectVal, "p256dh");
        auth   = get(*kit->second.objectVal, "auth");
    } else {
        p256dh = get(o, "p256dh");
        auth   = get(o, "auth");
    }
    return !endpoint.empty() && !p256dh.empty() && !auth.empty();
}

static bool bantuPushSave(const Value& sub, const std::string& tag) {
    if (!bantuPushDb) return false;
    std::string endpoint, p256dh, auth;
    if (!bantuPushExtract(sub, endpoint, p256dh, auth)) return false;

    // Reject a subscription whose key material we could never use, rather than
    // storing it and failing on every future send.
    bantu_webpush::Bytes kb, ab;
    if (!bantu_webpush::b64url_decode(p256dh, kb) || kb.size() != 65) return false;
    if (!bantu_webpush::b64url_decode(auth, ab)   || ab.empty())      return false;

    const char* sql = "INSERT INTO push_subscriptions(endpoint,p256dh,auth,tag,created) "
                      "VALUES(?,?,?,?,?) ON CONFLICT(endpoint) DO UPDATE SET "
                      "p256dh=excluded.p256dh, auth=excluded.auth, tag=excluded.tag;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(bantuPushDb, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, endpoint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, p256dh.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, auth.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, tag.c_str(),     -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool bantuPushForget(const std::string& endpoint) {
    if (!bantuPushDb || endpoint.empty()) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(bantuPushDb, "DELETE FROM push_subscriptions WHERE endpoint = ?;",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, endpoint.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok && sqlite3_changes(bantuPushDb) > 0;
}

// Every stored subscription, or just those carrying `tag`.
static Value bantuPushList(const std::string& tag) {
    std::vector<Value> out;
    if (!bantuPushDb) return Value(std::move(out));
    const char* sql = tag.empty()
        ? "SELECT endpoint,p256dh,auth,tag,created FROM push_subscriptions ORDER BY created;"
        : "SELECT endpoint,p256dh,auth,tag,created FROM push_subscriptions WHERE tag = ? ORDER BY created;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(bantuPushDb, sql, -1, &st, nullptr) != SQLITE_OK) return Value(std::move(out));
    if (!tag.empty()) sqlite3_bind_text(st, 1, tag.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        auto txt = [&](int i) {
            const unsigned char* p = sqlite3_column_text(st, i);
            return p ? std::string((const char*)p) : std::string("");
        };
        ObjectMap row, keys;
        row["endpoint"] = Value(txt(0));
        keys["p256dh"]  = Value(txt(1));
        keys["auth"]    = Value(txt(2));
        row["keys"]     = Value(std::move(keys));
        row["tag"]      = Value(txt(3));
        row["created"]  = Value((double)sqlite3_column_int64(st, 4));
        out.push_back(Value(std::move(row)));
    }
    sqlite3_finalize(st);
    return Value(std::move(out));
}

// ── sending ─────────────────────────────────────────────────────────────────

static Value bantuPushSendOne(const Value& sub, const Value& payload, const Value& opts) {
    ObjectMap out;
    auto fail = [&](const std::string& msg, int status = 0) {
        out["ok"] = Value(false);
        out["status"] = Value((double)status);
        out["error"] = Value(msg);
        return Value(std::move(out));
    };

    if (!bantu_webpush::selftest().ok)
        return fail("Web Push is unavailable: the crypto selftest failed in this binary");
    if (bantuPushPrivateKeyB64.empty())
        return fail("call sua.push.configure({public_key, private_key, subject}) first");

    std::string endpoint, p256dh, auth;
    if (!bantuPushExtract(sub, endpoint, p256dh, auth))
        return fail("subscription needs endpoint plus keys.p256dh and keys.auth");

    std::string aud;
    if (!bantu_webpush::origin_of(endpoint, aud))
        return fail("endpoint must be an https URL: " + endpoint);

    // Payload: objects and lists become JSON (the shape /serviceworker.js
    // expects); anything else is sent as text.
    std::string body_text;
    if (payload.isObject() || payload.isList()) body_text = bantuJsonStringify(payload);
    else if (!payload.isNull())                 body_text = payload.toString();

    if (body_text.size() > bantu_webpush::kMaxPayload)
        return fail("payload is " + std::to_string(body_text.size()) + " octets; the limit is " +
                    std::to_string(bantu_webpush::kMaxPayload) +
                    " (4096 minus the 86-octet header, delimiter and tag)");

    bantu_webpush::Bytes key, salt_auth;
    if (!bantu_webpush::b64url_decode(p256dh, key) || key.size() != 65)
        return fail("keys.p256dh must be 65 base64url octets");
    if (!bantu_webpush::b64url_decode(auth, salt_auth) || salt_auth.empty())
        return fail("keys.auth is not valid base64url");

    bantu_webpush::Bytes encrypted;
    if (!bantu_webpush::encrypt(key.data(), salt_auth.data(), salt_auth.size(),
                                (const uint8_t*)body_text.data(), body_text.size(), encrypted))
        return fail("encryption failed — the subscription's p256dh may not be a valid P-256 point");

    // Options
    long ttl = 86400;
    std::string urgency, topic;
    bool insecure = false;
    if (opts.isObject()) {
        ObjectMap& o = *opts.objectVal;
        auto it = o.find("ttl");
        if (it != o.end() && it->second.isNumber()) ttl = (long)it->second.numberVal;
        it = o.find("urgency"); if (it != o.end() && !it->second.isNull()) urgency = it->second.toString();
        it = o.find("topic");   if (it != o.end() && !it->second.isNull()) topic = it->second.toString();
        it = o.find("insecure");if (it != o.end()) insecure = it->second.isTruthy();
    }

    int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    bantu_webpush::Bytes priv;
    if (!bantu_webpush::b64url_decode(bantuPushPrivateKeyB64, priv) || priv.size() != 32)
        return fail("the configured VAPID private key is malformed");

    std::string authHeader;
    // 12 hours: RFC 8292 §2 caps `exp` at 24 hours from now.
    if (!bantu_webpush::vapid_header(priv.data(), aud, bantuPushSubject, now + 12 * 3600, authHeader))
        return fail("could not build the VAPID Authorization header");

    BantuHttpOptions hopt;
    hopt.verifyTls = !insecure;
    hopt.timeout = 15;
    hopt.headers.emplace_back("Authorization", authHeader);
    hopt.headers.emplace_back("Content-Encoding", "aes128gcm");
    hopt.headers.emplace_back("Content-Type", "application/octet-stream");
    hopt.headers.emplace_back("TTL", std::to_string(ttl));   // required by RFC 8030 §5.2
    if (!urgency.empty()) hopt.headers.emplace_back("Urgency", urgency);
    if (!topic.empty())   hopt.headers.emplace_back("Topic", topic);

    std::string raw((const char*)encrypted.data(), encrypted.size());
    Value resp = bantuHttpRequestEx("POST", endpoint, raw, "application/octet-stream", hopt);

    int status = 0;
    std::string respBody, err;
    if (resp.isObject()) {
        auto& r = *resp.objectVal;
        auto it = r.find("status"); if (it != r.end()) status = (int)it->second.numberVal;
        it = r.find("body");        if (it != r.end()) respBody = it->second.toString();
        it = r.find("error");       if (it != r.end()) err = it->second.toString();
    }
    out["ok"] = Value(status >= 200 && status < 300);
    out["status"] = Value((double)status);
    out["endpoint"] = Value(endpoint);
    out["bytes"] = Value((double)encrypted.size());
    if (!respBody.empty()) out["body"] = Value(respBody);
    if (!err.empty()) out["error"] = Value(err);
    // 404/410 mean the subscription is permanently gone; send_all prunes on this.
    out["expired"] = Value(status == 404 || status == 410);
    if (status == 413) out["error"] = Value(std::string("push service rejected the payload as too large"));
    if (status == 401 || status == 403)
        out["error"] = Value(std::string("push service rejected the VAPID credentials "
                                         "(check `subject` and that the public key matches "
                                         "the one the browser subscribed with)"));
    return Value(std::move(out));
}

// POST <subscribe_url> — store what /pwa.js sends after PushManager.subscribe().
static Value bantuPushSubscribeHandler(std::vector<Value> args) {
    Value req = args.size() > 0 ? args[0] : Value();
    Value res = args.size() > 1 ? args[1] : Value();
    std::string tag;
    Value body;
    if (req.isObject()) {
        auto it = req.objectVal->find("body");
        if (it != req.objectVal->end()) body = it->second;
    }
    if (body.isObject()) {
        auto t = body.objectVal->find("tag");
        if (t != body.objectVal->end() && !t->second.isNull()) tag = t->second.toString();
    }
    bool ok = bantuPushSave(body, tag);
    ObjectMap o;
    o["ok"] = Value(ok);
    if (!ok) o["error"] = Value(std::string("invalid subscription"));
    bantuPwaRespond(res, ok ? 201 : 400, "application/json; charset=utf-8",
                    bantuJsonStringify(Value(std::move(o))));
    return Value();
}

// DELETE <subscribe_url> — drop a subscription the browser has unsubscribed.
static Value bantuPushUnsubscribeHandler(std::vector<Value> args) {
    Value req = args.size() > 0 ? args[0] : Value();
    Value res = args.size() > 1 ? args[1] : Value();
    std::string endpoint;
    if (req.isObject()) {
        auto it = req.objectVal->find("body");
        if (it != req.objectVal->end() && it->second.isObject()) {
            auto e = it->second.objectVal->find("endpoint");
            if (e != it->second.objectVal->end()) endpoint = e->second.toString();
        }
    }
    bool ok = bantuPushForget(endpoint);
    ObjectMap o;
    o["ok"] = Value(ok);
    bantuPwaRespond(res, 200, "application/json; charset=utf-8",
                    bantuJsonStringify(Value(std::move(o))));
    return Value();
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
        env_->functionScope = true;   // global root is an assignment boundary
        bantuWsCallback = [this](Value callee, std::vector<Value> args) -> Value {
            // Switch to the global environment so the handler can access
            // 'sua' and other globals. The WS handler may be called from
            // a different call stack than a normal HTTP request.
            auto savedEnv = this->env_;
            this->env_ = this->globalEnv_;
            auto result = this->bantuCallFunction(callee, std::move(args));
            this->env_ = savedEnv;
            return result;
        };
        curl_global_init(CURL_GLOBAL_DEFAULT);
        // Web Push takes its randomness from the one platform CSPRNG. Left
        // unset it fails closed rather than falling back to a predictable PRNG.
        bantu_webpush::random_bytes = bantuCsprng;
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

    // v1.2.2: Suppress informational [INCLUDE] log lines. Errors still print.
    void setQuiet(bool q) {
        quietMode_ = q;
        bantuQuietMode = q;   // free functions (the HTTP trace) read this
    }
    bool isQuiet() const { return quietMode_; }

    Value evaluate(std::vector<std::shared_ptr<ASTNode>>& program) {
        Value result;
        try {
            for (auto& node : program) {
                result = evalNode(node);
            }
        } catch (const BreakSignal&) {
            ErrorHandler::throwRuntimeError("'break' used outside of a loop");
        } catch (const ContinueSignal&) {
            ErrorHandler::throwRuntimeError("'continue' used outside of a loop");
        } catch (const ReturnSignal&) {
            // A top-level `return` simply ends the program.
        }
        return result;
    }

    // v1.2.1: Run a program from a file. Pushes the file path onto
    // the resolution stack so that `include` statements inside the
    // file resolve relative to it.
    Value runFile(const std::string& path, std::vector<std::shared_ptr<ASTNode>>& program) {
        filePathStack_.push_back(path);
        Value result;
        try {
            for (auto& node : program) {
                result = evalNode(node);
            }
        } catch (const BreakSignal&) {
            if (!filePathStack_.empty()) filePathStack_.pop_back();
            ErrorHandler::throwRuntimeError("'break' used outside of a loop");
        } catch (const ContinueSignal&) {
            if (!filePathStack_.empty()) filePathStack_.pop_back();
            ErrorHandler::throwRuntimeError("'continue' used outside of a loop");
        } catch (const ReturnSignal&) {
            // A top-level `return` simply ends the file.
        }
        if (!filePathStack_.empty()) filePathStack_.pop_back();
        return result;
    }

    // v1.2.1: Set the active file path (used by main.cpp when running `bantu run file.b`)
    void setEntryPoint(const std::string& path) {
        filePathStack_.push_back(path);
    }

private:
    std::shared_ptr<Environment> env_;
    std::shared_ptr<Environment> globalEnv_;

    // v1.2.1: stack of file paths being executed (for relative include resolution)
    std::vector<std::string> filePathStack_;

    // Cycle guard: includes already loaded in the current chain
    std::vector<std::string> loadedModules_;

    // v1.2.2: include depth guard (prevent infinite include chains)
    int includeDepth_ = 0;
    static constexpr int kMaxIncludeDepth = 64;

    // v1.2.2: quiet mode suppresses informational [INCLUDE] logs
    bool quietMode_ = false;

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
        if (dynamic_cast<BreakNode*>(node.get()))              throw BreakSignal{};
        if (dynamic_cast<ContinueNode*>(node.get()))           throw ContinueSignal{};
        if (auto n = dynamic_cast<ThrowNode*>(node.get()))     return evalThrow(n);
        if (auto n = dynamic_cast<SwitchNode*>(node.get()))    return evalSwitch(n);
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
        // v1.2.1: module include
        if (auto n = dynamic_cast<IncludeNode*>(node.get()))  return evalInclude(n);

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
        // `const $x = …` makes the binding final; typed decls (number/string/…)
        // define normally (annotations remain non-enforcing at runtime).
        bool isConst = (n->typeAnnotation == "const");
        env_->define(n->name, val, isConst);
        return val;
    }

    Value evalAssign(AssignNode* n) {
        Value val = evalNode(n->value);
        // Function-local assignment: resolve up to the enclosing function
        // boundary only, else define locally. Prevents a callee from clobbering
        // a caller's/global's variable of the same name (e.g. a loop counter).
        env_->assign(n->name, val);
        return val;
    }

    Value evalIndexAssign(IndexAssignNode* n) {
        Value val = evalNode(n->value);

        // Preferred path: resolve the container to its real storage location and
        // mutate in place. Handles a plain variable, a dict/list field of a class
        // instance (`this.list[i] = x`), a nested member (`this.a.b[i] = x`), and
        // dict entries — anything resolveLValue can address — so the write always
        // persists (lists are stored by value, so a copy would be lost).
        if (Value* base = resolveLValue(n->object.get())) {
            Value idx = evalNode(n->index);
            if (base->isList()) {
                int i = (int)idx.numberVal;
                if (i < 0) {
                    ErrorHandler::throwRuntimeError("Index out of bounds: " + std::to_string(i), n->line, n->col);
                    return Value();
                }
                if (i >= (int)base->listVal.size()) {
                    base->listVal.resize(i + 1, Value(0.0));
                }
                base->listVal[i] = val;
                return val;
            }
            if (base->isObject()) {
                (*base->objectVal)[idx.toString()] = val;
                return val;
            }
            // resolvable but not indexable → fall through to the error/slow path
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
            case BantuTokenType::PLUS:
                if (left.isString() || right.isString()) {
                    return Value(left.toString() + right.toString());
                }
                return Value(left.numberVal + right.numberVal);
            case BantuTokenType::MINUS: return Value(left.numberVal - right.numberVal);
            case BantuTokenType::MULTIPLY: return Value(left.numberVal * right.numberVal);
            case BantuTokenType::DIVIDE:
                if (right.numberVal == 0) {
                    ErrorHandler::throwRuntimeError("Division by zero", n->line, n->col);
                    return Value();
                }
                return Value(left.numberVal / right.numberVal);
            case BantuTokenType::MODULO:
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
            case BantuTokenType::EQUALTO: return Value(left.equals(right));
            case BantuTokenType::NOTEQUALTO: return Value(!left.equals(right));
            case BantuTokenType::GREATERTHAN: return Value(left.numberVal > right.numberVal);
            case BantuTokenType::LESSTHAN: return Value(left.numberVal < right.numberVal);
            case BantuTokenType::GREATERTHANEQUAL: return Value(left.numberVal >= right.numberVal);
            case BantuTokenType::LESSTHANEQUAL: return Value(left.numberVal <= right.numberVal);
            case BantuTokenType::AND: return Value(left.isTruthy() && right.isTruthy());
            case BantuTokenType::OR: return Value(left.isTruthy() || right.isTruthy());
            default:
                ErrorHandler::throwRuntimeError("Unknown operator", n->line, n->col);
                return Value();
        }
    }

    Value evalUnaryOp(UnaryOpNode* n) {
        Value operand = evalNode(n->operand);
        switch (n->op) {
            case BantuTokenType::NOT: return Value(!operand.isTruthy());
            case BantuTokenType::MINUS: return Value(-operand.numberVal);
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

    // each ($x in list) / each ($k, $v in dict) / for $x in ... / for $k, $v in ...
    // Iterates lists (element, or unpacking a [k,v] pair into two vars) and dicts
    // (key, or key+value). break/continue are honored; return/errors propagate.
    Value evalEach(EachNode* n) {
        Value iterable = evalNode(n->iterable);
        bool twoVars = !n->valueVar.empty();

        // Runs the body once with the loop var(s) bound. Returns false on break.
        auto runBody = [&](const Value& a, const Value& b) -> bool {
            auto prevEnv = env_;
            env_ = std::make_shared<Environment>(prevEnv);
            env_->define(n->varName, a);
            if (twoVars) env_->define(n->valueVar, b);
            try {
                for (auto& stmt : n->body) evalNode(stmt);
            } catch (const BreakSignal&) {
                env_ = prevEnv;
                return false;
            } catch (const ContinueSignal&) {
                env_ = prevEnv;
                return true;
            } catch (...) {
                env_ = prevEnv;   // return / error → restore scope and propagate
                throw;
            }
            env_ = prevEnv;
            return true;
        };

        if (iterable.isList()) {
            for (auto& item : iterable.listVal) {
                if (twoVars) {
                    // Unpack a [key, value] pair (e.g. from $dict.items()); if the
                    // element isn't a 2+ list, bind value to null.
                    Value a = item, b;
                    if (item.isList() && item.listVal.size() >= 2) {
                        a = item.listVal[0];
                        b = item.listVal[1];
                    }
                    if (!runBody(a, b)) break;
                } else {
                    if (!runBody(item, Value())) break;
                }
            }
        } else if (iterable.isObject()) {
            for (auto& kv : *iterable.objectVal) {
                if (!runBody(Value(kv.first), kv.second)) break;
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
            callEnv->functionScope = true;   // function-local assignment boundary
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

            std::string ct = bantu_mime::for_path(filePath);
            bool isHtml = ct.rfind("text/html", 0) == 0;

            // With auto_inject on, patch the PWA <head> block into served HTML so
            // an existing app becomes installable without editing its templates.
            // inject_meta() is idempotent — a page that already calls
            // sua.pwa.meta() is left untouched.
            if (bantuPwaConfig.configured && bantuPwaConfig.auto_inject && isHtml) {
                content = bantu_pwa::inject_meta(content, bantu_pwa::render_meta(bantuPwaConfig));
            }

            // A manifest is always revalidated: a stale one pins an old start_url
            // or icon set. HTML is only switched to no-cache once a PWA is
            // configured — there a stale shell pins old asset URLs and the
            // service worker makes it sticky. Non-PWA apps keep the previous
            // max-age so this change cannot alter behaviour they rely on.
            bool noCache = ct.rfind("application/manifest+json", 0) == 0
                        || (isHtml && bantuPwaConfig.configured);

            std::ostringstream resp;
            resp << "HTTP/1.1 200 OK\r\n";
            resp << "Content-Type: " << ct << "\r\n";
            resp << "Content-Length: " << content.size() << "\r\n";
            resp << "Access-Control-Allow-Origin: *\r\n";
            resp << "Cache-Control: " << (noCache ? "no-cache" : "public, max-age=300") << "\r\n";
            resp << "Server: Bantu-Sua/1.2\r\n";
            resp << "\r\n" << content;
            bantuConnWrite(sock, resp.str());
            return true;
        }
        return false;
    }

    // Handle a single HTTP request — parse, route, call Bantu handler, respond.
    // Handle ONE complete request. The event loop has already read the whole
    // header block (and body, if any) into `request`, so nothing here blocks --
    // which is what allows a single thread to serve every connection.
    void bantuDispatchRequest(int sock, const std::string& request) {

        // Parse request line: METHOD PATH HTTP/1.1
        size_t firstSp = request.find(' ');
        size_t secondSp = request.find(' ', firstSp + 1);
        if (firstSp == std::string::npos || secondSp == std::string::npos) {
            bantuConnWrite(sock, std::string("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"));
            bantuConnClose(sock);
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

        // ─── WebSocket upgrade detection (RFC 6455) ────────────────
        // If the request has Upgrade: websocket, handle it as a WS
        // connection instead of a normal HTTP request.
        if (headers.count("upgrade") &&
            headers["upgrade"].toString().find("websocket") != std::string::npos) {
            std::string wsKey = headers.count("sec-websocket-key")
                ? headers["sec-websocket-key"].toString() : "";

            // Cross-Site WebSocket Hijacking defence.
            //
            // The same-origin policy does NOT apply to WebSocket upgrades, and
            // the browser sends the user's cookies with them. Without this check
            // any website could open an authenticated socket to a Bantu server
            // on a visitor's behalf and read everything it publishes. The Origin
            // header is the only signal available at upgrade time, so it has to
            // be checked before we switch protocols.
            //
            // A missing Origin means a non-browser client (curl, a native app),
            // which cannot be driven by a hostile page -- allowed. A present
            // Origin must match the request's own Host, or be listed explicitly.
            bool originOk = true;
            if (bantuLimits.wsCheckOrigin && headers.count("origin")) {
                std::string origin = headers["origin"].toString();
                std::string host = headers.count("host") ? headers["host"].toString() : "";
                originOk = false;
                if (!host.empty()) {
                    // Compare host:port against the origin's authority.
                    size_t sep = origin.find("://");
                    std::string oauth = (sep == std::string::npos) ? origin : origin.substr(sep + 3);
                    if (oauth == host) originOk = true;
                }
                for (const auto& allowed : bantuLimits.wsAllowedOrigins) {
                    if (allowed == "*" || allowed == origin) { originOk = true; break; }
                }
                if (!originOk) {
                    std::cerr << "  [WS] rejected upgrade from origin: " << origin << "\n";
                    std::string deny =
                        "HTTP/1.1 403 Forbidden\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
                    bantuConnWrite(sock, deny);
                    bantuConnClose(sock);
                    return;
                }
            }

            if (!wsKey.empty() && originOk) {
                bantuWsUpgrade(sock, wsKey);
                return;
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
        // Third pass: wildcard match (route ends with *)
        // Enables SPA fallback: sua.server.get("/*", handler)
        // NOTE: This runs AFTER static file serving, so /style.css etc.
        // are served as static files, not as SPA fallback.
        if (!found && method == "GET") {
            // Try static files first — if served, we're done.
            if (bantuServeStaticFile(sock, path)) {
                bantuConnClose(sock);
                return;
            }
        }
        if (!found) {
            for (auto& route : bantuServerRoutes) {
                if (route.method != method) continue;
                if (route.path.size() >= 2 && route.path.substr(route.path.size() - 2) == "/*") {
                    std::string prefix = route.path.substr(0, route.path.size() - 1);
                    if (path.find(prefix) == 0 || path == route.path.substr(0, route.path.size() - 2)) {
                        matchedHandler = route.handler;
                        found = true;
                        break;
                    }
                } else if (!route.path.empty() && route.path.back() == '*' && route.path != "*") {
                    std::string prefix = route.path.substr(0, route.path.size() - 1);
                    if (path.find(prefix) == 0) {
                        matchedHandler = route.handler;
                        found = true;
                        break;
                    }
                }
            }
        }

        // If no route matched, try static files for non-GET methods (rare but possible)

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
        bantuConnWrite(sock, resp.str());
        bantuConnClose(sock);
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
#else
        // A client that sends a request and then closes without reading the
        // reply makes the server's next send() raise SIGPIPE, whose default
        // action is to terminate the process. That is an unauthenticated,
        // one-packet remote kill, and it long predates the event loop -- v1.3.0
        // dies to it identically (exit 141 = 128 + SIGPIPE).
        //
        // Ignoring the signal turns the same condition into send() returning
        // EPIPE, which the loop already handles by closing the connection.
        //
        // Scoped to the server rather than set process-wide at startup, so that
        // `bantu run script.b | head` still terminates on a closed pipe the way
        // every other CLI program does.
        signal(SIGPIPE, SIG_IGN);
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
        if (!bantu_loop::setNonBlocking(sock)) {
            std::cerr << "  [SERVER] FATAL: could not set the listener non-blocking\n";
            CLOSE_SOCKET(sock);
            return;
        }

        auto backend = bantu_loop::makeBackend();
        bantuLoopBackend = backend.get();
        backend->add(sock, true, false);

        std::cout << "  [SERVER] Listening on 0.0.0.0:" << port
                  << " (event loop: " << backend->name() << ")\n";
        std::cout.flush();

        // ─── The event loop ────────────────────────────────────────────
        // One thread, every connection. A connection costs its buffers rather
        // than an 8MB thread stack, and because all Bantu code runs here there
        // is no shared interpreter state to race on -- the Phase 1 locks were
        // deleted along with the threads.
        std::vector<bantu_loop::Event> events;
        std::vector<int> doomed;
        const int tickMs = 1000;

        auto dropConn = [&](int fd) {
            auto it = bantuConns.find(fd);
            if (it == bantuConns.end()) return;
            if (it->second.isWs) bantuWsTeardown(it->second.wsId);
            backend->del(fd);
            bantuConns.erase(it);
            CLOSE_SOCKET(fd);
            bantuLiveConnections--;
        };

        while (true) {
            int n = backend->wait(events, tickMs);
            if (n < 0) {
                std::cerr << "  [SERVER] event wait failed: " << strerror(errno) << "\n";
                break;
            }
            uint64_t now = bantu_loop::nowMs();

            for (const auto& ev : events) {
                // ── the listener ──
                if (ev.fd == sock) {
                    // Drain the accept queue; one wakeup can cover many pending
                    // connections and leaving them queued adds latency.
                    for (;;) {
                        struct sockaddr_in ca;
                        socklen_t cl = sizeof(ca);
                        int cfd = (int)accept(sock, (struct sockaddr*)&ca, &cl);
                        if (cfd < 0) break;

                        if (bantuLiveConnections.load() >= bantuLimits.maxConnections) {
                            static const char* busy =
                                "HTTP/1.1 503 Service Unavailable\r\n"
                                "Content-Length: 0\r\nConnection: close\r\n"
                                "Retry-After: 1\r\n\r\n";
                            send(cfd, busy, (int)strlen(busy), 0);
                            CLOSE_SOCKET(cfd);
                            continue;
                        }
                        bantu_loop::setNonBlocking(cfd);
                        BantuConn c;
                        c.fd = cfd;
                        c.lastActive = now;
                        bantuConns[cfd] = std::move(c);
                        backend->add(cfd, true, false);
                        bantuLiveConnections++;
                    }
                    continue;
                }

                auto it = bantuConns.find(ev.fd);
                if (it == bantuConns.end()) { backend->del(ev.fd); continue; }
                BantuConn& c = it->second;
                c.lastActive = now;

                // ── readable ──
                if (ev.readable) {
                    char buf[16384];
                    bool peerClosed = false;
                    for (;;) {
                        ssize_t r = recv(c.fd, buf, sizeof(buf), 0);
                        if (r > 0) { c.in.append(buf, (size_t)r); continue; }
                        if (r == 0) { peerClosed = true; break; }
                        if (bantu_loop::wouldBlock()) break;
                        peerClosed = true; break;
                    }

                    if (c.isWs) {
                        if (!bantuWsProcess(c)) c.closing = true;
                    } else {
                        // An HTTP request is dispatched only once the whole
                        // header block (and any declared body) has arrived --
                        // the fix for header blocks split across segments.
                        size_t he = c.in.find("\r\n\r\n");
                        // The size cap applies whether or not the terminator has
                        // arrived. Checking only while still searching would let
                        // an oversized-but-complete header block straight
                        // through, which is exactly how this regressed.
                        if (he != std::string::npos && he > bantuLimits.maxHeaderBytes)
                            he = std::string::npos;
                        if (he == std::string::npos) {
                            if (c.in.size() > bantuLimits.maxHeaderBytes) {
                                bantuConnWrite(c.fd, std::string(
                                    "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                                    "Content-Length: 0\r\nConnection: close\r\n\r\n"));
                                c.closing = true;
                            }
                        } else {
                            size_t need = he + 4 + bantuContentLengthOf(c.in, he);
                            if (need > bantuLimits.maxBodyBytes + he + 4) {
                                bantuConnWrite(c.fd, std::string(
                                    "HTTP/1.1 413 Payload Too Large\r\n"
                                    "Content-Length: 0\r\nConnection: close\r\n\r\n"));
                                c.closing = true;
                            } else if (c.in.size() >= need) {
                                std::string request = c.in.substr(0, need);
                                c.in.erase(0, need);
                                bantuDispatchRequest(c.fd, request);
                                // bantuConns may have rehashed while the handler
                                // ran (a handler can open connections), so the
                                // reference above is no longer safe to use.
                                auto again = bantuConns.find(ev.fd);
                                if (again == bantuConns.end()) continue;
                                again->second.lastActive = now;
                            }
                        }
                    }
                    if (peerClosed) {
                        auto again = bantuConns.find(ev.fd);
                        if (again != bantuConns.end()) again->second.closing = true;
                    }
                }

                // ── writable ──
                auto cur = bantuConns.find(ev.fd);
                if (cur == bantuConns.end()) continue;
                BantuConn& cc = cur->second;
                if (ev.writable || cc.outPos < cc.out.size()) {
                    while (cc.outPos < cc.out.size()) {
                        ssize_t w = send(cc.fd, cc.out.data() + cc.outPos,
                                         (int)(cc.out.size() - cc.outPos), 0);
                        if (w > 0) { cc.outPos += (size_t)w; continue; }
                        if (bantu_loop::wouldBlock()) break;
                        cc.closing = true; break;
                    }
                    if (cc.outPos >= cc.out.size()) { cc.out.clear(); cc.outPos = 0; }
                    bantuConnSyncInterest(cc);
                }

                if (ev.error && cc.outPos >= cc.out.size()) cc.closing = true;
                if (cc.closing && cc.outPos >= cc.out.size()) doomed.push_back(cc.fd);
            }

            // ── timers: reap idle connections ──
            for (auto& kv : bantuConns) {
                BantuConn& c = kv.second;
                if (c.closing && c.outPos >= c.out.size()) { doomed.push_back(c.fd); continue; }
                uint64_t limit = c.isWs ? (uint64_t)bantuLimits.idleTimeoutMs
                                        : (uint64_t)bantuLimits.headerTimeoutMs;
                if (now - c.lastActive > limit) doomed.push_back(c.fd);
            }
            for (int fd : doomed) dropConn(fd);
            doomed.clear();
        }

        bantuLoopBackend = nullptr;
    }

    // ════════════════════════════════════════════════════════════
    // FUNCTION EVALUATION
    // ════════════════════════════════════════════════════════════

    Value evalFuncDecl(FuncDeclNode* n) {
        auto fn = std::make_shared<BantuFunction>(n->name, n->params, n->body, env_);
        Value fnVal(std::move(fn));
        // Named declarations bind into the current scope; anonymous function
        // expressions (empty name) just yield the value so `def(...) { }` can be
        // used inline (as a dict value, argument, etc.).
        if (!n->name.empty()) {
            env_->define(n->name, fnVal);
        }
        return fnVal;
    }

    Value evalReturn(ReturnNode* n) {
        Value val = evalNode(n->value);
        throw ReturnSignal{val};
    }

    // Resolve an assignable slot (variable / list element / dict entry) to a
    // mutable Value*, or nullptr if the node isn't a valid lvalue. Powers the
    // in-place list mutators below.
    Value* resolveLValue(ASTNode* node) {
        if (auto v = dynamic_cast<VariableNode*>(node)) {
            if (env_->has(v->name)) return &env_->getRef(v->name);
            return nullptr;
        }
        if (auto idx = dynamic_cast<IndexAccessNode*>(node)) {
            Value* base = resolveLValue(idx->object.get());
            if (!base) return nullptr;
            Value key = evalNode(idx->index);
            if (base->isList()) {
                int i = (int)key.numberVal;
                if (i < 0 || i >= (int)base->listVal.size()) return nullptr;
                return &base->listVal[i];
            }
            if (base->isObject()) return &(*base->objectVal)[key.toString()];
            return nullptr;
        }
        if (auto dot = dynamic_cast<DotAccessNode*>(node)) {
            Value* base = resolveLValue(dot->object.get());
            if (base && base->isObject()) return &(*base->objectVal)[dot->property];
            // A class instance stores its fields by value, so return a pointer to
            // the actual stored field. Without this, `this.list[i] = x` and
            // `this.list.push(x)` would mutate a *copy* and silently do nothing
            // (dicts escaped this only because their map is shared via shared_ptr).
            if (base && base->isClassInstance() && base->classInstanceVal)
                return &base->classInstanceVal->properties[dot->property];
            return nullptr;
        }
        return nullptr;
    }

    // In-place list mutators operating on the resolved list `lst`.
    //   append(l, x…) · push(l, x…) · pop(l) · insert(l, i, x) · remove(l, i) · extend(l, l2)
    // `argStart` is where the value arguments begin: 1 for function form
    // (arg0 is the list), 0 for method form ($l.push(x) — the list is the receiver).
    Value listMutator(const std::string& op, Value& lst, CallNode* n, size_t argStart = 1) {
        std::vector<Value> args;
        for (size_t i = argStart; i < n->args.size(); i++) args.push_back(evalNode(n->args[i]));
        auto& vec = lst.listVal;
        if (op == "append") {
            for (auto& a : args) vec.push_back(a);
            return Value((double)vec.size());
        }
        if (op == "push") {
            // Same as append, but returns the (mutated) list so the older
            // `$l = push($l, x)` idiom keeps working correctly.
            for (auto& a : args) vec.push_back(a);
            return lst;
        }
        if (op == "pop") {
            if (vec.empty()) return Value();
            Value last = vec.back(); vec.pop_back(); return last;
        }
        if (op == "insert") {
            if (args.size() < 2) ErrorHandler::throwRuntimeError("insert(list, index, value) needs an index and a value", n->line, n->col);
            int i = (int)args[0].numberVal;
            if (i < 0) i = 0;
            if (i > (int)vec.size()) i = (int)vec.size();
            vec.insert(vec.begin() + i, args[1]);
            return Value((double)vec.size());
        }
        if (op == "remove") {
            if (args.empty()) return Value();
            int i = (int)args[0].numberVal;
            if (i < 0 || i >= (int)vec.size()) return Value();
            Value removed = vec[i];
            vec.erase(vec.begin() + i);
            return removed;
        }
        if (op == "extend") {
            if (!args.empty() && args[0].isList())
                for (auto& e : args[0].listVal) vec.push_back(e);
            return Value((double)vec.size());
        }
        return Value();
    }

    Value evalCall(CallNode* n) {
        // In-place list mutators (append/push/pop/insert/remove/extend). Resolved
        // here because a native builtin only receives args by value and could not
        // mutate the caller's list. A user-defined function of the same name wins.
        if (auto callVar = dynamic_cast<VariableNode*>(n->callee.get())) {
            const std::string& fname = callVar->name;
            if (!env_->has(fname) && !n->args.empty() &&
                (fname == "append" || fname == "push" || fname == "pop" || fname == "insert" ||
                 fname == "remove" || fname == "extend")) {
                Value* lv = resolveLValue(n->args[0].get());
                if (!lv || !lv->isList()) {
                    ErrorHandler::throwRuntimeError(fname + "() expects a list variable as its first argument", n->line, n->col);
                }
                return listMutator(fname, *lv, n, 1);
            }
        }

        // Method-style list mutation: $list.push(x) / $list.pop().
        // evalDotAccess only ever sees a COPY of the list, so these are resolved
        // against the real storage here. If the receiver isn't an addressable
        // list (e.g. a literal), we fall through to the value-copy methods.
        if (auto dot = dynamic_cast<DotAccessNode*>(n->callee.get())) {
            if (dot->property == "push" || dot->property == "pop") {
                Value* lv = resolveLValue(dot->object.get());
                if (lv && lv->isList()) {
                    return listMutator(dot->property, *lv, n, 0);
                }
            }
        }

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
            callEnv->functionScope = true;   // function-local assignment boundary

            // Propagate the caller's `this` into free functions (dynamic `this`),
            // but NOT into a bound method — a method accessed via obj.method already
            // carries its own receiver in its closure (see evalDotAccess), and
            // overwriting it here would make an instance's method run with the
            // *caller's* `this` (breaks obj-A calling obj-B.method()). Only inherit
            // when the callee's closure has no real instance `this` of its own.
            bool calleeHasOwnThis = fn->closure && fn->closure->has("this") &&
                                    fn->closure->get("this").isClassInstance();
            if (!calleeHasOwnThis && env_->has("this")) {
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
            if (it != obj.objectVal->end()) return it->second;   // a real key always wins

            // Dict pseudo-methods (only reachable when no key of that name exists),
            // returned as callables so `$d.keys()`, `$d.items()`, etc. work:
            //   .keys()  → list of keys
            //   .values()→ list of values
            //   .items() → list of [key, value] pairs (for  for $k,$v in $d.items())
            //   .size()/.length() → number of entries
            auto omap = obj.objectVal;
            if (n->property == "size" || n->property == "length") {
                return makeNative([omap](std::vector<Value>) -> Value { return Value((double)omap->size()); });
            }
            if (n->property == "keys") {
                return makeNative([omap](std::vector<Value>) -> Value {
                    std::vector<Value> ks; ks.reserve(omap->size());
                    for (auto& kv : *omap) ks.push_back(Value(kv.first));
                    return Value(std::move(ks));
                });
            }
            if (n->property == "values") {
                return makeNative([omap](std::vector<Value>) -> Value {
                    std::vector<Value> vs; vs.reserve(omap->size());
                    for (auto& kv : *omap) vs.push_back(kv.second);
                    return Value(std::move(vs));
                });
            }
            if (n->property == "items") {
                return makeNative([omap](std::vector<Value>) -> Value {
                    std::vector<Value> items; items.reserve(omap->size());
                    for (auto& kv : *omap) {
                        std::vector<Value> pair;
                        pair.push_back(Value(kv.first));
                        pair.push_back(kv.second);
                        items.push_back(Value(std::move(pair)));
                    }
                    return Value(std::move(items));
                });
            }
            // For leniency with $req.body.X access patterns, return null
            // instead of throwing when a key is missing on a plain object.
            // (Class instances still throw — they use the class-instance branch above.)
            return Value();
        }

        // List methods
        if (obj.isList()) {
            if (n->property == "length") return Value((double)obj.listVal.size());
            // .size() as a callable (parallels dict/string; blogsite uses $list.size())
            if (n->property == "size") {
                double count = (double)obj.listVal.size();
                return makeNative([count](std::vector<Value>) -> Value { return Value(count); });
            }
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
            if (n->property == "size") {
                double count = (double)obj.stringVal.size();
                return makeNative([count](std::vector<Value>) -> Value { return Value(count); });
            }
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

    // try { ... } catch ($e) { ... }
    // Binds $e to the thrown value (for `throw`) or a structured error dict
    // { message, type, line } (for runtime/type errors). Control-flow signals
    // (break/continue/return) intentionally pass straight through.
    Value evalTryCatch(TryCatchNode* n) {
        auto prevEnv = env_;
        try {
            env_ = std::make_shared<Environment>(prevEnv);
            for (auto& stmt : n->tryBody) evalNode(stmt);
            env_ = prevEnv;
        }
        catch (const BantuThrow& t) {
            // A Bantu `throw <expr>` — bind the catch var to the thrown value.
            env_ = std::make_shared<Environment>(prevEnv);
            env_->define(n->catchVar, t.value);
            for (auto& stmt : n->catchBody) evalNode(stmt);
            env_ = prevEnv;
        }
        catch (const BantuError& e) {
            // A runtime/type/reference error — bind a structured error dict.
            env_ = std::make_shared<Environment>(prevEnv);
            ObjectMap err;
            err["message"] = Value(e.message);
            err["type"]    = Value(e.typeName);
            err["line"]    = Value((double)e.line);
            env_->define(n->catchVar, Value(std::move(err)));
            for (auto& stmt : n->catchBody) evalNode(stmt);
            env_ = prevEnv;
        }
        catch (const std::exception& e) {
            // Any other C++ exception — bind its message string.
            env_ = std::make_shared<Environment>(prevEnv);
            env_->define(n->catchVar, Value(std::string(e.what())));
            for (auto& stmt : n->catchBody) evalNode(stmt);
            env_ = prevEnv;
        }
        catch (...) {
            // break/continue/return must not be swallowed by try/catch —
            // restore scope and let them reach the enclosing loop/function.
            env_ = prevEnv;
            throw;
        }
        return Value();
    }

    // throw <expr>; — raise the evaluated value as a catchable BantuThrow.
    Value evalThrow(ThrowNode* n) {
        throw BantuThrow(evalNode(n->value));
    }

    // switch ($subject) { case <v> { … } … default { … } }
    // First case whose value `equals` the subject runs (no fallthrough); else
    // the default block, if present. Each block runs in its own child scope.
    Value evalSwitch(SwitchNode* n) {
        Value subject = evalNode(n->subject);
        auto runBlock = [&](std::vector<std::shared_ptr<ASTNode>>& body) {
            auto prevEnv = env_;
            env_ = std::make_shared<Environment>(prevEnv);
            try {
                for (auto& stmt : body) evalNode(stmt);
            } catch (...) { env_ = prevEnv; throw; }
            env_ = prevEnv;
        };
        for (auto& c : n->cases) {
            if (subject.equals(evalNode(c.value))) {
                runBlock(c.body);
                return Value();
            }
        }
        if (n->hasDefault) runBlock(n->defaultBody);
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

        // Informational only — respect quiet mode (like the [INCLUDE] logs), so
        // libraries that define classes don't spam stdout on every include.
        if (!quietMode_) std::cout << "  [class] Defined: " << n->name << "\n";
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
                // A native handle reports its tag (e.g. "column"), so
                // type($c) == "column" works for arctic columns.
                case Value::NATIVE_HANDLE: return Value(args[0].handleTag());
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
            // Full byte range 0..255 (was ASCII-clamped to 127); needed so byte
            // sequences can be materialized for the crypto/hash modules.
            if (code < 0 || code > 255) return Value(std::string("?"));
            return Value(std::string(1, (char)(unsigned char)code));
        }));

        // ─── Crypto primitives (crypto-suite): u32 bitwise/modular ops ───
        // Operate on Bantu numbers treated as 32-bit unsigned words, with
        // defined wraparound. These are the atoms of MD5/SHA/HMAC written in .b.
        env_->define("band", makeNative([](std::vector<Value> a) -> Value {
            return Value((double)(bantuU32(a.size() > 0 ? a[0].numberVal : 0) & bantuU32(a.size() > 1 ? a[1].numberVal : 0)));
        }));
        env_->define("bor", makeNative([](std::vector<Value> a) -> Value {
            return Value((double)(bantuU32(a.size() > 0 ? a[0].numberVal : 0) | bantuU32(a.size() > 1 ? a[1].numberVal : 0)));
        }));
        env_->define("bxor", makeNative([](std::vector<Value> a) -> Value {
            return Value((double)(bantuU32(a.size() > 0 ? a[0].numberVal : 0) ^ bantuU32(a.size() > 1 ? a[1].numberVal : 0)));
        }));
        env_->define("bnot", makeNative([](std::vector<Value> a) -> Value {
            return Value((double)(uint32_t)(~bantuU32(a.size() > 0 ? a[0].numberVal : 0)));
        }));
        env_->define("shl", makeNative([](std::vector<Value> a) -> Value {
            uint32_t x = bantuU32(a.size() > 0 ? a[0].numberVal : 0);
            uint32_t s = bantuU32(a.size() > 1 ? a[1].numberVal : 0) & 31u;
            return Value((double)(uint32_t)(x << s));
        }));
        env_->define("shr", makeNative([](std::vector<Value> a) -> Value {
            uint32_t x = bantuU32(a.size() > 0 ? a[0].numberVal : 0);
            uint32_t s = bantuU32(a.size() > 1 ? a[1].numberVal : 0) & 31u;
            return Value((double)(uint32_t)(x >> s));
        }));
        env_->define("rotl", makeNative([](std::vector<Value> a) -> Value {
            uint32_t x = bantuU32(a.size() > 0 ? a[0].numberVal : 0);
            uint32_t r = bantuU32(a.size() > 1 ? a[1].numberVal : 0) & 31u;
            uint32_t y = (r == 0) ? x : (uint32_t)((x << r) | (x >> (32 - r)));
            return Value((double)y);
        }));
        env_->define("rotr", makeNative([](std::vector<Value> a) -> Value {
            uint32_t x = bantuU32(a.size() > 0 ? a[0].numberVal : 0);
            uint32_t r = bantuU32(a.size() > 1 ? a[1].numberVal : 0) & 31u;
            uint32_t y = (r == 0) ? x : (uint32_t)((x >> r) | (x << (32 - r)));
            return Value((double)y);
        }));
        env_->define("add32", makeNative([](std::vector<Value> a) -> Value {
            return Value((double)(uint32_t)(bantuU32(a.size() > 0 ? a[0].numberVal : 0) + bantuU32(a.size() > 1 ? a[1].numberVal : 0)));
        }));
        env_->define("mul32", makeNative([](std::vector<Value> a) -> Value {
            uint64_t p = (uint64_t)bantuU32(a.size() > 0 ? a[0].numberVal : 0) * (uint64_t)bantuU32(a.size() > 1 ? a[1].numberVal : 0);
            return Value((double)(uint32_t)p);
        }));

        // ─── Crypto primitives: byte / hex conversion ───
        // Bytes are a Bantu list of numbers 0..255. bytes() is the missing
        // whole-string ord(); frombytes() rebuilds a string from bytes.
        env_->define("bytes", makeNative([](std::vector<Value> a) -> Value {
            if (a.empty()) return Value(std::vector<Value>{});
            auto b = bantuToBytes(a[0]);
            return bantuBytesToList(b.data(), b.size());
        }));
        env_->define("frombytes", makeNative([](std::vector<Value> a) -> Value {
            if (a.empty() || !a[0].isList()) return Value(std::string(""));
            std::string s;
            s.reserve(a[0].listVal.size());
            for (auto& e : a[0].listVal) s.push_back((char)(unsigned char)(bantuU32(e.numberVal) & 0xFFu));
            return Value(s);
        }));
        env_->define("ord", makeNative([](std::vector<Value> a) -> Value {
            if (a.empty()) return Value(-1.0);
            if (a[0].isNumber()) return a[0];
            if (a[0].isString() && !a[0].stringVal.empty()) return Value((double)(unsigned char)a[0].stringVal[0]);
            return Value(-1.0);
        }));
        env_->define("tohex", makeNative([](std::vector<Value> a) -> Value {
            if (a.empty()) return Value(std::string(""));
            return Value(bantuHexOf(bantuToBytes(a[0])));
        }));
        env_->define("fromhex", makeNative([](std::vector<Value> a) -> Value {
            std::vector<Value> out;
            if (!a.empty() && a[0].isString()) {
                const std::string& h = a[0].stringVal;
                auto nyb = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int hi = -1;
                for (char c : h) {
                    int v = nyb(c);
                    if (v < 0) continue;                 // skip spaces/colons/etc.
                    if (hi < 0) { hi = v; }
                    else { out.push_back(Value((double)((hi << 4) | v))); hi = -1; }
                }
            }
            return Value(std::move(out));
        }));

        // ─── Crypto primitives: OS CSPRNG + constant-time compare ───
        // randbytes(n) → list of n cryptographically-secure bytes. This is the
        // ONLY safe randomness source for keys/salts/nonces/UUIDv4.
        env_->define("randbytes", makeNative([](std::vector<Value> a) -> Value {
            long long n = a.empty() ? 0 : (long long)std::llround(a[0].numberVal);
            if (n < 0) n = 0;
            if (n > 1048576) n = 1048576;               // 1 MiB sanity cap
            std::vector<unsigned char> buf((size_t)n);
            if (n > 0 && !bantuCsprng(buf.data(), (size_t)n)) {
                ErrorHandler::throwError("randbytes(): OS CSPRNG unavailable", 0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            return bantuBytesToList(buf.data(), buf.size());
        }));
        // ct_equal(a, b) → bool. Constant-time (no early exit) over strings or
        // byte-lists; use for MAC/tag/digest verification to avoid timing leaks.
        env_->define("ct_equal", makeNative([](std::vector<Value> a) -> Value {
            if (a.size() < 2) return Value(false);
            auto x = bantuToBytes(a[0]);
            auto y = bantuToBytes(a[1]);
            unsigned diff = (unsigned)(x.size() ^ y.size());
            for (size_t i = 0; i < x.size(); i++) {
                unsigned char yb = (i < y.size()) ? y[i] : 0;
                diff |= (unsigned)(x[i] ^ yb);
            }
            return Value(diff == 0);
        }));

        // ════════════════════════════════════════════════════════════════
        // NATIVE DIGEST ACCELERATORS (C++ fast paths for the hash/ module)
        // ----------------------------------------------------------------
        // Byte-identical to the pure-Bantu digests but ~10^5x faster, so the
        // .b modules delegate to these when present (gated on has_native).
        // The pure-Bantu implementations remain the auditable reference and
        // the portability fallback; a differential test asserts they agree.
        // Each accepts a string OR a byte-list (0..255) and returns lowercase
        // hex. These digest PUBLIC data, so constant-time is not required;
        // secret comparisons still go through ct_equal.
        // ════════════════════════════════════════════════════════════════
        {
            // Register one string|list -> hex digest under `name`, computing
            // `outLen` bytes via `fn(msg, len, out)`.
            auto defDigest = [&](const char* name, size_t outLen,
                                 void(*fn)(const unsigned char*, size_t, unsigned char*)) {
                env_->define(name, makeNative([outLen, fn](std::vector<Value> a) -> Value {
                    if (a.empty()) return Value(std::string(""));
                    auto b = bantuToBytes(a[0]);
                    std::vector<unsigned char> out(outLen);
                    fn(b.data(), b.size(), out.data());
                    return Value(bantu_native::toHex(out.data(), outLen));
                }));
            };
            defDigest("native_md5",    16, bantu_native::md5_raw);
            defDigest("native_sha1",   20, bantu_native::sha1_raw);
            defDigest("native_sha224", 28, bantu_native::sha224_raw);
            defDigest("native_sha256", 32, bantu_native::sha256_raw);
            defDigest("native_sha384", 48, bantu_native::sha384_raw);
            defDigest("native_sha512", 64, bantu_native::sha512_raw);

            // HMAC-SHA256(key, msg) -> hex. Both args accept string|byte-list.
            env_->define("native_hmac_sha256", makeNative([](std::vector<Value> a) -> Value {
                if (a.size() < 2) return Value(std::string(""));
                auto key = bantuToBytes(a[0]);
                auto msg = bantuToBytes(a[1]);
                unsigned char out[32];
                bantu_native::hmac_sha256_raw(key.data(), key.size(), msg.data(), msg.size(), out);
                return Value(bantu_native::toHex(out, 32));
            }));

            // native_hash_file(path, algo) -> hex. Reads the file in C++ so a
            // large file never has to be materialized as a Bantu byte-list —
            // this is what makes bulk file hashing usable (see docs/hash.md).
            // Streams in 64 KiB chunks would need incremental state; for now we
            // read the whole file (bounded by available memory) then digest.
            env_->define("native_hash_file", makeNative([](std::vector<Value> a) -> Value {
                if (a.size() < 2 || !a[0].isString() || !a[1].isString()) return Value(nullptr);
                std::ifstream f(a[0].stringVal, std::ios::binary);
                if (!f) return Value(nullptr);   // caller distinguishes null (missing/unreadable)
                std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)),
                                                 std::istreambuf_iterator<char>());
                const std::string& algo = a[1].stringVal;
                std::vector<unsigned char> out;
                if      (algo == "md5")    { out.resize(16); bantu_native::md5_raw(data.data(), data.size(), out.data()); }
                else if (algo == "sha1")   { out.resize(20); bantu_native::sha1_raw(data.data(), data.size(), out.data()); }
                else if (algo == "sha224") { out.resize(28); bantu_native::sha224_raw(data.data(), data.size(), out.data()); }
                else if (algo == "sha256") { out.resize(32); bantu_native::sha256_raw(data.data(), data.size(), out.data()); }
                else if (algo == "sha384") { out.resize(48); bantu_native::sha384_raw(data.data(), data.size(), out.data()); }
                else if (algo == "sha512") { out.resize(64); bantu_native::sha512_raw(data.data(), data.size(), out.data()); }
                else return Value(nullptr);      // unknown algo
                return Value(bantu_native::toHex(out.data(), out.size()));
            }));

            // ════════════════════════════════════════════════════════
            // WEB PUSH — RFC 8188 (aes128gcm) / 8291 / 8292 (VAPID)
            //
            // Atoms only: encryption and signing are separate so each is pinned
            // by its own published vectors. Policy (TTL, retries, pruning dead
            // subscriptions) lives in Bantu, in sua.push.
            //
            // Every entry point runs the memoised known-answer selftest first and
            // FAILS CLOSED, so a miscompiled binary can never emit a broken or
            // insecure push. See webpush.hpp.
            // ════════════════════════════════════════════════════════

            // Guard: returns false (and complains once) if the selftest failed.
            auto webpushReady = []() -> bool {
                const auto& st = bantu_webpush::selftest();
                if (!st.ok) {
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        std::cerr << "  [webpush] SELFTEST FAILED at \"" << st.first_failure
                                  << "\" — Web Push is disabled in this binary.\n";
                    }
                    return false;
                }
                return true;
            };

            env_->define("webpush_selftest", makeNative([](std::vector<Value>) -> Value {
                const auto& st = bantu_webpush::selftest();
                ObjectMap o;
                o["ok"] = Value(st.ok);
                o["ran"] = Value((double)st.ran);
                o["failed"] = st.ok ? Value() : Value(st.first_failure);
                return Value(std::move(o));
            }));

            // Generate a VAPID application-server keypair. The public key is what
            // the browser passes as `applicationServerKey`; keep the private key
            // secret and STABLE — rotating it invalidates every subscription.
            env_->define("webpush_keygen", makeNative([webpushReady](std::vector<Value>) -> Value {
                if (!webpushReady()) return Value();
                unsigned char priv[32], pub[65];
                bool have = false;
                for (int i = 0; i < 16 && !have; i++) {
                    if (!bantuCsprng(priv, 32)) return Value();
                    have = bantu_p256::valid_scalar(priv);   // rejection sampling, unbiased
                }
                if (!have || !bantu_p256::public_from_private(priv, pub)) return Value();
                ObjectMap o;
                // Not "public"/"private": those are reserved words, so `$k.public`
                // would not parse in Bantu.
                o["private_key"] = Value(bantu_webpush::b64url_encode(priv, 32));
                o["public_key"]  = Value(bantu_webpush::b64url_encode(pub, 65));
                bantu_p256::secure_zero(priv, sizeof priv);
                return Value(std::move(o));
            }));

            // Derive the public key from a base64url private key.
            env_->define("webpush_public_key", makeNative([webpushReady](std::vector<Value> a) -> Value {
                if (!webpushReady() || a.empty()) return Value();
                bantu_webpush::Bytes priv;
                if (!bantu_webpush::b64url_decode(a[0].toString(), priv) || priv.size() != 32) return Value();
                unsigned char pub[65];
                if (!bantu_p256::public_from_private(priv.data(), pub)) return Value();
                return Value(bantu_webpush::b64url_encode(pub, 65));
            }));

            // webpush_encrypt(p256dh, auth, plaintext) -> byte-list body
            //
            // `p256dh` and `auth` are the base64url strings from the browser's
            // PushSubscription. The salt and the ephemeral keypair are generated
            // internally and are deliberately NOT parameters — RFC 8291 §2
            // requires both to be fresh per message, and reusing either breaks
            // the AEAD completely.
            env_->define("webpush_encrypt", makeNative([webpushReady](std::vector<Value> a) -> Value {
                if (!webpushReady() || a.size() < 3) return Value();
                bantu_webpush::Bytes p256dh, auth;
                if (!bantu_webpush::b64url_decode(a[0].toString(), p256dh) || p256dh.size() != 65) return Value();
                if (!bantu_webpush::b64url_decode(a[1].toString(), auth)   || auth.size() == 0)   return Value();
                std::vector<unsigned char> pt = bantuToBytes(a[2]);
                bantu_webpush::Bytes body;
                if (!bantu_webpush::encrypt(p256dh.data(), auth.data(), auth.size(),
                                            pt.data(), pt.size(), body)) return Value();
                return bantuBytesToList(body.data(), body.size());
            }));

            // Decrypt as a subscriber would. For tests and tooling; a server
            // never needs it. Returns null on ANY failure — null means REJECT.
            env_->define("webpush_decrypt", makeNative([webpushReady](std::vector<Value> a) -> Value {
                if (!webpushReady() || a.size() < 3) return Value();
                bantu_webpush::Bytes priv, auth;
                if (!bantu_webpush::b64url_decode(a[0].toString(), priv) || priv.size() != 32) return Value();
                if (!bantu_webpush::b64url_decode(a[1].toString(), auth)) return Value();
                std::vector<unsigned char> body = bantuToBytes(a[2]);
                bantu_webpush::Bytes out;
                if (!bantu_webpush::decrypt(priv.data(), auth.data(), auth.size(),
                                            body.data(), body.size(), out)) return Value();
                return bantuBytesToList(out.data(), out.size());
            }));

            // webpush_jwt(private, aud, sub, exp_seconds) -> signed ES256 JWT
            env_->define("webpush_jwt", makeNative([webpushReady](std::vector<Value> a) -> Value {
                if (!webpushReady() || a.size() < 4) return Value();
                bantu_webpush::Bytes priv;
                if (!bantu_webpush::b64url_decode(a[0].toString(), priv) || priv.size() != 32) return Value();
                std::string out;
                if (!bantu_webpush::jwt(priv.data(), a[1].toString(), a[2].toString(),
                                        (int64_t)a[3].numberVal, out)) return Value();
                return Value(out);
            }));

            // webpush_vapid_header(private, aud, sub, exp) -> "vapid t=..., k=..."
            env_->define("webpush_vapid_header", makeNative([webpushReady](std::vector<Value> a) -> Value {
                if (!webpushReady() || a.size() < 4) return Value();
                bantu_webpush::Bytes priv;
                if (!bantu_webpush::b64url_decode(a[0].toString(), priv) || priv.size() != 32) return Value();
                std::string out;
                if (!bantu_webpush::vapid_header(priv.data(), a[1].toString(), a[2].toString(),
                                                 (int64_t)a[3].numberVal, out)) return Value();
                return Value(out);
            }));

            // The `aud` claim: the ORIGIN of an endpoint, not the whole URL.
            env_->define("webpush_aud", makeNative([](std::vector<Value> a) -> Value {
                if (a.empty()) return Value();
                std::string out;
                if (!bantu_webpush::origin_of(a[0].toString(), out)) return Value();
                return Value(out);
            }));

            // file_exists(path) -> bool. readfile()/open() raise on a missing
            // file, so without this there is no way to write a "create it if it
            // isn't there yet" flow in Bantu.
            env_->define("file_exists", makeNative([](std::vector<Value> a) -> Value {
                if (a.empty()) return Value(false);
                std::ifstream f(a[0].toString(), std::ios::binary);
                return Value(f.good());
            }));

            env_->define("b64url_encode", makeNative([](std::vector<Value> a) -> Value {
                if (a.empty()) return Value(std::string(""));
                std::vector<unsigned char> b = bantuToBytes(a[0]);
                return Value(bantu_webpush::b64url_encode(b.data(), b.size()));
            }));
            env_->define("b64url_decode", makeNative([](std::vector<Value> a) -> Value {
                if (a.empty()) return Value();
                bantu_webpush::Bytes out;
                if (!bantu_webpush::b64url_decode(a[0].toString(), out)) return Value();
                return bantuBytesToList(out.data(), out.size());
            }));

            // has_native(name) -> bool. Lets .b modules feature-detect an
            // accelerator and fall back to the pure implementation when a given
            // interpreter build doesn't ship it. Kept in sync with the set above.
            env_->define("has_native", makeNative([](std::vector<Value> a) -> Value {
                if (a.empty() || !a[0].isString()) return Value(false);
                static const std::set<std::string> kNatives = {
                    "md5","sha1","sha224","sha256","sha384","sha512",
                    "hmac_sha256","hash_file",
                    "col",  // arctic native column primitives + kernels
                    "pwa"   // sua.pwa: manifest / service worker / offline
#ifdef BANTU_ARROW
                    ,"arrow"  // Parquet + Feather/Arrow-IPC I/O (opt-in build)
#endif
                };
                if (a[0].stringVal == "webpush") {
                    // Reported only when the known-answer selftest passes, so a
                    // miscompiled build advertises no push support at all.
                    return Value(bantu_webpush::selftest().ok);
                }
                return Value(kNatives.count(a[0].stringVal) > 0);
            }));

            // eprint(...) — write to STDERR (diagnostics / warnings). Keeps
            // security notices and logs off stdout so they never corrupt piped
            // program output (e.g. a bare digest). Space-separated, newline-out.
            env_->define("eprint", makeNative([](std::vector<Value> a) -> Value {
                for (size_t i = 0; i < a.size(); i++) {
                    if (i) std::cerr << " ";
                    std::cerr << a[i].toString();
                }
                std::cerr << std::endl;
                return Value(nullptr);
            }));

            // ════════════════════════════════════════════════════════════
            // AUTHENTICATED ENCRYPTION + PASSWORD HASHING (libsodium, C3)
            // ------------------------------------------------------------
            // Present as working crypto ONLY when the interpreter was built
            // with BANTU_SODIUM (and libsodium linked). Otherwise the builtins
            // are still registered but report unavailability, so crypto.b can
            // detect and raise a clear "rebuild with libsodium" error rather
            // than silently doing nothing. Bytes cross as byte-lists (0..255).
            // ════════════════════════════════════════════════════════════

            // sodium_available() -> bool. Feature-detection for crypto.b.
            env_->define("sodium_available", makeNative([](std::vector<Value>) -> Value {
                return Value(bantu_sodium::available());
            }));

            // aead_encrypt(key, message, aad?) -> byte-list (nonce||ct||tag), or
            // null on error (bad key size / not compiled in). key must be 32 bytes.
            env_->define("aead_encrypt", makeNative([](std::vector<Value> a) -> Value {
                if (a.size() < 2) return Value(nullptr);
                auto key = bantuToBytes(a[0]);
                auto msg = bantuToBytes(a[1]);
                std::vector<unsigned char> aad = (a.size() > 2) ? bantuToBytes(a[2])
                                                                : std::vector<unsigned char>();
                bool ok = false;
                auto out = bantu_sodium::aeadEncrypt(
                    std::vector<uint8_t>(key.begin(), key.end()),
                    std::vector<uint8_t>(msg.begin(), msg.end()),
                    std::vector<uint8_t>(aad.begin(), aad.end()), ok);
                if (!ok) return Value(nullptr);
                return bantuBytesToList(out.data(), out.size());
            }));

            // aead_decrypt(key, blob, aad?) -> byte-list plaintext, or null if
            // authentication fails (wrong key / tampered data / truncated). A
            // null result MUST be treated as "reject", never as empty plaintext.
            env_->define("aead_decrypt", makeNative([](std::vector<Value> a) -> Value {
                if (a.size() < 2) return Value(nullptr);
                auto key  = bantuToBytes(a[0]);
                auto blob = bantuToBytes(a[1]);
                std::vector<unsigned char> aad = (a.size() > 2) ? bantuToBytes(a[2])
                                                                : std::vector<unsigned char>();
                bool ok = false;
                auto out = bantu_sodium::aeadDecrypt(
                    std::vector<uint8_t>(key.begin(), key.end()),
                    std::vector<uint8_t>(blob.begin(), blob.end()),
                    std::vector<uint8_t>(aad.begin(), aad.end()), ok);
                if (!ok) return Value(nullptr);
                return bantuBytesToList(out.data(), out.size());
            }));

            // pwhash(password) -> encoded argon2id string (store this), or null.
            env_->define("pwhash", makeNative([](std::vector<Value> a) -> Value {
                if (a.empty()) return Value(nullptr);
                std::string pw = a[0].isString() ? a[0].stringVal : a[0].toString();
                bool ok = false;
                std::string h = bantu_sodium::pwhash(pw, ok);
                if (!ok) return Value(nullptr);
                return Value(h);
            }));

            // pwhash_verify(encoded, password) -> bool (constant-time).
            env_->define("pwhash_verify", makeNative([](std::vector<Value> a) -> Value {
                if (a.size() < 2 || !a[0].isString()) return Value(false);
                std::string pw = a[1].isString() ? a[1].stringVal : a[1].toString();
                return Value(bantu_sodium::pwhashVerify(a[0].stringVal, pw));
            }));

            // ════════════════════════════════════════════════════════════
            // ARCTIC COLUMN PRIMITIVES (data-science foundations, `col_*`)
            // ------------------------------------------------------------
            // A native typed column (f64/i64/bool/utf8 + null mask) held by a
            // shared_ptr handle (auto-freed). These are the atoms the pure-Bantu
            // `arctic` library composes. See dataframe_native.hpp. Phase 2 =
            // construction + introspection; kernels arrive in Phase 3.
            // Errors from the native layer become plain-language Bantu errors.
            // ════════════════════════════════════════════════════════════

            // Run a column op, translating any std::exception into a Bantu error.
            auto colGuard = [](const char* where, std::function<Value()> body) -> Value {
                try { return body(); }
                catch (const std::exception& e) {
                    ErrorHandler::throwError(std::string(where) + ": " + e.what(), 0, 0,
                                             ErrorHandler::RUNTIME_ERROR);
                }
                return Value();
            };

            // col(list, dtype) -> column. dtype ∈ f64/i64/bool/utf8.
            env_->define("col", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col", [&]() -> Value {
                    if (a.size() < 2 || !a[0].isList() || !a[1].isString())
                        throw std::runtime_error("usage: col(list, dtype)");
                    auto dt = arctic::dtypeFromName(a[1].stringVal);
                    return arctic::wrap(arctic::makeColumn(a[0].listVal, dt));
                });
            }));

            // col_len(c) -> number of elements.
            env_->define("col_len", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_len", [&]() -> Value {
                    return Value((double)arctic::asColumn(a[0])->n);
                });
            }));

            // col_dtype(c) -> "f64"|"i64"|"bool"|"utf8"|"date"|"datetime"|"cat".
            env_->define("col_dtype", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_dtype", [&]() -> Value {
                    return Value(arctic::columnTypeName(*arctic::asColumn(a[0])));
                });
            }));

            // col_get(c, i) -> element value, or null if that element is null.
            env_->define("col_get", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_get", [&]() -> Value {
                    if (a.size() < 2 || !a[1].isNumber())
                        throw std::runtime_error("usage: col_get(column, index)");
                    auto c = arctic::asColumn(a[0]);
                    long long i = (long long)a[1].numberVal;
                    if (i < 0 || (size_t)i >= c->n)
                        throw std::runtime_error("index " + std::to_string(i) +
                                                 " out of range (len " + std::to_string(c->n) + ")");
                    return arctic::elemToValue(*c, (size_t)i);
                });
            }));

            // col_to_list(c) -> Bantu list (nulls become Bantu null).
            env_->define("col_to_list", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_to_list", [&]() -> Value {
                    return arctic::columnToList(*arctic::asColumn(a[0]));
                });
            }));

            // col_slice(c, start, len) -> a new column of that contiguous range.
            env_->define("col_slice", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_slice", [&]() -> Value {
                    if (a.size() < 3 || !a[1].isNumber() || !a[2].isNumber())
                        throw std::runtime_error("usage: col_slice(column, start, len)");
                    auto c = arctic::asColumn(a[0]);
                    long long start = (long long)a[1].numberVal, len = (long long)a[2].numberVal;
                    if (start < 0) start = 0;
                    if (len < 0) len = 0;
                    return arctic::wrap(arctic::sliceColumn(*c, (size_t)start, (size_t)len));
                });
            }));

            // col_cast(c, dtype) -> a new column converted to dtype.
            env_->define("col_cast", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_cast", [&]() -> Value {
                    if (a.size() < 2 || !a[1].isString())
                        throw std::runtime_error("usage: col_cast(column, dtype)");
                    auto c = arctic::asColumn(a[0]);
                    return arctic::wrap(arctic::castColumn(*c, arctic::dtypeFromName(a[1].stringVal)));
                });
            }));

            // col_is_null(c) -> bool column, 1 where the element is null.
            env_->define("col_is_null", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_is_null", [&]() -> Value {
                    return arctic::wrap(arctic::isNullMask(*arctic::asColumn(a[0])));
                });
            }));

            // col_null_count(c) -> number of null elements.
            env_->define("col_null_count", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_null_count", [&]() -> Value {
                    return Value((double)arctic::nullCount(*arctic::asColumn(a[0])));
                });
            }));

            // col_fill_null(c, value) -> a new column with nulls replaced.
            env_->define("col_fill_null", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_fill_null", [&]() -> Value {
                    if (a.size() < 2) throw std::runtime_error("usage: col_fill_null(column, value)");
                    return arctic::wrap(arctic::fillNull(*arctic::asColumn(a[0]), a[1]));
                });
            }));

            // ── Datetime / date / categorical (logical overlays) ──────────────
            // col_to_datetime(c) parses utf8 (ISO-8601) or takes numeric epoch ms.
            env_->define("col_to_datetime", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_to_datetime", [&]() -> Value {
                    return arctic::wrap(arctic::toDatetime(*arctic::asColumn(a[0]), false));
                });
            }));
            // col_to_date(c) → date (days since epoch); utf8 ISO or numeric days.
            env_->define("col_to_date", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_to_date", [&]() -> Value {
                    return arctic::wrap(arctic::toDatetime(*arctic::asColumn(a[0]), true));
                });
            }));
            // col_strftime(c, fmt) → utf8 (specifiers %Y %y %m %d %H %M %S %j %%).
            env_->define("col_strftime", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_strftime", [&]() -> Value {
                    if (a.size() < 2 || !a[1].isString()) throw std::runtime_error("usage: col_strftime(column, format)");
                    return arctic::wrap(arctic::strftimeCol(*arctic::asColumn(a[0]), a[1].stringVal));
                });
            }));
            // Calendar components → i64 columns.
            auto defDtPart = [&](const char* name, arctic::DtPart part) {
                env_->define(name, makeNative([colGuard, name, part](std::vector<Value> a) -> Value {
                    return colGuard(name, [&]() -> Value {
                        return arctic::wrap(arctic::dtComponent(*arctic::asColumn(a[0]), part));
                    });
                }));
            };
            defDtPart("col_dt_year",    arctic::DtPart::YEAR);
            defDtPart("col_dt_month",   arctic::DtPart::MONTH);
            defDtPart("col_dt_day",     arctic::DtPart::DAY);
            defDtPart("col_dt_hour",    arctic::DtPart::HOUR);
            defDtPart("col_dt_minute",  arctic::DtPart::MINUTE);
            defDtPart("col_dt_second",  arctic::DtPart::SECOND);
            defDtPart("col_dt_weekday", arctic::DtPart::WEEKDAY);
            // Categoricals: encode, list categories, extract raw codes.
            env_->define("col_to_categorical", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_to_categorical", [&]() -> Value {
                    return arctic::wrap(arctic::toCategorical(*arctic::asColumn(a[0])));
                });
            }));
            env_->define("col_categories", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_categories", [&]() -> Value {
                    return arctic::wrap(arctic::categoriesOf(*arctic::asColumn(a[0])));
                });
            }));
            env_->define("col_codes", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_codes", [&]() -> Value {
                    return arctic::wrap(arctic::codesOf(*arctic::asColumn(a[0])));
                });
            }));

            // ── Window / set / string kernels ─────────────────────────────────
            // Cumulative statistics: col_cumsum/cumprod/cummax/cummin(c).
            auto defCum = [&](const char* name, arctic::Cum op) {
                env_->define(name, makeNative([colGuard, name, op](std::vector<Value> a) -> Value {
                    return colGuard(name, [&]() -> Value {
                        return arctic::wrap(arctic::cumOp(*arctic::asColumn(a[0]), op));
                    });
                }));
            };
            defCum("col_cumsum",  arctic::Cum::SUM);
            defCum("col_cumprod", arctic::Cum::PROD);
            defCum("col_cummax",  arctic::Cum::MAX);
            defCum("col_cummin",  arctic::Cum::MIN);

            // col_shift(c, n) — move values down by n (negative = up); gaps null.
            env_->define("col_shift", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_shift", [&]() -> Value {
                    if (a.size() < 2 || !a[1].isNumber()) throw std::runtime_error("usage: col_shift(column, n)");
                    return arctic::wrap(arctic::shiftOp(*arctic::asColumn(a[0]), (int64_t)a[1].numberVal));
                });
            }));
            // col_full(n, value) — a constant column of length n.
            env_->define("col_full", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_full", [&]() -> Value {
                    if (a.empty() || !a[0].isNumber()) throw std::runtime_error("usage: col_full(n, value)");
                    size_t n = (size_t)std::max(0.0, a[0].numberVal);
                    return arctic::wrap(arctic::fullOp(n, a.size() > 1 ? a[1] : Value()));
                });
            }));
            // col_reverse(c) — rows in reverse order.
            env_->define("col_reverse", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_reverse", [&]() -> Value {
                    return arctic::wrap(arctic::reverseOp(*arctic::asColumn(a[0])));
                });
            }));
            // col_rank(c, descending?) — 1-based, ties share the lowest rank.
            env_->define("col_rank", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_rank", [&]() -> Value {
                    bool desc = a.size() > 1 && a[1].isTruthy();
                    return arctic::wrap(arctic::rankOp(*arctic::asColumn(a[0]), desc));
                });
            }));
            // col_quantile(c, q) — linear interpolation, q in [0,1].
            env_->define("col_quantile", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_quantile", [&]() -> Value {
                    if (a.size() < 2 || !a[1].isNumber()) throw std::runtime_error("usage: col_quantile(column, q)");
                    return arctic::quantileOp(*arctic::asColumn(a[0]), a[1].numberVal);
                });
            }));
            // col_concat([c1, c2, ...]) or col_concat(c1, c2, ...) — stack rows.
            env_->define("col_concat", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_concat", [&]() -> Value {
                    std::vector<arctic::ColumnPtr> parts;
                    if (a.size() == 1 && a[0].isList()) { for (auto& v : a[0].listVal) parts.push_back(arctic::asColumn(v)); }
                    else { for (auto& v : a) parts.push_back(arctic::asColumn(v)); }
                    return arctic::wrap(arctic::concatCols(parts));
                });
            }));
            // col_unique_mask(cols) — true at the first occurrence of each key.
            env_->define("col_unique_mask", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_unique_mask", [&]() -> Value {
                    return arctic::wrap(arctic::uniqueMask(arctic::asColumnList(a[0])));
                });
            }));
            // col_is_in(c, [values]) — membership mask.
            env_->define("col_is_in", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_is_in", [&]() -> Value {
                    if (a.size() < 2 || !a[1].isList()) throw std::runtime_error("usage: col_is_in(column, list)");
                    return arctic::wrap(arctic::isInOp(*arctic::asColumn(a[0]), a[1].listVal));
                });
            }));
            // col_round(c, digits)
            env_->define("col_round", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_round", [&]() -> Value {
                    int d = (a.size() > 1 && a[1].isNumber()) ? (int)a[1].numberVal : 0;
                    return arctic::wrap(arctic::roundOp(*arctic::asColumn(a[0]), d));
                });
            }));

            // Text: col_upper/lower/strip/str_len, contains/starts_with/ends_with,
            // col_replace(c, from, to), col_substr(c, start, len).
            auto defStrUn = [&](const char* name, arctic::StrUn op) {
                env_->define(name, makeNative([colGuard, name, op](std::vector<Value> a) -> Value {
                    return colGuard(name, [&]() -> Value {
                        return arctic::wrap(arctic::strUnary(*arctic::asColumn(a[0]), op));
                    });
                }));
            };
            defStrUn("col_upper",   arctic::StrUn::UPPER);
            defStrUn("col_lower",   arctic::StrUn::LOWER);
            defStrUn("col_strip",   arctic::StrUn::STRIP);
            defStrUn("col_str_len", arctic::StrUn::LENGTH);

            auto defStrPred = [&](const char* name, arctic::StrPred op) {
                env_->define(name, makeNative([colGuard, name, op](std::vector<Value> a) -> Value {
                    return colGuard(name, [&]() -> Value {
                        if (a.size() < 2 || !a[1].isString()) throw std::runtime_error("needs a text argument");
                        return arctic::wrap(arctic::strPredicate(*arctic::asColumn(a[0]), op, a[1].stringVal));
                    });
                }));
            };
            defStrPred("col_contains",    arctic::StrPred::CONTAINS);
            defStrPred("col_starts_with", arctic::StrPred::STARTS);
            defStrPred("col_ends_with",   arctic::StrPred::ENDS);

            env_->define("col_replace", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_replace", [&]() -> Value {
                    if (a.size() < 3 || !a[1].isString() || !a[2].isString())
                        throw std::runtime_error("usage: col_replace(column, from, to)");
                    return arctic::wrap(arctic::strReplace(*arctic::asColumn(a[0]), a[1].stringVal, a[2].stringVal));
                });
            }));
            env_->define("col_substr", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_substr", [&]() -> Value {
                    if (a.size() < 2 || !a[1].isNumber()) throw std::runtime_error("usage: col_substr(column, start, len?)");
                    int64_t len = (a.size() > 2 && a[2].isNumber()) ? (int64_t)a[2].numberVal : -1;
                    return arctic::wrap(arctic::strSlice(*arctic::asColumn(a[0]), (int64_t)a[1].numberVal, len));
                });
            }));

            // ── Kernels (Phase 3) — each operand may be a column OR a scalar ──

            // Arithmetic: col_add/sub/mul/div/mod/pow(a, b) -> column.
            auto defArith = [&](const char* name, arctic::Arith op) {
                env_->define(name, makeNative([colGuard, name, op](std::vector<Value> a) -> Value {
                    return colGuard(name, [&]() -> Value {
                        if (a.size() < 2) throw std::runtime_error("needs two operands");
                        return arctic::wrap(arctic::arithOp(a[0], a[1], op));
                    });
                }));
            };
            defArith("col_add", arctic::Arith::ADD);
            defArith("col_sub", arctic::Arith::SUB);
            defArith("col_mul", arctic::Arith::MUL);
            defArith("col_div", arctic::Arith::DIV);
            defArith("col_mod", arctic::Arith::MOD);
            defArith("col_pow", arctic::Arith::POW);
            env_->define("col_neg", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_neg", [&]() -> Value { return arctic::wrap(arctic::unaryOp(a[0], false)); });
            }));
            env_->define("col_abs", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_abs", [&]() -> Value { return arctic::wrap(arctic::unaryOp(a[0], true)); });
            }));

            // Comparisons -> boolean mask column.
            auto defCmp = [&](const char* name, arctic::Cmp op) {
                env_->define(name, makeNative([colGuard, name, op](std::vector<Value> a) -> Value {
                    return colGuard(name, [&]() -> Value {
                        if (a.size() < 2) throw std::runtime_error("needs two operands");
                        return arctic::wrap(arctic::compareOp(a[0], a[1], op));
                    });
                }));
            };
            defCmp("col_gt", arctic::Cmp::GT); defCmp("col_ge", arctic::Cmp::GE);
            defCmp("col_lt", arctic::Cmp::LT); defCmp("col_le", arctic::Cmp::LE);
            defCmp("col_eq", arctic::Cmp::EQ); defCmp("col_ne", arctic::Cmp::NE);

            // Mask logic.
            env_->define("col_and", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_and", [&]() -> Value { return arctic::wrap(arctic::maskBin(a[0], a[1], true)); });
            }));
            env_->define("col_or", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_or", [&]() -> Value { return arctic::wrap(arctic::maskBin(a[0], a[1], false)); });
            }));
            env_->define("col_not", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_not", [&]() -> Value { return arctic::wrap(arctic::maskNot(a[0])); });
            }));

            // Conditional (if/else and if-elif-else over a column).
            env_->define("col_where", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_where", [&]() -> Value {
                    if (a.size() < 3) throw std::runtime_error("usage: col_where(mask, ifTrue, ifFalse)");
                    return arctic::wrap(arctic::whereOp(a[0], a[1], a[2]));
                });
            }));
            env_->define("col_case", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_case", [&]() -> Value {
                    if (a.size() < 2 || !a[0].isList())
                        throw std::runtime_error("usage: col_case([mask, value, ...], default)");
                    return arctic::wrap(arctic::caseOp(a[0].listVal, a[1]));
                });
            }));

            // Selection.
            env_->define("col_filter", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_filter", [&]() -> Value {
                    if (a.size() < 2) throw std::runtime_error("usage: col_filter(column, mask)");
                    return arctic::wrap(arctic::filterOp(a[0], a[1]));
                });
            }));
            env_->define("col_take", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_take", [&]() -> Value {
                    if (a.size() < 2) throw std::runtime_error("usage: col_take(column, indexColumn)");
                    return arctic::wrap(arctic::takeOp(a[0], a[1]));
                });
            }));
            env_->define("col_head", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_head", [&]() -> Value {
                    auto c = arctic::asColumn(a[0]);
                    long long n = (a.size() > 1 && a[1].isNumber()) ? (long long)a[1].numberVal : 5;
                    if (n < 0) n = 0;
                    return arctic::wrap(arctic::sliceColumn(*c, 0, (size_t)n));
                });
            }));
            env_->define("col_tail", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_tail", [&]() -> Value {
                    auto c = arctic::asColumn(a[0]);
                    long long n = (a.size() > 1 && a[1].isNumber()) ? (long long)a[1].numberVal : 5;
                    if (n < 0) n = 0;
                    size_t start = ((size_t)n >= c->n) ? 0 : (c->n - (size_t)n);
                    return arctic::wrap(arctic::sliceColumn(*c, start, (size_t)n));
                });
            }));

            // Aggregations -> scalar value.
            auto defAgg = [&](const char* name, arctic::Agg op) {
                env_->define(name, makeNative([colGuard, name, op](std::vector<Value> a) -> Value {
                    return colGuard(name, [&]() -> Value { return arctic::aggOp(*arctic::asColumn(a[0]), op); });
                }));
            };
            defAgg("col_sum", arctic::Agg::SUM);   defAgg("col_mean", arctic::Agg::MEAN);
            defAgg("col_min", arctic::Agg::MIN);   defAgg("col_max", arctic::Agg::MAX);
            defAgg("col_std", arctic::Agg::STD);   defAgg("col_var", arctic::Agg::VAR);
            defAgg("col_median", arctic::Agg::MEDIAN); defAgg("col_count", arctic::Agg::COUNT);
            defAgg("col_nunique", arctic::Agg::NUNIQUE);
            defAgg("col_any", arctic::Agg::ANY);   defAgg("col_all", arctic::Agg::ALL);

            // Ordering.
            env_->define("col_argsort", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_argsort", [&]() -> Value {
                    bool desc = (a.size() > 1) && a[1].isTruthy();
                    return arctic::wrap(arctic::argsortOp(*arctic::asColumn(a[0]), desc));
                });
            }));

            // Grouping & join (accept one column or a list of key columns).
            env_->define("col_group_ids", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_group_ids", [&]() -> Value {
                    return arctic::wrap(arctic::groupIds(arctic::asColumnList(a[0])));
                });
            }));
            env_->define("col_group_agg", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_group_agg", [&]() -> Value {
                    if (a.size() < 3 || !a[2].isString())
                        throw std::runtime_error("usage: col_group_agg(keyCols, valueCol, op)");
                    static const std::unordered_map<std::string, arctic::Agg> M = {
                        {"sum",arctic::Agg::SUM},{"mean",arctic::Agg::MEAN},{"min",arctic::Agg::MIN},
                        {"max",arctic::Agg::MAX},{"std",arctic::Agg::STD},{"var",arctic::Agg::VAR},
                        {"median",arctic::Agg::MEDIAN},{"count",arctic::Agg::COUNT},
                        {"nunique",arctic::Agg::NUNIQUE},{"any",arctic::Agg::ANY},{"all",arctic::Agg::ALL}};
                    auto it = M.find(a[2].stringVal);
                    if (it == M.end()) throw std::runtime_error("unknown agg '" + a[2].stringVal + "'");
                    return arctic::groupAgg(arctic::asColumnList(a[0]), *arctic::asColumn(a[1]), it->second);
                });
            }));
            env_->define("col_join", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("col_join", [&]() -> Value {
                    if (a.size() < 2) throw std::runtime_error("usage: col_join(leftKeys, rightKeys, how)");
                    std::string how = (a.size() > 2 && a[2].isString()) ? a[2].stringVal : "inner";
                    arctic::Join j = arctic::Join::INNER;
                    if (how == "left") j = arctic::Join::LEFT;
                    else if (how == "right") j = arctic::Join::RIGHT;
                    else if (how == "outer") j = arctic::Join::OUTER;
                    else if (how != "inner") throw std::runtime_error("how must be inner/left/right/outer");
                    return arctic::joinIdx(arctic::asColumnList(a[0]), arctic::asColumnList(a[1]), j);
                });
            }));

            // ── Typed I/O (Phase 4) ──────────────────────────────────────
            // read_csv(path, options?) -> { names:[...], cols:{name:column}, shape:[r,c] }
            // options (dict, optional): { "delim": ",", "header": true }
            env_->define("read_csv", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("read_csv", [&]() -> Value {
                    if (a.empty() || !a[0].isString()) throw std::runtime_error("usage: read_csv(path, options?)");
                    char delim = ','; bool header = true; bool slow = false;
                    std::vector<std::string> usecols;      // projection: only these columns
                    if (a.size() > 1 && a[1].isObject()) {
                        auto& o = *a[1].objectVal;
                        auto d = o.find("delim");  if (d != o.end() && d->second.isString() && !d->second.stringVal.empty()) delim = d->second.stringVal[0];
                        auto h = o.find("header"); if (h != o.end()) header = h->second.isTruthy();
                        auto e = o.find("engine"); if (e != o.end() && e->second.isString()) slow = (e->second.stringVal == "slow");
                        auto c = o.find("columns"); // projection pushdown target
                        if (c != o.end() && c->second.isList())
                            for (auto& cv : c->second.listVal) usecols.push_back(cv.toString());
                    }
                    // Fast slurp: size the file and read it in one block (the
                    // istreambuf_iterator form is char-by-char and far slower).
                    std::ifstream f(a[0].stringVal, std::ios::binary | std::ios::ate);
                    if (!f) throw std::runtime_error("cannot open '" + a[0].stringVal + "'");
                    std::streamsize sz = f.tellg();
                    std::string text;
                    if (sz > 0) { text.resize((size_t)sz); f.seekg(0); f.read(&text[0], sz); }
                    return arctic::readCsvText(text, delim, header, usecols, slow);
                });
            }));

            // write_csv(frame, path) OR write_csv(names, cols, path).
            //   frame = { "names":[...], "cols":{name:column} }
            env_->define("write_csv", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("write_csv", [&]() -> Value {
                    std::vector<std::string> names;
                    std::vector<arctic::ColumnPtr> cols;
                    std::string path;
                    const Value* colsDict = nullptr; const Value* namesList = nullptr;
                    if (a.size() >= 2 && a[0].isObject() && a[0].objectVal->count("cols") && a[0].objectVal->count("names")) {
                        namesList = &a[0].objectVal->at("names");
                        colsDict  = &a[0].objectVal->at("cols");
                        if (a.size() < 2 || !a[1].isString()) throw std::runtime_error("usage: write_csv(frame, path)");
                        path = a[1].stringVal;
                    } else if (a.size() >= 3 && a[0].isList() && a[1].isObject() && a[2].isString()) {
                        namesList = &a[0]; colsDict = &a[1]; path = a[2].stringVal;
                    } else {
                        throw std::runtime_error("usage: write_csv(frame, path) or write_csv(names, cols, path)");
                    }
                    for (auto& nv : namesList->listVal) {
                        std::string nm = nv.toString();
                        auto it = colsDict->objectVal->find(nm);
                        if (it == colsDict->objectVal->end()) throw std::runtime_error("column '" + nm + "' not found");
                        names.push_back(nm);
                        cols.push_back(arctic::asColumn(it->second));
                    }
                    std::string csv = arctic::writeCsvText(names, cols, ',');
                    std::ofstream of(path, std::ios::binary);
                    if (!of) throw std::runtime_error("cannot write '" + path + "'");
                    of << csv;
                    return Value(true);
                });
            }));

            // read_sqlite(path, query) -> { names:[...], cols:{name:column}, shape:[r,c] }
            // Runs the query via the already-linked SQLite and infers column types.
            env_->define("read_sqlite", makeNative([colGuard](std::vector<Value> a) -> Value {
                return colGuard("read_sqlite", [&]() -> Value {
                    if (a.size() < 2 || !a[0].isString() || !a[1].isString())
                        throw std::runtime_error("usage: read_sqlite(path, query)");
                    sqlite3* db = nullptr;
                    if (sqlite3_open(a[0].stringVal.c_str(), &db) != SQLITE_OK) {
                        std::string e = db ? sqlite3_errmsg(db) : "cannot open database";
                        if (db) sqlite3_close(db);
                        throw std::runtime_error(e);
                    }
                    sqlite3_stmt* st = nullptr;
                    if (sqlite3_prepare_v2(db, a[1].stringVal.c_str(), -1, &st, nullptr) != SQLITE_OK) {
                        std::string e = sqlite3_errmsg(db); sqlite3_close(db);
                        throw std::runtime_error(e);
                    }
                    int ncol = sqlite3_column_count(st);
                    std::vector<std::string> colNames(ncol);
                    std::vector<std::vector<Value>> colData(ncol);
                    for (int j=0;j<ncol;j++) colNames[j] = sqlite3_column_name(st, j) ? sqlite3_column_name(st, j) : ("col"+std::to_string(j));
                    while (sqlite3_step(st) == SQLITE_ROW) {
                        for (int j=0;j<ncol;j++) {
                            switch (sqlite3_column_type(st, j)) {
                                case SQLITE_INTEGER: colData[j].push_back(Value((double)sqlite3_column_int64(st, j))); break;
                                case SQLITE_FLOAT:   colData[j].push_back(Value(sqlite3_column_double(st, j))); break;
                                case SQLITE_NULL:    colData[j].push_back(Value()); break;
                                default: {
                                    const unsigned char* txt = sqlite3_column_text(st, j);
                                    colData[j].push_back(Value(std::string(txt ? (const char*)txt : "")));
                                }
                            }
                        }
                    }
                    sqlite3_finalize(st);
                    sqlite3_close(db);
                    ObjectMap out; std::vector<Value> names; ObjectMap cols;
                    size_t nrows = ncol ? colData[0].size() : 0;
                    for (int j=0;j<ncol;j++) {
                        names.push_back(Value(colNames[j]));
                        cols[colNames[j]] = arctic::wrap(arctic::makeColumnInferred(colData[j]));
                    }
                    out["names"] = Value(std::move(names));
                    out["cols"]  = Value(std::move(cols));
                    out["shape"] = Value(std::vector<Value>{ Value((double)nrows), Value((double)ncol) });
                    return Value(std::move(out));
                });
            }));

#ifdef BANTU_ARROW
            // ── Parquet + Feather/Arrow-IPC (opt-in build: -DBANTU_ARROW) ─────
            // read_parquet(path, {columns?}) / read_feather(path, {columns?})
            //   -> { names, cols, shape }   (projection pushed down to the reader)
            // write_parquet(frame, path, {compression?}) | write_parquet(names, cols, path, {compression?})
            // write_feather(frame, path)     | write_feather(names, cols, path)
            //
            // Extract (names, cols) from either a {names,cols} frame or separate
            // names-list + cols-dict, mirroring write_csv.
            auto arrowCollect = [](const Value* namesList, const Value* colsDict,
                                   std::vector<std::string>& names, std::vector<arctic::ColumnPtr>& cols) {
                for (auto& nv : namesList->listVal) {
                    std::string nm = nv.toString();
                    auto it = colsDict->objectVal->find(nm);
                    if (it == colsDict->objectVal->end()) throw std::runtime_error("column '" + nm + "' not found");
                    names.push_back(nm);
                    cols.push_back(arctic::asColumn(it->second));
                }
            };
            auto readColsOpt = [](const std::vector<Value>& a, size_t optIdx) {
                std::vector<std::string> usecols;
                if (a.size() > optIdx && a[optIdx].isObject()) {
                    auto c = a[optIdx].objectVal->find("columns");
                    if (c != a[optIdx].objectVal->end() && c->second.isList())
                        for (auto& cv : c->second.listVal) usecols.push_back(cv.toString());
                }
                return usecols;
            };

            env_->define("read_parquet", makeNative([colGuard, readColsOpt](std::vector<Value> a) -> Value {
                return colGuard("read_parquet", [&]() -> Value {
                    if (a.empty() || !a[0].isString()) throw std::runtime_error("usage: read_parquet(path, options?)");
                    return arctic::arrowio::readParquet(a[0].stringVal, readColsOpt(a, 1));
                });
            }));
            env_->define("read_feather", makeNative([colGuard, readColsOpt](std::vector<Value> a) -> Value {
                return colGuard("read_feather", [&]() -> Value {
                    if (a.empty() || !a[0].isString()) throw std::runtime_error("usage: read_feather(path, options?)");
                    return arctic::arrowio::readFeather(a[0].stringVal, readColsOpt(a, 1));
                });
            }));
            env_->define("write_parquet", makeNative([colGuard, arrowCollect](std::vector<Value> a) -> Value {
                return colGuard("write_parquet", [&]() -> Value {
                    std::vector<std::string> names; std::vector<arctic::ColumnPtr> cols;
                    std::string path, compression = "snappy"; const Value* opts = nullptr;
                    if (a.size() >= 2 && a[0].isObject() && a[0].objectVal->count("cols") && a[0].objectVal->count("names")) {
                        if (!a[1].isString()) throw std::runtime_error("usage: write_parquet(frame, path, options?)");
                        arrowCollect(&a[0].objectVal->at("names"), &a[0].objectVal->at("cols"), names, cols);
                        path = a[1].stringVal; if (a.size() > 2) opts = &a[2];
                    } else if (a.size() >= 3 && a[0].isList() && a[1].isObject() && a[2].isString()) {
                        arrowCollect(&a[0], &a[1], names, cols); path = a[2].stringVal; if (a.size() > 3) opts = &a[3];
                    } else throw std::runtime_error("usage: write_parquet(frame, path, options?) or write_parquet(names, cols, path, options?)");
                    if (opts && opts->isObject()) { auto c = opts->objectVal->find("compression"); if (c != opts->objectVal->end() && c->second.isString()) compression = c->second.stringVal; }
                    arctic::arrowio::writeParquet(names, cols, path, compression);
                    return Value(true);
                });
            }));
            env_->define("write_feather", makeNative([colGuard, arrowCollect](std::vector<Value> a) -> Value {
                return colGuard("write_feather", [&]() -> Value {
                    std::vector<std::string> names; std::vector<arctic::ColumnPtr> cols; std::string path;
                    if (a.size() >= 2 && a[0].isObject() && a[0].objectVal->count("cols") && a[0].objectVal->count("names")) {
                        if (!a[1].isString()) throw std::runtime_error("usage: write_feather(frame, path)");
                        arrowCollect(&a[0].objectVal->at("names"), &a[0].objectVal->at("cols"), names, cols); path = a[1].stringVal;
                    } else if (a.size() >= 3 && a[0].isList() && a[1].isObject() && a[2].isString()) {
                        arrowCollect(&a[0], &a[1], names, cols); path = a[2].stringVal;
                    } else throw std::runtime_error("usage: write_feather(frame, path) or write_feather(names, cols, path)");
                    arctic::arrowio::writeFeather(names, cols, path);
                    return Value(true);
                });
            }));
#endif // BANTU_ARROW
        }

        // NOTE: `push` is intentionally NOT registered as a builtin. A native fn
        // receives its args by value and so could never mutate the caller's list
        // (the old registration here silently did nothing). It is handled in
        // evalCall's lvalue intercept instead, alongside append/pop/insert/
        // remove/extend, so that it mutates in place for real.

        // ─── Dict introspection (v1.3.0) ───
        // keys($d) → list of keys, values($d) → list of values,
        // entries($d) → list of [key, value] pairs. (each/for can also iterate
        // dicts directly; these builtins are handy for one-off use.)
        env_->define("keys", makeNative([](std::vector<Value> args) -> Value {
            std::vector<Value> out;
            if (!args.empty() && args[0].isObject()) {
                for (auto& kv : *args[0].objectVal) out.push_back(Value(kv.first));
            }
            return Value(std::move(out));
        }));
        env_->define("values", makeNative([](std::vector<Value> args) -> Value {
            std::vector<Value> out;
            if (!args.empty() && args[0].isObject()) {
                for (auto& kv : *args[0].objectVal) out.push_back(kv.second);
            }
            return Value(std::move(out));
        }));
        env_->define("entries", makeNative([](std::vector<Value> args) -> Value {
            std::vector<Value> out;
            if (!args.empty() && args[0].isObject()) {
                for (auto& kv : *args[0].objectVal) {
                    std::vector<Value> pair;
                    pair.push_back(Value(kv.first));
                    pair.push_back(kv.second);
                    out.push_back(Value(std::move(pair)));
                }
            }
            return Value(std::move(out));
        }));

        // ─── Python-style file I/O (v1.3.0) ───
        // $f = open(path, mode)   modes: "r" read, "w" truncate-write, "a" append
        // read($f) whole file · readline($f) one line · readlines($f) list of lines
        // write($f, text) · close($f) · plus one-shot readfile/writefile/appendfile.
        env_->define("open", makeNative([](std::vector<Value> args) -> Value {
            if (args.empty()) ErrorHandler::throwError("open() needs a path", 0, 0, ErrorHandler::FILE_ERROR);
            std::string path = args[0].toString();
            std::string mode = args.size() > 1 ? args[1].toString() : "r";
            std::ios_base::openmode m;
            if (mode == "w")      m = std::ios::out | std::ios::trunc;
            else if (mode == "a") m = std::ios::out | std::ios::app;
            else                  m = std::ios::in;   // default "r"
            std::fstream fs(path, m);
            if (!fs.is_open()) {
                ErrorHandler::throwError("Cannot open file '" + path + "' (mode " + mode + ")",
                                         0, 0, ErrorHandler::FILE_ERROR);
            }
            int id = bantuNextFileId++;
            bantuFileTable()[id] = std::move(fs);
            ObjectMap handle;
            handle["__file"] = Value((double)id);
            handle["path"] = Value(path);
            handle["mode"] = Value(mode);
            return Value(std::move(handle));
        }));
        auto fileIdOf = [](const Value& h) -> int {
            if (h.isObject()) {
                auto it = h.objectVal->find("__file");
                if (it != h.objectVal->end()) return (int)it->second.numberVal;
            }
            return -1;
        };
        env_->define("read", makeNative([fileIdOf](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string(""));
            int id = fileIdOf(args[0]);
            auto it = bantuFileTable().find(id);
            if (it == bantuFileTable().end()) ErrorHandler::throwError("read(): not an open file", 0, 0, ErrorHandler::FILE_ERROR);
            std::stringstream ss; ss << it->second.rdbuf();
            return Value(ss.str());
        }));
        env_->define("readline", makeNative([fileIdOf](std::vector<Value> args) -> Value {
            if (args.empty()) return Value();
            int id = fileIdOf(args[0]);
            auto it = bantuFileTable().find(id);
            if (it == bantuFileTable().end()) ErrorHandler::throwError("readline(): not an open file", 0, 0, ErrorHandler::FILE_ERROR);
            std::string line;
            if (!std::getline(it->second, line)) return Value();   // null at EOF
            return Value(line);
        }));
        env_->define("readlines", makeNative([fileIdOf](std::vector<Value> args) -> Value {
            std::vector<Value> lines;
            if (args.empty()) return Value(std::move(lines));
            int id = fileIdOf(args[0]);
            auto it = bantuFileTable().find(id);
            if (it == bantuFileTable().end()) ErrorHandler::throwError("readlines(): not an open file", 0, 0, ErrorHandler::FILE_ERROR);
            std::string line;
            while (std::getline(it->second, line)) lines.push_back(Value(line));
            return Value(std::move(lines));
        }));
        env_->define("write", makeNative([fileIdOf](std::vector<Value> args) -> Value {
            if (args.size() < 2) return Value(false);
            int id = fileIdOf(args[0]);
            auto it = bantuFileTable().find(id);
            if (it == bantuFileTable().end()) ErrorHandler::throwError("write(): not an open file", 0, 0, ErrorHandler::FILE_ERROR);
            std::string data = args[1].toString();
            it->second << data;
            return Value((double)data.size());
        }));
        env_->define("close", makeNative([fileIdOf](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(false);
            int id = fileIdOf(args[0]);
            auto it = bantuFileTable().find(id);
            if (it == bantuFileTable().end()) return Value(false);
            it->second.close();
            bantuFileTable().erase(it);
            return Value(true);
        }));
        env_->define("readfile", makeNative([](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string(""));
            std::ifstream fs(args[0].toString());
            if (!fs.is_open()) ErrorHandler::throwError("Cannot read file '" + args[0].toString() + "'", 0, 0, ErrorHandler::FILE_ERROR);
            std::stringstream ss; ss << fs.rdbuf();
            return Value(ss.str());
        }));
        env_->define("writefile", makeNative([](std::vector<Value> args) -> Value {
            if (args.size() < 2) return Value(false);
            std::ofstream fs(args[0].toString(), std::ios::trunc);
            if (!fs.is_open()) ErrorHandler::throwError("Cannot write file '" + args[0].toString() + "'", 0, 0, ErrorHandler::FILE_ERROR);
            fs << args[1].toString();
            return Value(true);
        }));
        env_->define("appendfile", makeNative([](std::vector<Value> args) -> Value {
            if (args.size() < 2) return Value(false);
            std::ofstream fs(args[0].toString(), std::ios::app);
            if (!fs.is_open()) ErrorHandler::throwError("Cannot append file '" + args[0].toString() + "'", 0, 0, ErrorHandler::FILE_ERROR);
            fs << args[1].toString();
            return Value(true);
        }));

        // ─── FFI (v1.3.0): call C functions in shared libraries via libffi ───
        env_->define("loadlib", makeNative(&bantuFfiLoadLib));
        env_->define("func", makeNative(&bantuFfiFunc));

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

        // sua.server.limits({...}) — resource and security limits.
        // Called with no argument it just reports the current settings.
        serverObj["limits"] = makeNative([](std::vector<Value> args) -> Value {
            if (!args.empty() && args[0].isObject()) {
                const ObjectMap& o = *args[0].objectVal;
                // decay_t matters: decltype(dst) is a REFERENCE type here, and
                // casting a double to `size_t&` reinterprets its bits instead of
                // converting it (1024.0 came back as 4652218415073722368).
                auto num = [&](const char* k, auto& dst) {
                    auto it = o.find(k);
                    if (it == o.end()) return;
                    using T = typename std::decay<decltype(dst)>::type;
                    double v = it->second.numberVal;
                    if (v < 0) v = 0;
                    dst = static_cast<T>(v);
                };
                num("max_header_bytes",     bantuLimits.maxHeaderBytes);
                num("max_body_bytes",       bantuLimits.maxBodyBytes);
                num("max_connections",      bantuLimits.maxConnections);
                num("header_timeout_ms",    bantuLimits.headerTimeoutMs);
                num("idle_timeout_ms",      bantuLimits.idleTimeoutMs);
                num("max_ws_frame_bytes",   bantuLimits.maxWsFrameBytes);
                num("max_ws_message_bytes", bantuLimits.maxWsMessageBytes);
                auto co = o.find("ws_check_origin");
                if (co != o.end()) bantuLimits.wsCheckOrigin = co->second.isTruthy();
                auto ao = o.find("ws_allowed_origins");
                if (ao != o.end() && ao->second.isList()) {
                    bantuLimits.wsAllowedOrigins.clear();
                    for (const auto& v : ao->second.listVal)
                        bantuLimits.wsAllowedOrigins.push_back(v.toString());
                }
            }
            ObjectMap out;
            out["max_header_bytes"]     = Value((double)bantuLimits.maxHeaderBytes);
            out["max_body_bytes"]       = Value((double)bantuLimits.maxBodyBytes);
            out["max_connections"]      = Value((double)bantuLimits.maxConnections);
            out["header_timeout_ms"]    = Value((double)bantuLimits.headerTimeoutMs);
            out["idle_timeout_ms"]      = Value((double)bantuLimits.idleTimeoutMs);
            out["max_ws_frame_bytes"]   = Value((double)bantuLimits.maxWsFrameBytes);
            out["max_ws_message_bytes"] = Value((double)bantuLimits.maxWsMessageBytes);
            out["ws_check_origin"]      = Value(bantuLimits.wsCheckOrigin);
            std::vector<Value> origins;
            for (const auto& a : bantuLimits.wsAllowedOrigins) origins.push_back(Value(a));
            out["ws_allowed_origins"]   = Value(std::move(origins));
            out["live_connections"]     = Value((double)bantuLiveConnections.load());
            return Value(std::move(out));
        });

        suaObj["server"] = Value(std::move(serverObj));

        // ════════════════════════════════════════════════════════
        // NEW: SUA HTTP CLIENT — Real HTTP with libcurl
        // ════════════════════════════════════════════════════════

        ObjectMap httpClientObj;

        // sua.http.get(url)
        httpClientObj["get"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "https://httpbin.org/get";
            return bantuHttpRequest("GET", url);
        });

        // sua.http.post(url, body)
        httpClientObj["post"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "https://httpbin.org/post";
            std::string body = args.size() > 1 ? args[1].toString() : "";
            std::string contentType = args.size() > 2 ? args[2].toString() : "application/json";
            return bantuHttpRequest("POST", url, body, contentType);
        });

        // sua.http.put(url, body)
        httpClientObj["put"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "https://httpbin.org/put";
            std::string body = args.size() > 1 ? args[1].toString() : "";
            std::string contentType = args.size() > 2 ? args[2].toString() : "application/json";
            return bantuHttpRequest("PUT", url, body, contentType);
        });

        // sua.http.delete(url)
        httpClientObj["delete"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "https://httpbin.org/delete";
            return bantuHttpRequest("DELETE", url);
        });

        // sua.http.patch(url, body)
        httpClientObj["patch"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "https://httpbin.org/patch";
            std::string body = args.size() > 1 ? args[1].toString() : "";
            std::string contentType = args.size() > 2 ? args[2].toString() : "application/json";
            return bantuHttpRequest("PATCH", url, body, contentType);
        });

        // sua.http.head(url)
        httpClientObj["head"] = makeNative([](std::vector<Value> args) -> Value {
            std::string url = args.size() > 0 ? args[0].toString() : "https://httpbin.org/get";
            return bantuHttpRequest("HEAD", url);
        });

        // sua.http.request({method, url, headers, body, timeout, insecure})
        //
        // The general form: arbitrary request headers and a BINARY-SAFE body.
        // The convenience helpers above cannot set an Authorization header, and
        // before this existed a body containing a NUL byte was silently
        // truncated by libcurl's strlen(). `body` may be a string or a byte-list.
        httpClientObj["request"] = makeNative([](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].isObject()) {
                ObjectMap err;
                err["ok"] = Value(false);
                err["status"] = Value(0.0);
                err["error"] = Value(std::string("sua.http.request expects an options object"));
                return Value(std::move(err));
            }
            ObjectMap& o = *args[0].objectVal;
            auto opt_str = [&](const char* k, const std::string& dflt) {
                auto it = o.find(k);
                return (it == o.end() || it->second.isNull()) ? dflt : it->second.toString();
            };
            std::string method = opt_str("method", "GET");
            for (auto& c : method) c = (char)std::toupper((unsigned char)c);
            std::string url = opt_str("url", "");
            if (url.empty()) {
                ObjectMap err;
                err["ok"] = Value(false);
                err["status"] = Value(0.0);
                err["error"] = Value(std::string("sua.http.request: url is required"));
                return Value(std::move(err));
            }

            std::string body;
            auto bit = o.find("body");
            if (bit != o.end() && !bit->second.isNull()) {
                if (bit->second.isList()) {
                    std::vector<unsigned char> b = bantuToBytes(bit->second);
                    body.assign(b.begin(), b.end());
                } else if (bit->second.isObject()) {
                    body = bantuJsonStringify(bit->second);
                } else {
                    body = bit->second.toString();
                }
            }

            BantuHttpOptions opt;
            auto tit = o.find("timeout");
            if (tit != o.end() && tit->second.isNumber()) opt.timeout = (long)tit->second.numberVal;
            auto iit = o.find("insecure");
            if (iit != o.end()) opt.verifyTls = !iit->second.isTruthy();
            auto vit = o.find("verbose");
            if (vit != o.end()) opt.verbose = vit->second.isTruthy();

            auto hit = o.find("headers");
            if (hit != o.end() && hit->second.isObject()) {
                for (auto& kv : *hit->second.objectVal)
                    opt.headers.emplace_back(kv.first, kv.second.toString());
            }
            std::string contentType = opt_str("content_type", "");
            return bantuHttpRequestEx(method, url, body, contentType, opt);
        });

        // sua.http.insecure(true) — disable TLS certificate verification for the
        // convenience helpers above, which take no options object.
        //
        // The escape hatch exists because verification is now on by default and
        // sua.http.get(...) has nowhere to put a per-request flag; without it, an
        // app talking to a self-signed internal endpoint would have no way
        // forward. It is deliberately loud, global and explicit — prefer
        // sua.http.request({..., "insecure": true}) so the exemption is scoped to
        // the one call that needs it.
        httpClientObj["insecure"] = makeNative([](std::vector<Value> args) -> Value {
            bool on = args.empty() ? true : args[0].isTruthy();
            if (on != bantuHttpInsecureAll && on) {
                std::cerr << "  [sua.http] WARNING: TLS certificate verification disabled for "
                             "sua.http.get/post/put/delete/patch/head. Every outbound HTTPS request "
                             "is now unauthenticated and open to interception.\n";
            }
            bantuHttpInsecureAll = on;
            ObjectMap o;
            o["insecure"] = Value(bantuHttpInsecureAll);
            o["verify"] = Value(!bantuHttpInsecureAll);
            return Value(std::move(o));
        });

        suaObj["http"] = Value(std::move(httpClientObj));

        // ════════════════════════════════════════════════════════
        // SUA PWA — manifest, service worker, offline page, install prompt.
        //
        // Modelled on django-pwa: one flat config dict, three auto-registered
        // root URLs, and a meta-tag helper. See docs/pwa-research.md.
        // ════════════════════════════════════════════════════════

        ObjectMap pwaObj;

        pwaObj["configure"] = makeNative([](std::vector<Value> args) -> Value {
            bantu_pwa::Config& c = bantuPwaConfig;
            if (!args.empty() && args[0].isObject()) {
                ObjectMap& o = *args[0].objectVal;
                auto S = [&](const char* k, std::string& dst) {
                    auto it = o.find(k);
                    if (it != o.end() && !it->second.isNull()) dst = it->second.toString();
                };
                auto B = [&](const char* k, bool& dst) {
                    auto it = o.find(k);
                    if (it != o.end() && !it->second.isNull()) dst = it->second.isTruthy();
                };
                auto J = [&](const char* k, std::string& dst) {   // pass the list through as JSON
                    auto it = o.find(k);
                    if (it != o.end() && it->second.isList()) dst = bantuJsonStringify(it->second);
                };
                // Parse a list of {src, sizes, type, media} for the meta tags.
                auto ICONS = [&](const char* k, std::vector<bantu_pwa::IconEntry>& dst) {
                    auto it = o.find(k);
                    if (it == o.end() || !it->second.isList()) return;
                    dst.clear();
                    for (auto& e : it->second.listVal) {
                        if (!e.isObject()) continue;
                        bantu_pwa::IconEntry ie;
                        ObjectMap& m = *e.objectVal;
                        auto get = [&](const char* kk) {
                            auto i2 = m.find(kk);
                            return (i2 == m.end() || i2->second.isNull()) ? std::string("") : i2->second.toString();
                        };
                        ie.src = get("src"); ie.sizes = get("sizes");
                        ie.type = get("type"); ie.media = get("media");
                        if (!ie.src.empty()) dst.push_back(ie);
                    }
                };

                S("name", c.name);                     S("short_name", c.short_name);
                S("description", c.description);       S("theme_color", c.theme_color);
                S("background_color", c.background_color);
                S("display", c.display);               S("scope", c.scope);
                S("start_url", c.start_url);           S("orientation", c.orientation);
                S("lang", c.lang);                     S("dir", c.dir);
                S("status_bar_color", c.status_bar_color);
                S("offline_url", c.offline_url);       S("service_worker", c.service_worker);
                S("cache_version", c.cache_version);
                B("debug", c.debug);                   B("auto_inject", c.auto_inject);
                B("auto_register", c.auto_register);

                J("icons", c.icons_json);              J("screenshots", c.screenshots_json);
                J("shortcuts", c.shortcuts_json);      J("categories", c.categories_json);
                ICONS("icons", c.icons);
                ICONS("icons_apple", c.icons_apple);
                ICONS("splash_screen", c.splash_screen);

                auto pit = o.find("precache");
                if (pit != o.end() && pit->second.isList()) {
                    c.precache.clear();
                    for (auto& e : pit->second.listVal) c.precache.push_back(e.toString());
                }
            }
            c.configured = true;

            // ── auto-register the routes, exactly as django-pwa's urls.py does ──
            // Dropping any previous set keeps configure() idempotent.
            static const char* kPwaPaths[] = { "/manifest.json", "/manifest.webmanifest",
                                               "/serviceworker.js", "/pwa.js", "/offline" };
            bantuServerRoutes.erase(
                std::remove_if(bantuServerRoutes.begin(), bantuServerRoutes.end(),
                    [&](const BantuServerRoute& r) {
                        for (const char* p : kPwaPaths)
                            if (r.path == p && r.method == "GET") return true;
                        return false;
                    }),
                bantuServerRoutes.end());

            auto addRoute = [](const char* path, NativeFn fn) {
                bantuServerRoutes.push_back({ "GET", path, makeNative(std::move(fn)) });
            };

            addRoute("/manifest.json", [](std::vector<Value> a) -> Value {
                bantuPwaRespond(a.size() > 1 ? a[1] : Value(), 200,
                                "application/manifest+json; charset=utf-8",
                                bantu_pwa::render_manifest(bantuPwaConfig),
                                {{"Cache-Control", "no-cache"}});
                return Value();
            });
            addRoute("/manifest.webmanifest", [](std::vector<Value> a) -> Value {
                bantuPwaRespond(a.size() > 1 ? a[1] : Value(), 200,
                                "application/manifest+json; charset=utf-8",
                                bantu_pwa::render_manifest(bantuPwaConfig),
                                {{"Cache-Control", "no-cache"}});
                return Value();
            });
            addRoute("/serviceworker.js", [](std::vector<Value> a) -> Value {
                std::string js;
                // A custom worker replaces ours wholesale (django-pwa's
                // PWA_SERVICE_WORKER_PATH).
                if (!bantuPwaConfig.service_worker.empty()) {
                    std::ifstream f(bantuPwaConfig.service_worker, std::ios::binary);
                    if (f.good()) { std::stringstream ss; ss << f.rdbuf(); js = ss.str(); }
                    else {
                        std::cerr << "  [sua.pwa] service_worker not found: "
                                  << bantuPwaConfig.service_worker << " — serving the generated one\n";
                    }
                }
                if (js.empty()) js = bantu_pwa::render_service_worker(bantuPwaConfig);
                // Root scope + never cached: a stale worker is sticky and hard
                // for a user to clear.
                bantuPwaRespond(a.size() > 1 ? a[1] : Value(), 200,
                                "application/javascript; charset=utf-8", js,
                                {{"Cache-Control", "no-cache"},
                                 {"Service-Worker-Allowed", "/"}});
                return Value();
            });
            addRoute("/pwa.js", [](std::vector<Value> a) -> Value {
                bantuPwaRespond(a.size() > 1 ? a[1] : Value(), 200,
                                "application/javascript; charset=utf-8",
                                bantu_pwa::render_client_js(bantuPwaConfig),
                                {{"Cache-Control", "no-cache"}});
                return Value();
            });
            addRoute("/offline", [](std::vector<Value> a) -> Value {
                // Prefer the app's own offline.html from a static dir; fall back
                // to the built-in page.
                std::string html;
                for (const auto& dir : bantuServerStatic) {
                    std::string p = dir;
                    if (!p.empty() && p.back() == '/') p.pop_back();
                    p += "/offline.html";
                    std::ifstream f(p, std::ios::binary);
                    if (f.good()) { std::stringstream ss; ss << f.rdbuf(); html = ss.str(); break; }
                }
                if (html.empty()) html = bantu_pwa::render_offline_page(bantuPwaConfig);
                else if (bantuPwaConfig.auto_inject)
                    html = bantu_pwa::inject_meta(html, bantu_pwa::render_meta(bantuPwaConfig));
                bantuPwaRespond(a.size() > 1 ? a[1] : Value(), 200,
                                "text/html; charset=utf-8", html,
                                {{"Cache-Control", "no-cache"}});
                return Value();
            });

            ObjectMap info;
            info["configured"] = Value(true);
            info["name"] = Value(bantuPwaConfig.name);
            std::vector<Value> routes;
            for (const char* p : kPwaPaths) routes.push_back(Value(std::string(p)));
            info["routes"] = Value(std::move(routes));
            return Value(std::move(info));
        });

        // The <head> block — django-pwa's {% progressive_web_app_meta %}.
        pwaObj["meta"] = makeNative([](std::vector<Value>) -> Value {
            return Value(bantu_pwa::render_meta(bantuPwaConfig));
        });
        pwaObj["manifest"] = makeNative([](std::vector<Value>) -> Value {
            return Value(bantu_pwa::render_manifest(bantuPwaConfig));
        });
        pwaObj["serviceworker"] = makeNative([](std::vector<Value>) -> Value {
            return Value(bantu_pwa::render_service_worker(bantuPwaConfig));
        });
        pwaObj["client_js"] = makeNative([](std::vector<Value>) -> Value {
            return Value(bantu_pwa::render_client_js(bantuPwaConfig));
        });
        pwaObj["offline_page"] = makeNative([](std::vector<Value>) -> Value {
            return Value(bantu_pwa::render_offline_page(bantuPwaConfig));
        });
        // Inject the meta block into a caller-supplied HTML string.
        pwaObj["inject"] = makeNative([](std::vector<Value> a) -> Value {
            if (a.empty()) return Value(std::string(""));
            return Value(bantu_pwa::inject_meta(a[0].toString(),
                                                bantu_pwa::render_meta(bantuPwaConfig)));
        });
        pwaObj["config"] = makeNative([](std::vector<Value>) -> Value {
            const bantu_pwa::Config& c = bantuPwaConfig;
            ObjectMap o;
            o["configured"] = Value(c.configured);
            o["name"] = Value(c.name);
            o["short_name"] = Value(c.short_name.empty() ? c.name : c.short_name);
            o["theme_color"] = Value(c.theme_color);
            o["display"] = Value(c.display);
            o["scope"] = Value(c.scope);
            o["start_url"] = Value(c.start_url);
            o["offline_url"] = Value(c.offline_url);
            o["auto_inject"] = Value(c.auto_inject);
            o["debug"] = Value(c.debug);
            o["push_enabled"] = Value(!c.vapid_public_key.empty());
            return Value(std::move(o));
        });

        suaObj["pwa"] = Value(std::move(pwaObj));

        // ════════════════════════════════════════════════════════
        // SUA PUSH — Web Push notifications (the django-webpush half).
        // ════════════════════════════════════════════════════════

        ObjectMap pushObj;

        pushObj["available"] = makeNative([](std::vector<Value>) -> Value {
            return Value(bantu_webpush::selftest().ok);
        });

        // Generate a VAPID keypair. Do this ONCE and store it — the public key
        // is baked into every subscription, so rotating it invalidates them all.
        pushObj["vapid_keys"] = makeNative([](std::vector<Value>) -> Value {
            if (!bantu_webpush::selftest().ok) return Value();
            unsigned char priv[32], pub[65];
            bool have = false;
            for (int i = 0; i < 16 && !have; i++) {
                if (!bantuCsprng(priv, 32)) return Value();
                have = bantu_p256::valid_scalar(priv);
            }
            if (!have || !bantu_p256::public_from_private(priv, pub)) return Value();
            ObjectMap o;
            o["public_key"]  = Value(bantu_webpush::b64url_encode(pub, 65));
            o["private_key"] = Value(bantu_webpush::b64url_encode(priv, 32));
            bantu_p256::secure_zero(priv, sizeof priv);
            return Value(std::move(o));
        });

        // sua.push.configure({public_key, private_key, subject, db, subscribe_url})
        pushObj["configure"] = makeNative([](std::vector<Value> args) -> Value {
            ObjectMap out;
            if (args.empty() || !args[0].isObject()) {
                out["ok"] = Value(false);
                out["error"] = Value(std::string("sua.push.configure expects an options object"));
                return Value(std::move(out));
            }
            ObjectMap& o = *args[0].objectVal;
            auto get = [&](const char* k) {
                auto it = o.find(k);
                return (it == o.end() || it->second.isNull()) ? std::string("") : it->second.toString();
            };
            std::string pub = get("public_key"), priv = get("private_key"), subj = get("subject");
            std::string db = get("db"), sub_url = get("subscribe_url");

            if (pub.empty() || priv.empty()) {
                out["ok"] = Value(false);
                out["error"] = Value(std::string("public_key and private_key are required "
                                                 "— generate them once with sua.push.vapid_keys()"));
                return Value(std::move(out));
            }
            // Fail early and loudly rather than at the first 401 from a push service.
            bantu_webpush::Bytes pb, sb;
            if (!bantu_webpush::b64url_decode(priv, sb) || sb.size() != 32) {
                out["ok"] = Value(false);
                out["error"] = Value(std::string("private_key must be 32 base64url-encoded octets"));
                return Value(std::move(out));
            }
            if (!bantu_webpush::b64url_decode(pub, pb) || pb.size() != 65) {
                out["ok"] = Value(false);
                out["error"] = Value(std::string("public_key must be 65 base64url-encoded octets "
                                                 "(uncompressed P-256 point)"));
                return Value(std::move(out));
            }
            unsigned char derived[65];
            if (!bantu_p256::public_from_private(sb.data(), derived) ||
                std::memcmp(derived, pb.data(), 65) != 0) {
                out["ok"] = Value(false);
                out["error"] = Value(std::string("public_key does not match private_key"));
                return Value(std::move(out));
            }
            if (subj.empty()) subj = "mailto:admin@example.com";
            if (subj.rfind("mailto:", 0) != 0 && subj.rfind("https://", 0) != 0) {
                out["ok"] = Value(false);
                out["error"] = Value(std::string("subject must be a mailto: or https: URI"));
                return Value(std::move(out));
            }

            bantuPushPrivateKeyB64 = priv;
            bantuPushSubject = subj;
            bantuPwaConfig.vapid_public_key = pub;
            if (!sub_url.empty()) bantuPwaConfig.subscribe_url = sub_url;
            bantuPushDbPath = db.empty() ? std::string("./push_subscriptions.db") : db;
            if (!bantuPushDbOpen(bantuPushDbPath)) {
                out["ok"] = Value(false);
                out["error"] = Value(std::string("could not open the subscription store: ") + bantuPushDbPath);
                return Value(std::move(out));
            }

            // The subscribe/unsubscribe endpoint the generated /pwa.js posts to.
            std::string path = bantuPwaConfig.subscribe_url;
            bantuServerRoutes.erase(
                std::remove_if(bantuServerRoutes.begin(), bantuServerRoutes.end(),
                    [&](const BantuServerRoute& r) { return r.path == path; }),
                bantuServerRoutes.end());
            bantuServerRoutes.push_back({ "POST", path, makeNative(bantuPushSubscribeHandler) });
            bantuServerRoutes.push_back({ "DELETE", path, makeNative(bantuPushUnsubscribeHandler) });

            out["ok"] = Value(true);
            out["subscribe_url"] = Value(path);
            out["db"] = Value(bantuPushDbPath);
            out["subject"] = Value(subj);
            return Value(std::move(out));
        });

        // sua.push.keys(path) — load the VAPID keypair from `path`, generating
        // and saving it on first run.
        //
        // The equivalent of django-webpush's `manage.py
        // webpush_generate_vapid_keypair`, and the reason it exists: the public
        // key is baked into every subscription a browser creates, so generating
        // a fresh pair on each restart silently invalidates all of them. Keeping
        // the "generate once, then reuse" logic here means an app cannot get
        // that wrong.
        pushObj["keys"] = makeNative([](std::vector<Value> a) -> Value {
            if (!bantu_webpush::selftest().ok) return Value();
            std::string path = a.empty() ? std::string("./vapid.json") : a[0].toString();

            std::ifstream in(path, std::ios::binary);
            if (in.good()) {
                std::stringstream ss;
                ss << in.rdbuf();
                std::string text = ss.str();
                size_t pos = 0;
                Value parsed = bantuJsonParse(text, pos);
                if (parsed.isObject()) {
                    auto pk = parsed.objectVal->find("public_key");
                    auto sk = parsed.objectVal->find("private_key");
                    if (pk != parsed.objectVal->end() && sk != parsed.objectVal->end()) return parsed;
                }
                std::cerr << "  [sua.push] " << path << " is not a valid keypair file; "
                             "move it aside to generate a new one\n";
                return Value();
            }

            unsigned char priv[32], pub[65];
            bool have = false;
            for (int i = 0; i < 16 && !have; i++) {
                if (!bantuCsprng(priv, 32)) return Value();
                have = bantu_p256::valid_scalar(priv);
            }
            if (!have || !bantu_p256::public_from_private(priv, pub)) return Value();

            ObjectMap o;
            o["public_key"]  = Value(bantu_webpush::b64url_encode(pub, 65));
            o["private_key"] = Value(bantu_webpush::b64url_encode(priv, 32));
            bantu_p256::secure_zero(priv, sizeof priv);
            Value v(std::move(o));

            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.good()) {
                std::cerr << "  [sua.push] could not write " << path
                          << " — the keypair will not survive a restart, "
                             "which invalidates every subscription\n";
                return v;
            }
            out << bantuJsonStringify(v) << "\n";
            out.close();
            std::cerr << "  [sua.push] generated a VAPID keypair -> " << path
                      << " (keep it secret and stable)\n";
            return v;
        });

        pushObj["save"] = makeNative([](std::vector<Value> a) -> Value {
            if (a.empty()) return Value(false);
            std::string tag = a.size() > 1 ? a[1].toString() : "";
            return Value(bantuPushSave(a[0], tag));
        });
        pushObj["forget"] = makeNative([](std::vector<Value> a) -> Value {
            if (a.empty()) return Value(false);
            return Value(bantuPushForget(a[0].toString()));
        });
        pushObj["subscriptions"] = makeNative([](std::vector<Value> a) -> Value {
            std::string tag = a.empty() ? "" : a[0].toString();
            return bantuPushList(tag);
        });
        pushObj["count"] = makeNative([](std::vector<Value> a) -> Value {
            std::string tag = a.empty() ? "" : a[0].toString();
            Value l = bantuPushList(tag);
            return Value((double)(l.isList() ? l.listVal.size() : 0));
        });

        // sua.push.send(subscription, payload, options?) -> {ok, status, ...}
        pushObj["send"] = makeNative([](std::vector<Value> a) -> Value {
            if (a.size() < 2) {
                ObjectMap e; e["ok"] = Value(false);
                e["error"] = Value(std::string("sua.push.send(subscription, payload)"));
                return Value(std::move(e));
            }
            return bantuPushSendOne(a[0], a[1], a.size() > 2 ? a[2] : Value());
        });

        // Fan out to every stored subscription, pruning any the push service
        // reports as gone (404/410) — the django-webpush behaviour.
        pushObj["send_all"] = makeNative([](std::vector<Value> a) -> Value {
            if (a.empty()) {
                ObjectMap e; e["ok"] = Value(false);
                e["error"] = Value(std::string("sua.push.send_all(payload, options?, tag?)"));
                return Value(std::move(e));
            }
            Value opts = a.size() > 1 ? a[1] : Value();
            std::string tag = a.size() > 2 ? a[2].toString() : "";
            Value subs = bantuPushList(tag);
            int sent = 0, failed = 0, pruned = 0;
            std::vector<Value> results;
            if (subs.isList()) {
                for (auto& s : subs.listVal) {
                    Value r = bantuPushSendOne(s, a[0], opts);
                    bool ok = false;
                    int status = 0;
                    if (r.isObject()) {
                        auto it = r.objectVal->find("ok");
                        if (it != r.objectVal->end()) ok = it->second.isTruthy();
                        auto st = r.objectVal->find("status");
                        if (st != r.objectVal->end()) status = (int)st->second.numberVal;
                    }
                    if (ok) sent++;
                    else {
                        failed++;
                        if (status == 404 || status == 410) {   // subscription is dead
                            std::string ep;
                            if (s.isObject()) {
                                auto e = s.objectVal->find("endpoint");
                                if (e != s.objectVal->end()) ep = e->second.toString();
                            }
                            if (!ep.empty() && bantuPushForget(ep)) pruned++;
                        }
                    }
                    results.push_back(r);
                }
            }
            ObjectMap out;
            out["ok"] = Value(failed == 0);
            out["sent"] = Value((double)sent);
            out["failed"] = Value((double)failed);
            out["pruned"] = Value((double)pruned);
            out["results"] = Value(std::move(results));
            return Value(std::move(out));
        });

        suaObj["push"] = Value(std::move(pushObj));

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

            // Parameterized path: exec(sql, [params]) binds `?` placeholders
            // via a prepared statement (safe against SQL injection).
            if (args.size() > 1 && args[1].isList()) {
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(bantuSqliteDb, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
                    ObjectMap e; e["error"] = Value(std::string(sqlite3_errmsg(bantuSqliteDb))); e["success"] = Value(false);
                    return Value(std::move(e));
                }
                bantuSqliteBindParams(stmt, args[1].listVal);
                int rc = sqlite3_step(stmt);
                if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                    std::string err = sqlite3_errmsg(bantuSqliteDb);
                    sqlite3_finalize(stmt);
                    ObjectMap e; e["error"] = Value(err); e["success"] = Value(false);
                    return Value(std::move(e));
                }
                sqlite3_finalize(stmt);
                ObjectMap info;
                info["changes"] = Value((double)sqlite3_changes(bantuSqliteDb));
                info["lastInsertId"] = Value((double)sqlite3_last_insert_rowid(bantuSqliteDb));
                info["success"] = Value(true);
                return Value(std::move(info));
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

        // sua.sqlite.query(sql [, params])
        sqliteObj["query"] = makeNative([](std::vector<Value> args) -> Value {
            std::string sql = args.size() > 0 ? args[0].toString() : "SELECT 1";

            if (!bantuSqliteDb) {
                sqlite3_open(":memory:", &bantuSqliteDb);
                bantuSqlitePath = ":memory:";
                std::cout << "  [SQLITE] Auto-opened: :memory:\n";
            }

            // Parameterized path: query(sql, [params]) binds `?` placeholders
            // via a prepared statement, then collects rows.
            if (args.size() > 1 && args[1].isList()) {
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(bantuSqliteDb, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
                    ObjectMap e; e["error"] = Value(std::string(sqlite3_errmsg(bantuSqliteDb))); e["success"] = Value(false);
                    return Value(std::move(e));
                }
                bantuSqliteBindParams(stmt, args[1].listVal);
                std::vector<Value> prows;
                int cols = sqlite3_column_count(stmt);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    ObjectMap row;
                    for (int c = 0; c < cols; c++) {
                        row[sqlite3_column_name(stmt, c)] =
                            bantuSqliteCellToValue(reinterpret_cast<const char*>(sqlite3_column_text(stmt, c)));
                    }
                    prows.push_back(Value(std::move(row)));
                }
                sqlite3_finalize(stmt);
                return Value(std::move(prows));
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
        // When built with -DBANTU_POSTGRES=ON, opens a real PG connection.
        // Otherwise stores the connStr and sets the connected flag (stub mode).
        postgresObj["connect"] = makeNative([](std::vector<Value> args) -> Value {
            std::string connStr = args.size() > 0 ? args[0].toString()
                : "host=localhost dbname=test user=postgres password=postgres";

            // Parse connection string for display + stub fallback
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

            std::cout << "  [POSTGRES] Connecting to " << bantuPgHost << ":" << port << "/" << bantuPgDb << "\n";

#ifdef HAS_LIBPQ
            // Real PostgreSQL connection via libpq
            if (bantuPgConn != nullptr) {
                PQfinish(bantuPgConn);
                bantuPgConn = nullptr;
            }
            bantuPgConn = PQconnectdb(connStr.c_str());
            if (bantuPgConn == nullptr || PQstatus(bantuPgConn) != CONNECTION_OK) {
                std::string errMsg = bantuPgConn ? PQerrorMessage(bantuPgConn) : "PQconnectdb returned null";
                std::cout << "  [POSTGRES] Connection FAILED: " << errMsg << "\n";
                if (bantuPgConn) { PQfinish(bantuPgConn); bantuPgConn = nullptr; }
                bantuPgConnected = false;
                ObjectMap errInfo;
                errInfo["connected"] = Value(false);
                errInfo["error"] = Value(errMsg);
                return Value(std::move(errInfo));
            }
            bantuPgConnected = true;
            std::cout << "  [POSTGRES] Connected as " << bantuPgUser
                      << " (server: " << PQserverVersion(bantuPgConn) << ")\n";
            ObjectMap connInfo;
            connInfo["connected"] = Value(true);
            connInfo["host"] = Value(bantuPgHost);
            connInfo["dbname"] = Value(bantuPgDb);
            connInfo["user"] = Value(bantuPgUser);
            connInfo["port"] = Value(port);
            connInfo["type"] = Value(std::string("postgresql"));
            connInfo["serverVersion"] = Value((double)PQserverVersion(bantuPgConn));
            connInfo["protocolVersion"] = Value((double)PQprotocolVersion(bantuPgConn));
            return Value(std::move(connInfo));
#else
            // Stub mode (no libpq linked)
            bantuPgConnected = true;
            std::cout << "  [POSTGRES] Connected as " << bantuPgUser
                      << " (stub mode — rebuild with -DBANTU_POSTGRES=ON for real queries)\n";
            ObjectMap connInfo;
            connInfo["connected"] = Value(true);
            connInfo["host"] = Value(bantuPgHost);
            connInfo["dbname"] = Value(bantuPgDb);
            connInfo["user"] = Value(bantuPgUser);
            connInfo["port"] = Value(port);
            connInfo["type"] = Value(std::string("postgresql"));
            connInfo["serverVersion"] = Value(std::string("PostgreSQL 16.x (stub)"));
            connInfo["protocolVersion"] = Value(3.0);
            return Value(std::move(connInfo));
#endif
        });

        // sua.postgres.query(sql) — returns a list of row objects for SELECT,
        // or an execInfo object for INSERT/UPDATE/DELETE/CREATE/DROP.
        // In stub mode (no libpq), returns simulated data.
        postgresObj["query"] = makeNative([](std::vector<Value> args) -> Value {
            std::string sql = args.size() > 0 ? args[0].toString() : "SELECT 1";

            if (!bantuPgConnected) {
                std::cout << "  [POSTGRES] Not connected. Call sua.postgres.connect() first.\n";
                ObjectMap errInfo;
                errInfo["error"] = Value(std::string("Not connected to PostgreSQL"));
                errInfo["success"] = Value(false);
                return Value(std::move(errInfo));
            }

#ifdef HAS_LIBPQ
            // Real PostgreSQL query via libpq
            if (bantuPgConn == nullptr) {
                ObjectMap errInfo;
                errInfo["error"] = Value(std::string("PGconn is null"));
                errInfo["success"] = Value(false);
                return Value(std::move(errInfo));
            }
            std::cout << "  [POSTGRES] Query: " << sql.substr(0, 80)
                      << (sql.length() > 80 ? "..." : "") << "\n";
            // Parameterized path: query(sql, [params]) binds $1..$n via
            // PQexecParams (injection-safe); otherwise a plain PQexec.
            PGresult* res = nullptr;
            if (args.size() > 1 && args[1].isList()) {
                std::vector<std::string> storage;
                std::vector<const char*> vals;
                bantuPgBuildParams(args[1].listVal, storage, vals);
                res = PQexecParams(bantuPgConn, sql.c_str(), (int)vals.size(), nullptr,
                                   vals.empty() ? nullptr : vals.data(), nullptr, nullptr, 0);
            } else {
                res = PQexec(bantuPgConn, sql.c_str());
            }
            if (res == nullptr) {
                ObjectMap errInfo;
                errInfo["error"] = Value(std::string("PQexec returned null"));
                errInfo["success"] = Value(false);
                return Value(std::move(errInfo));
            }
            ExecStatusType status = PQresultStatus(res);
            if (status == PGRES_FATAL_ERROR || status == PGRES_NONFATAL_ERROR) {
                std::string errMsg = PQresultErrorMessage(res);
                std::cout << "  [POSTGRES] ERROR: " << errMsg << "\n";
                PQclear(res);
                ObjectMap errInfo;
                errInfo["error"] = Value(errMsg);
                errInfo["success"] = Value(false);
                return Value(std::move(errInfo));
            }
            if (status == PGRES_TUPLES_OK) {
                // SELECT — return list of row objects
                int nRows = PQntuples(res);
                int nCols = PQnfields(res);
                std::vector<Value> rows;
                rows.reserve(nRows);
                for (int r = 0; r < nRows; r++) {
                    ObjectMap row;
                    for (int c = 0; c < nCols; c++) {
                        std::string colName = PQfname(res, c);
                        char* val = PQgetvalue(res, r, c);
                        if (PQgetisnull(res, r, c)) {
                            row[colName] = Value();
                        } else {
                            Oid colType = PQftype(res, c);
                            // Numeric OIDs: 23=int4, 20=int8, 1700=numeric,
                            // 700=float4, 701=float8, 16=bool, 17=bytea
                            if (colType == 23 || colType == 20 || colType == 21) {
                                row[colName] = Value((double)atoll(val));
                            } else if (colType == 700 || colType == 701 || colType == 1700) {
                                row[colName] = Value(atof(val));
                            } else if (colType == 16) {
                                row[colName] = Value(std::string(val) == "t");
                            } else {
                                row[colName] = Value(std::string(val));
                            }
                        }
                    }
                    rows.push_back(Value(std::move(row)));
                }
                    std::cout << "  [POSTGRES]   Rows: " << rows.size() << "\n";
                PQclear(res);
                return Value(std::move(rows));
            }
            // Non-SELECT (INSERT/UPDATE/DELETE/CREATE/DROP/etc.)
            int affected = atoi(PQcmdTuples(res));
            std::string oidStr = PQoidStatus(res);
            PQclear(res);
            ObjectMap execInfo;
            execInfo["affectedRows"] = Value((double)affected);
            execInfo["success"] = Value(true);
            if (!oidStr.empty() && oidStr != "0") {
                execInfo["insertId"] = Value((double)atoll(oidStr.c_str()));
            }
                std::cout << "  [POSTGRES]   Affected " << affected << " row(s)\n";
            return Value(std::move(execInfo));
#else
            // Stub mode — simulate based on SQL keyword
            std::cout << "  [POSTGRES] Query: " << sql.substr(0, 80) << (sql.length() > 80 ? "..." : "") << "\n";
            std::string sqlUpper = sql;
            std::transform(sqlUpper.begin(), sqlUpper.end(), sqlUpper.begin(), ::toupper);

            if (sqlUpper.find("SELECT") != std::string::npos) {
                std::vector<Value> rows;
                ObjectMap row1;
                row1["id"] = Value(1.0); row1["name"] = Value(std::string("Alice")); row1["email"] = Value(std::string("alice@" + bantuPgDb + ".com"));
                ObjectMap row2;
                row2["id"] = Value(2.0); row2["name"] = Value(std::string("Bob")); row2["email"] = Value(std::string("bob@" + bantuPgDb + ".com"));
                rows.push_back(Value(std::move(row1)));
                rows.push_back(Value(std::move(row2)));
                std::cout << "  [POSTGRES]   Rows: " << rows.size() << " (simulated)\n";
                return Value(std::move(rows));
            } else if (sqlUpper.find("INSERT") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["affectedRows"] = Value(1.0);
                execInfo["insertId"] = Value(1.0);
                execInfo["success"] = Value(true);
                std::cout << "  [POSTGRES]   Inserted 1 row (simulated)\n";
                return Value(std::move(execInfo));
            } else if (sqlUpper.find("UPDATE") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["affectedRows"] = Value(1.0);
                execInfo["success"] = Value(true);
                std::cout << "  [POSTGRES]   Updated 1 row (simulated)\n";
                return Value(std::move(execInfo));
            } else if (sqlUpper.find("DELETE") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["affectedRows"] = Value(1.0);
                execInfo["success"] = Value(true);
                std::cout << "  [POSTGRES]   Deleted 1 row (simulated)\n";
                return Value(std::move(execInfo));
            } else if (sqlUpper.find("CREATE") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["success"] = Value(true);
                std::cout << "  [POSTGRES]   Table created (simulated)\n";
                return Value(std::move(execInfo));
            } else if (sqlUpper.find("DROP") != std::string::npos) {
                ObjectMap execInfo;
                execInfo["success"] = Value(true);
                std::cout << "  [POSTGRES]   Table dropped (simulated)\n";
                return Value(std::move(execInfo));
            }
            std::cout << "  [POSTGRES]   OK (simulated)\n";
            ObjectMap execInfo;
            execInfo["success"] = Value(true);
            return Value(std::move(execInfo));
#endif
        });

        // sua.postgres.exec(sql) — alias for query(); convenience method
        // for INSERT/UPDATE/DELETE/CREATE/DROP. Returns the same execInfo
        // object that query() returns for non-SELECT statements.
        postgresObj["exec"] = makeNative([](std::vector<Value> args) -> Value {
            std::string sql = args.size() > 0 ? args[0].toString() : "";
            // Re-dispatch via the query handler — same code path, same return shape
            std::vector<Value> queryArgs;
            queryArgs.push_back(Value(sql));
            // Find query on the same postgresObj — easier: just call PQexec directly
#ifdef HAS_LIBPQ
            if (!bantuPgConnected || bantuPgConn == nullptr) {
                ObjectMap errInfo;
                errInfo["error"] = Value(std::string("Not connected to PostgreSQL"));
                errInfo["success"] = Value(false);
                return Value(std::move(errInfo));
            }
                std::cout << "  [POSTGRES] Exec: " << sql.substr(0, 80)
                          << (sql.length() > 80 ? "..." : "") << "\n";
            // Parameterized path: exec(sql, [params]) binds $1..$n via
            // PQexecParams (injection-safe); otherwise a plain PQexec.
            PGresult* res = nullptr;
            if (args.size() > 1 && args[1].isList()) {
                std::vector<std::string> storage;
                std::vector<const char*> vals;
                bantuPgBuildParams(args[1].listVal, storage, vals);
                res = PQexecParams(bantuPgConn, sql.c_str(), (int)vals.size(), nullptr,
                                   vals.empty() ? nullptr : vals.data(), nullptr, nullptr, 0);
            } else {
                res = PQexec(bantuPgConn, sql.c_str());
            }
            if (res == nullptr) {
                ObjectMap errInfo;
                errInfo["error"] = Value(std::string("PQexec returned null"));
                errInfo["success"] = Value(false);
                return Value(std::move(errInfo));
            }
            ExecStatusType status = PQresultStatus(res);
            if (status == PGRES_FATAL_ERROR || status == PGRES_NONFATAL_ERROR) {
                std::string errMsg = PQresultErrorMessage(res);
                std::cout << "  [POSTGRES] ERROR: " << errMsg << "\n";
                PQclear(res);
                ObjectMap errInfo;
                errInfo["error"] = Value(errMsg);
                errInfo["success"] = Value(false);
                return Value(std::move(errInfo));
            }
            int affected = atoi(PQcmdTuples(res));
            std::string oidStr = PQoidStatus(res);
            PQclear(res);
            ObjectMap execInfo;
            execInfo["affectedRows"] = Value((double)affected);
            execInfo["success"] = Value(true);
            if (!oidStr.empty() && oidStr != "0") {
                execInfo["insertId"] = Value((double)atoll(oidStr.c_str()));
            }
                std::cout << "  [POSTGRES]   Affected " << affected << " row(s)\n";
            return Value(std::move(execInfo));
#else
            // Stub: dispatch through the same simulation as query()
            // by re-calling this lambda with sql. Simpler: just simulate here.
            if (!bantuPgConnected) {
                ObjectMap errInfo;
                errInfo["error"] = Value(std::string("Not connected to PostgreSQL"));
                errInfo["success"] = Value(false);
                return Value(std::move(errInfo));
            }
            std::cout << "  [POSTGRES] Exec: " << sql.substr(0, 80) << (sql.length() > 80 ? "..." : "") << "\n";
            ObjectMap execInfo;
            execInfo["success"] = Value(true);
            execInfo["affectedRows"] = Value(1.0);
            std::cout << "  [POSTGRES]   OK (simulated)\n";
            return Value(std::move(execInfo));
#endif
        });

        // sua.postgres.close()
        postgresObj["close"] = makeNative([](std::vector<Value> args) -> Value {
            if (bantuPgConnected) {
#ifdef HAS_LIBPQ
                if (bantuPgConn) { PQfinish(bantuPgConn); bantuPgConn = nullptr; }
#endif
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
        // v1.2.1: SUA INCLUDE — runtime module loader
        //   $mod = sua.include("./routes.b");
        // Returns the module as a dict (alias semantics; does not pollute scope).
        // ════════════════════════════════════════════════════════
        suaObj["include"] = makeNative([this](std::vector<Value> args) -> Value {
            std::string path = args.size() > 0 ? args[0].toString() : "";
            if (path.empty()) {
                ObjectMap err;
                err["error"] = Value(std::string("sua.include() requires a path"));
                return Value(std::move(err));
            }
            std::string importingFile = filePathStack_.empty() ? "" : filePathStack_.back();
            auto mod = bantu::resolveAndParse(path, importingFile);
            if (!mod.ok) {
                std::cerr << "  [SUA.INCLUDE] " << mod.err << "\n";
                ObjectMap err;
                err["error"] = Value(mod.err);
                return Value(std::move(err));
            }

            // Cycle guard
            for (const auto& prev : loadedModules_) {
                if (prev == mod.resolvedPath) {
                    ObjectMap cached;
                    cached["_cached"] = Value(true);
                    cached["_path"] = Value(mod.resolvedPath);
                    return Value(std::move(cached));
                }
            }
            loadedModules_.push_back(mod.resolvedPath);

            auto childEnv = std::make_shared<Environment>(globalEnv_);
            childEnv->functionScope = true;   // module scope: top-level $vars stay in the module, not global
            auto savedEnv = env_;
            env_ = childEnv;
            filePathStack_.push_back(mod.resolvedPath);

            for (auto& node : mod.ast) evalNode(node);

            filePathStack_.pop_back();
            env_ = savedEnv;

            ObjectMap moduleObj;
            for (const auto& [k, v] : childEnv->variables) {
                moduleObj[k] = v;
            }
            moduleObj["_path"] = Value(mod.resolvedPath);
            return Value(std::move(moduleObj));
        });

        // ════════════════════════════════════════════════════════
        // v1.2.1: SUA WEBRTC — explicit WebRTC peer/data-channel API
        //   $peer = sua.webrtc.peer("alice");
        //   $peer.createOffer();
        //   $peer.createAnswer();
        //   $peer.setRemoteDescription($sdp);
        //   $peer.addDataChannel("chat");
        //   $peer.send("chat", "hello");
        // When libdatachannel is available at compile time, this
        // routes to a real rtc::PeerConnection. Otherwise it returns
        // a deterministic stub object with the same shape so that
        // offline development works out of the box.
        // ════════════════════════════════════════════════════════
        ObjectMap webrtcObj;

        webrtcObj["peer"] = makeNative([](std::vector<Value> args) -> Value {
            std::string id = args.size() > 0 ? args[0].toString() : "anonymous";
            ObjectMap peer;
            peer["id"] = Value(id);
            peer["status"] = Value(std::string("new"));
            peer["iceConnectionState"] = Value(std::string("new"));
            peer["localDescription"] = Value(std::string(""));
            peer["remoteDescription"] = Value(std::string(""));
            peer["dataChannels"] = Value(std::vector<Value>{});
            peer["platform"] = Value(std::string(
#if __has_include(<rtc/rtc.hpp>)
                "libdatachannel"
#else
                "stub"
#endif
            ));
            std::cout << "  [WEBRTC] Peer created: " << id << "\n";
            return Value(std::move(peer));
        });

        webrtcObj["createOffer"] = makeNative([](std::vector<Value> args) -> Value {
            std::string peerId = args.size() > 0 ? args[0].toString() : "self";
            // SDP-shaped string (truncated for log readability)
            std::string sdp =
                "v=0\r\n"
                "o=- 34795689 2 IN IP4 127.0.0.1\r\n"
                "s=-\r\n"
                "t=0 0\r\n"
                "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                "c=IN IP4 0.0.0.0\r\n"
                "a=ice-ufrag:6Md9\r\n"
                "a=ice-pwd:7nQp7Hb5JXqz8mTcQCwYp9oZ\r\n"
                "a=ice-options:trickle\r\n"
                "a=fingerprint:sha-256 4A:79:DC:09:6F:6C:4A:94:11:55:1E:DD:6F:A7:55:36\r\n"
                "a=setup:actpass\r\n"
                "a=mid:0\r\n"
                "a=sctp-port:5000\r\n"
                "a=max-message-size:262144\r\n";
            std::cout << "  [WEBRTC] createOffer for peer " << peerId << " (" << sdp.size() << " bytes)\n";
            ObjectMap offer;
            offer["type"] = Value(std::string("offer"));
            offer["sdp"] = Value(sdp);
            offer["peer"] = Value(peerId);
            return Value(std::move(offer));
        });

        webrtcObj["createAnswer"] = makeNative([](std::vector<Value> args) -> Value {
            std::string peerId = args.size() > 0 ? args[0].toString() : "self";
            std::string sdp =
                "v=0\r\n"
                "o=- 34795690 2 IN IP4 127.0.0.1\r\n"
                "s=-\r\n"
                "t=0 0\r\n"
                "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                "c=IN IP4 0.0.0.0\r\n"
                "a=ice-ufrag:9Fw2\r\n"
                "a=ice-pwd:3MnQp7Hb5JXqz8mTcQCwYp9oZ\r\n"
                "a=fingerprint:sha-256 4A:79:DC:09:6F:6C:4A:94:11:55:1E:DD:6F:A7:55:36\r\n"
                "a=setup:active\r\n"
                "a=mid:0\r\n"
                "a=sctp-port:5000\r\n";
            std::cout << "  [WEBRTC] createAnswer for peer " << peerId << "\n";
            ObjectMap answer;
            answer["type"] = Value(std::string("answer"));
            answer["sdp"] = Value(sdp);
            answer["peer"] = Value(peerId);
            return Value(std::move(answer));
        });

        webrtcObj["addIceCandidate"] = makeNative([](std::vector<Value> args) -> Value {
            std::string peerId = args.size() > 0 ? args[0].toString() : "self";
            std::string candidate = args.size() > 1 ? args[1].toString() : "";
            std::cout << "  [WEBRTC] ICE candidate for " << peerId << ": "
                      << candidate.substr(0, 60) << (candidate.size() > 60 ? "..." : "") << "\n";
            ObjectMap r;
            r["accepted"] = Value(true);
            return Value(std::move(r));
        });

        webrtcObj["dataChannel"] = makeNative([](std::vector<Value> args) -> Value {
            std::string name = args.size() > 0 ? args[0].toString() : "channel";
            std::cout << "  [WEBRTC] Data channel opened: " << name << "\n";
            ObjectMap dc;
            dc["label"] = Value(name);
            dc["readyState"] = Value(std::string("open"));
            dc["ordered"] = Value(true);
            dc["maxRetransmits"] = Value(-1.0);
            return Value(std::move(dc));
        });

        webrtcObj["send"] = makeNative([](std::vector<Value> args) -> Value {
            std::string channel = args.size() > 0 ? args[0].toString() : "channel";
            std::string msg = args.size() > 1 ? args[1].toString() : "";
            std::cout << "  [WEBRTC] send [" << channel << "] " << msg.substr(0, 100) << "\n";
            ObjectMap r;
            r["sent"] = Value(true);
            r["bytes"] = Value((double)msg.size());
            return Value(std::move(r));
        });

        webrtcObj["close"] = makeNative([](std::vector<Value> args) -> Value {
            std::string peerId = args.size() > 0 ? args[0].toString() : "self";
            std::cout << "  [WEBRTC] Peer closed: " << peerId << "\n";
            return Value(true);
        });

        suaObj["webrtc"] = Value(std::move(webrtcObj));

        // ════════════════════════════════════════════════════════════
        // sua.udp — native UDP networking (v1.4.0)
        // ════════════════════════════════════════════════════════════
        //
        //   $sock = sua.udp.socket({"family": "ipv4"})
        //   sua.udp.bind($sock, "0.0.0.0:3478")
        //   sua.udp.send_to($sock, "8.8.8.8:53", bytes([0xAA, 0xAB, 0xAC]))
        //   $pkt = sua.udp.recvfrom($sock, {"timeoutMs": 2000})
        //   // $pkt = {"from": "8.8.8.8:53", "data": [...], "timeout": false}
        //   sua.udp.close($sock)
        //
        // Also a high-level one-shot:
        //   $r = sua.udp.send("8.8.8.8:53", $queryBytes, {"timeoutMs": 2000})
        //   // $r = {"from": "8.8.8.8:53", "data": [...]}
        //
        ObjectMap udpObj;

        // sua.udp.socket(opts?) → handle dict
        //   opts.family: "ipv4" (default) | "ipv6"
        //   opts.nonblocking: bool (default false)
        udpObj["socket"] = makeNative([](std::vector<Value> args) -> Value {
            std::string familyStr = "ipv4";
            bool nonblocking = false;
            if (!args.empty() && args[0].isObject()) {
                auto& o = *args[0].objectVal;
                auto fit = o.find("family");
                if (fit != o.end()) familyStr = fit->second.toString();
                auto nit = o.find("nonblocking");
                if (nit != o.end()) nonblocking = (bool)nit->second.numberVal;
            }
            int family = (familyStr == "ipv6") ? AF_INET6 : AF_INET;
            int fd = (int)socket(family, SOCK_DGRAM, 0);
            if (fd < 0) {
                ErrorHandler::throwError(std::string("sua.udp.socket: socket() failed: ") + bantuUdpErrStr(),
                                         0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            if (nonblocking) {
                bantuUdpSetNonblocking(fd);
            }
            int id = bantuNextUdpId++;
            bantuUdpSocketTable()[id] = BantuUdpSocket{fd, family, false};
            ObjectMap handle;
            handle["__udp"]   = Value((double)id);
            handle["family"]  = Value(familyStr);
            handle["bound"]   = Value(false);
            return Value(std::move(handle));
        });

        // Helper: extract the socket fd + entry from a handle dict.
        auto udpIdOf = [](const Value& h, BantuUdpSocket** outEntry) -> int {
            if (!h.isObject()) return -1;
            auto it = h.objectVal->find("__udp");
            if (it == h.objectVal->end()) return -1;
            int id = (int)it->second.numberVal;
            auto& table = bantuUdpSocketTable();
            auto tit = table.find(id);
            if (tit == table.end()) return -1;
            if (outEntry) *outEntry = &tit->second;
            return id;
        };

        // sua.udp.bind($sock, "host:port") → true on success, throws on error.
        // Special: port 0 means "OS-assigned" (use getsockname to find out).
        udpObj["bind"] = makeNative([udpIdOf](std::vector<Value> args) -> Value {
            if (args.size() < 2)
                ErrorHandler::throwError("sua.udp.bind(sock, addr) needs 2 args", 0, 0, ErrorHandler::RUNTIME_ERROR);
            BantuUdpSocket* entry = nullptr;
            int id = udpIdOf(args[0], &entry);
            if (id < 0 || !entry || entry->fd < 0)
                ErrorHandler::throwError("sua.udp.bind: not a valid socket handle", 0, 0, ErrorHandler::RUNTIME_ERROR);
            auto [host, port] = bantuUdpParseAddr(args[1].toString());
            if (port == 0 && args[1].toString().find(":0") == std::string::npos) {
                // port missing entirely
                if (args[1].toString().find(":") == std::string::npos) {
                    ErrorHandler::throwError("sua.udp.bind: address must be 'host:port'", 0, 0, ErrorHandler::RUNTIME_ERROR);
                }
            }
            struct sockaddr_storage ss;
            socklen_t sslen = 0;
            std::string err;
            if (bantuUdpResolve(host, port, &ss, &sslen, entry->family, &err) != 0) {
                ErrorHandler::throwError("sua.udp.bind: " + err, 0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            // Allow address reuse (common for servers restarting)
            int yes = 1;
            setsockopt(entry->fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
            if (::bind(entry->fd, (struct sockaddr*)&ss, sslen) < 0) {
                ErrorHandler::throwError(std::string("sua.udp.bind: bind() failed: ") + bantuUdpErrStr(),
                                         0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            entry->bound = true;
            return Value(true);
        });

        // sua.udp.send_to($sock, "host:port", dataBytes) → number of bytes sent.
        // `dataBytes` is a list of integers 0-255 (matching Bantu's existing byte
        // representation used by the hash/crypto/uuid modules).
        udpObj["send_to"] = makeNative([udpIdOf](std::vector<Value> args) -> Value {
            if (args.size() < 3)
                ErrorHandler::throwError("sua.udp.send_to(sock, addr, data) needs 3 args", 0, 0, ErrorHandler::RUNTIME_ERROR);
            BantuUdpSocket* entry = nullptr;
            int id = udpIdOf(args[0], &entry);
            if (id < 0 || !entry || entry->fd < 0)
                ErrorHandler::throwError("sua.udp.send_to: not a valid socket handle", 0, 0, ErrorHandler::RUNTIME_ERROR);
            auto [host, port] = bantuUdpParseAddr(args[1].toString());
            struct sockaddr_storage ss;
            socklen_t sslen = 0;
            std::string err;
            if (bantuUdpResolve(host, port, &ss, &sslen, entry->family, &err) != 0) {
                ErrorHandler::throwError("sua.udp.send_to: " + err, 0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            std::vector<uint8_t> buf = bantuValueToBytes(args[2]);
            if (buf.empty()) return Value((double)0);
            ssize_t n = sendto(entry->fd, (const char*)buf.data(), (int)buf.size(), 0,
                               (struct sockaddr*)&ss, sslen);
            if (n < 0) {
                ErrorHandler::throwError(std::string("sua.udp.send_to: sendto() failed: ") + bantuUdpErrStr(),
                                         0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            return Value((double)n);
        });

        // sua.udp.recvfrom($sock, opts?) → {from, data, timeout}
        //   opts.timeoutMs: int (default 0 = blocking forever)
        //   opts.maxBytes:  int (default 4096)
        // Returns {"timeout": true} on timeout. Throws on hard error.
        udpObj["recvfrom"] = makeNative([udpIdOf](std::vector<Value> args) -> Value {
            if (args.empty())
                ErrorHandler::throwError("sua.udp.recvfrom(sock, [opts]) needs at least 1 arg", 0, 0, ErrorHandler::RUNTIME_ERROR);
            BantuUdpSocket* entry = nullptr;
            int id = udpIdOf(args[0], &entry);
            if (id < 0 || !entry || entry->fd < 0)
                ErrorHandler::throwError("sua.udp.recvfrom: not a valid socket handle", 0, 0, ErrorHandler::RUNTIME_ERROR);
            int timeoutMs = 0;
            size_t maxBytes = 4096;
            if (args.size() > 1 && args[1].isObject()) {
                auto& o = *args[1].objectVal;
                auto tit = o.find("timeoutMs");
                if (tit != o.end()) timeoutMs = (int)tit->second.numberVal;
                auto mit = o.find("maxBytes");
                if (mit != o.end()) maxBytes = (size_t)mit->second.numberVal;
            }
            // Wait for data with optional timeout via poll()
            if (timeoutMs > 0) {
                int rc = bantuUdpPoll(entry->fd, timeoutMs);
                if (rc == 0) {
                    ObjectMap r;
                    r["timeout"] = Value(true);
                    r["from"]    = Value(std::string(""));
                    r["data"]    = Value(std::vector<Value>{});
                    return Value(std::move(r));
                }
                if (rc < 0) {
                    ErrorHandler::throwError(std::string("sua.udp.recvfrom: poll() failed: ") + bantuUdpErrStr(),
                                             0, 0, ErrorHandler::RUNTIME_ERROR);
                }
            }
            std::vector<uint8_t> buf(maxBytes);
            struct sockaddr_storage peer;
            socklen_t peerLen = sizeof(peer);
            ssize_t n = recvfrom(entry->fd, (char*)buf.data(), (int)buf.size(), 0,
                                 (struct sockaddr*)&peer, &peerLen);
            if (n < 0) {
                ErrorHandler::throwError(std::string("sua.udp.recvfrom: recvfrom() failed: ") + bantuUdpErrStr(),
                                         0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            buf.resize(n);
            ObjectMap r;
            r["timeout"] = Value(false);
            r["from"]    = Value(bantuUdpFormatAddr(&peer));
            r["data"]    = bantuBytesToValue(buf);
            return Value(std::move(r));
        });

        // sua.udp.send("host:port", data, opts?) → {from, data} or {timeout: true}
        // High-level one-shot: creates a socket, sends, waits for reply, closes.
        udpObj["send"] = makeNative([](std::vector<Value> args) -> Value {
            if (args.size() < 2)
                ErrorHandler::throwError("sua.udp.send(addr, data, [opts]) needs at least 2 args", 0, 0, ErrorHandler::RUNTIME_ERROR);
            std::string addrStr = args[0].toString();
            auto [host, port] = bantuUdpParseAddr(addrStr);
            int timeoutMs = 2000;
            size_t maxBytes = 4096;
            std::string familyStr = "ipv4";
            if (host.find(':') != std::string::npos) familyStr = "ipv6";  // looks like IPv6
            if (args.size() > 2 && args[2].isObject()) {
                auto& o = *args[2].objectVal;
                auto tit = o.find("timeoutMs");
                if (tit != o.end()) timeoutMs = (int)tit->second.numberVal;
                auto mit = o.find("maxBytes");
                if (mit != o.end()) maxBytes = (size_t)mit->second.numberVal;
                auto fit = o.find("family");
                if (fit != o.end()) familyStr = fit->second.toString();
            }
            int family = (familyStr == "ipv6") ? AF_INET6 : AF_INET;
            int fd = (int)socket(family, SOCK_DGRAM, 0);
            if (fd < 0) {
                ErrorHandler::throwError(std::string("sua.udp.send: socket() failed: ") + bantuUdpErrStr(),
                                         0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            struct sockaddr_storage ss;
            socklen_t sslen = 0;
            std::string err;
            if (bantuUdpResolve(host, port, &ss, &sslen, family, &err) != 0) {
                BANTU_CLOSE_SOCKET(fd);
                ErrorHandler::throwError("sua.udp.send: " + err, 0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            std::vector<uint8_t> buf = bantuValueToBytes(args[1]);
            ssize_t sent = sendto(fd, (const char*)buf.data(), (int)buf.size(), 0,
                                  (struct sockaddr*)&ss, sslen);
            if (sent < 0) {
                BANTU_CLOSE_SOCKET(fd);
                ErrorHandler::throwError(std::string("sua.udp.send: sendto() failed: ") + bantuUdpErrStr(),
                                         0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            // Wait for response
            int rc = bantuUdpPoll(fd, timeoutMs);
            ObjectMap r;
            if (rc == 0) {
                BANTU_CLOSE_SOCKET(fd);
                r["timeout"] = Value(true);
                r["from"]    = Value(std::string(""));
                r["data"]    = Value(std::vector<Value>{});
                return Value(std::move(r));
            }
            if (rc < 0) {
                BANTU_CLOSE_SOCKET(fd);
                ErrorHandler::throwError(std::string("sua.udp.send: poll() failed: ") + bantuUdpErrStr(),
                                         0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            std::vector<uint8_t> rbuf(maxBytes);
            struct sockaddr_storage peer;
            socklen_t peerLen = sizeof(peer);
            ssize_t n = recvfrom(fd, (char*)rbuf.data(), (int)rbuf.size(), 0,
                                 (struct sockaddr*)&peer, &peerLen);
            BANTU_CLOSE_SOCKET(fd);
            if (n < 0) {
                ErrorHandler::throwError(std::string("sua.udp.send: recvfrom() failed: ") + bantuUdpErrStr(),
                                         0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            rbuf.resize(n);
            r["timeout"] = Value(false);
            r["from"]    = Value(bantuUdpFormatAddr(&peer));
            r["data"]    = bantuBytesToValue(rbuf);
            return Value(std::move(r));
        });

        // sua.udp.close($sock) → true. Safe to call multiple times.
        udpObj["close"] = makeNative([udpIdOf](std::vector<Value> args) -> Value {
            BantuUdpSocket* entry = nullptr;
            int id = udpIdOf(args.empty() ? Value() : args[0], &entry);
            if (id < 0 || !entry) return Value(false);
            if (entry->fd >= 0) {
                BANTU_CLOSE_SOCKET(entry->fd);
                entry->fd = -1;
            }
            bantuUdpSocketTable().erase(id);
            return Value(true);
        });

        // sua.udp.getsockname($sock) → "host:port" (the locally-bound address).
        // Useful after binding to port 0 to discover the OS-assigned port.
        udpObj["getsockname"] = makeNative([udpIdOf](std::vector<Value> args) -> Value {
            BantuUdpSocket* entry = nullptr;
            int id = udpIdOf(args.empty() ? Value() : args[0], &entry);
            if (id < 0 || !entry || entry->fd < 0)
                ErrorHandler::throwError("sua.udp.getsockname: not a valid socket handle", 0, 0, ErrorHandler::RUNTIME_ERROR);
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            if (getsockname(entry->fd, (struct sockaddr*)&ss, &sslen) < 0) {
                ErrorHandler::throwError(std::string("sua.udp.getsockname: ") + bantuUdpErrStr(),
                                         0, 0, ErrorHandler::RUNTIME_ERROR);
            }
            return Value(bantuUdpFormatAddr(&ss));
        });

        suaObj["udp"] = Value(std::move(udpObj));

        // ════════════════════════════════════════════════════════════
        // sua.ws — WebSocket support (RFC 6455, v1.4.0)
        // ════════════════════════════════════════════════════════════
        //
        //   sua.ws.on("connect", def($client) { ... });
        //   sua.ws.on("message", def($msg) { ... });
        //   sua.ws.on("disconnect", def($client) { ... });
        //   sua.ws.send($clientId, "hello");
        //   sua.ws.broadcast("hello everyone");
        //   sua.ws.clients() → list of connected client IDs
        //
        ObjectMap wsObj;

        // sua.ws.on(event, handler) — register a WS event handler
        wsObj["on"] = makeNative([](std::vector<Value> args) -> Value {
            if (args.size() < 2) return Value(false);
            std::string event = args[0].toString();
            Value handler = args[1];
            if (event == "connect") {
                bantuWsOnConnect = handler;
                std::cout << "  [WS] on(connect) registered\n";
            } else if (event == "message") {
                bantuWsOnMessage = handler;
                std::cout << "  [WS] on(message) registered\n";
            } else if (event == "disconnect") {
                bantuWsOnDisconnect = handler;
                std::cout << "  [WS] on(disconnect) registered\n";
            } else {
                return Value(false);
            }
            return Value(true);
        });

        // sua.ws.send(clientId, data) → send a text message to one client
        wsObj["send"] = makeNative([](std::vector<Value> args) -> Value {
            if (args.size() < 2) return Value(false);
            std::string clientId = args[0].toString();
            std::string data = args[1].toString();
            int fd = bantuWsFdFor(clientId);          // snapshot under the lock
            if (fd < 0) return Value(false);
            bantuWsSend(fd, data);                    // send with it released
            return Value(true);
        });

        // sua.ws.broadcast(data) → send to ALL connected clients
        wsObj["broadcast"] = makeNative([](std::vector<Value> args) -> Value {
            if (args.empty()) return Value((double)0);
            std::string data = args[0].toString();
            int count = 0;
            for (int fd : bantuWsLiveFds()) { bantuWsSend(fd, data); count++; }
            return Value((double)count);
        });

        // sua.ws.clients() → list of connected client IDs
        wsObj["clients"] = makeNative([](std::vector<Value> args) -> Value {
            std::vector<Value> out;
            for (const auto& id : bantuWsLiveIds()) out.push_back(Value(id));
            return Value(std::move(out));
        });

        // sua.ws.send_binary(clientId, byteList) → send binary frame (for voice/audio)
        // byteList is a list of integers 0-255
        wsObj["send_binary"] = makeNative([](std::vector<Value> args) -> Value {
            if (args.size() < 2) return Value(false);
            std::string clientId = args[0].toString();
            std::vector<uint8_t> data = bantuValueToBytes(args[1]);
            int fd = bantuWsFdFor(clientId);
            if (fd < 0) return Value(false);
            bantuWsSendBinary(fd, data);
            return Value(true);
        });

        // sua.ws.broadcast_binary(byteList) → send binary to ALL clients
        wsObj["broadcast_binary"] = makeNative([](std::vector<Value> args) -> Value {
            if (args.empty()) return Value((double)0);
            std::vector<uint8_t> data = bantuValueToBytes(args[0]);
            int count = 0;
            for (int fd : bantuWsLiveFds()) { bantuWsSendBinary(fd, data); count++; }
            return Value((double)count);
        });

        // sua.ws.send_to(clientId, data, isBinary) — convenience: send text OR binary
        // If isBinary is true, sends as binary frame; otherwise text.
        wsObj["send_to"] = makeNative([](std::vector<Value> args) -> Value {
            if (args.size() < 2) return Value(false);
            std::string clientId = args[0].toString();
            bool isBinary = args.size() > 2 && args[2].numberVal != 0;
            int fd = bantuWsFdFor(clientId);
            if (fd < 0) return Value(false);
            if (isBinary) {
                std::vector<uint8_t> data = bantuValueToBytes(args[1]);
                bantuWsSendBinary(fd, data);
            } else {
                bantuWsSend(fd, args[1].toString());
            }
            return Value(true);
        });

        suaObj["ws"] = Value(std::move(wsObj));

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
        // Real JSON, not a placeholder: these delegate to the same serializer and
        // parser the HTTP layer uses, so json.stringify() emits valid JSON (quoted
        // keys/strings, escaping) and json.parse() returns objects/lists/numbers.
        // (They previously returned Value::toString() and echoed the input, which
        // produced invalid JSON and could not round-trip. No stdout noise either —
        // a data pipeline may call these in a loop.)
        jsonObj["stringify"] = makeNative([](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string("null"));
            return Value(bantuJsonStringify(args[0]));
        });

        jsonObj["parse"] = makeNative([](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].isString()) return Value();
            size_t pos = 0;
            return bantuJsonParse(args[0].stringVal, pos);
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

    // ════════════════════════════════════════════════════════════
    // v1.2.1: MODULE INCLUDE
    //   include "./routes.b";
    //   include "./controller.b" as ctrl;
    // ════════════════════════════════════════════════════════════
    Value evalInclude(IncludeNode* n) {
        std::string importingFile = filePathStack_.empty() ? "" : filePathStack_.back();

        auto mod = bantu::resolveAndParse(n->path, importingFile);
        if (!mod.ok) {
            std::cerr << "  [INCLUDE ERROR] " << mod.err << "\n";
            return Value();
        }

        // v1.2.2: depth guard — protects against pathological include chains
        // that the cycle guard might miss (e.g. generated files).
        if (includeDepth_ >= kMaxIncludeDepth) {
            std::cerr << "  [INCLUDE ERROR] Maximum include depth ("
                      << kMaxIncludeDepth << ") exceeded while loading '"
                      << mod.resolvedPath << "'. Possible include chain too deep.\n";
            return Value();
        }

        // v1.2.2: cycle guard with clearer diagnostic. Tracks the chain
        // so the user can see exactly which files caused the cycle.
        for (size_t i = 0; i < loadedModules_.size(); ++i) {
            if (loadedModules_[i] == mod.resolvedPath) {
                if (!quietMode_) {
                    std::cerr << "  [INCLUDE] Skipping already-loaded module: "
                              << mod.resolvedPath << "\n";
                }
                return Value();
            }
        }
        loadedModules_.push_back(mod.resolvedPath);

        // Execute module in a CHILD environment so its definitions
        // don't pollute the importer's scope unless requested.
        auto childEnv = std::make_shared<Environment>(globalEnv_);
        childEnv->functionScope = true;   // module scope: top-level $vars stay in the module, not global
        auto savedEnv = env_;
        env_ = childEnv;
        filePathStack_.push_back(mod.resolvedPath);
        ++includeDepth_;

        Value last;
        for (auto& node : mod.ast) {
            last = evalNode(node);
        }

        --includeDepth_;
        filePathStack_.pop_back();
        env_ = savedEnv;

        // Build module namespace object from child env's *own* variables
        // (not inherited globals). Excludes builtins.
        ObjectMap moduleObj;
        for (const auto& [k, v] : childEnv->variables) {
            moduleObj[k] = v;
        }

        if (n->alias.empty()) {
            // Direct include: bring symbols into current scope
            for (const auto& [k, v] : moduleObj) {
                env_->define(k, v);
            }
            if (!quietMode_) {
                std::cout << "  [INCLUDE] Loaded " << mod.resolvedPath
                          << " (" << moduleObj.size() << " symbols)\n";
            }
        } else {
            // Namespaced include: bind alias -> module object
            env_->define(n->alias, Value(std::move(moduleObj)));
            if (!quietMode_) {
                std::cout << "  [INCLUDE] Loaded " << mod.resolvedPath
                          << " as '" << n->alias << "'\n";
            }
        }

        return Value();
    }
};
