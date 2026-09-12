#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  worker_pool.hpp — SO_REUSEPORT worker processes and the broadcast bus.
//
//  The event loop in event_loop.hpp holds tens of thousands of connections on
//  ONE thread. That thread is one core. This is the other fifteen.
//
//  See docs/sua-architecture.md §12 for the design and its consequences. The
//  short version:
//
//    * n processes, forked AFTER the Bantu program has run (routes registered,
//      globals initialised) and BEFORE anything is accepted, so every worker
//      starts from an identical fully-configured interpreter.
//    * each worker opens its OWN listening socket with SO_REUSEPORT; the
//      kernel load-balances accepts across them (NGINX's model).
//    * the parent never accepts traffic. It supervises (respawning a worker
//      that dies) and relays the broadcast bus. Keeping it out of the accept
//      path means a crash in request handling can never take down the thing
//      that restarts request handling.
//
//  Global Bantu state is therefore PER WORKER -- writes after the fork never
//  meet. That is not an oversight to patch later; it is the property that makes
//  the model safe (no shared mutable state => the interpreter races of §2.1 and
//  §2.2 cannot recur). Shared state belongs in a database or on the bus.
//
//  No new dependency: a socketpair and this file, rather than a broker to
//  operate. Same discipline as the from-scratch P-256.
// ════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
  #include <errno.h>
  #include <signal.h>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #include "event_loop.hpp"
#endif

namespace bantu_workers {

// ─── Bus wire format ───────────────────────────────────────────────────────
//   [u32 length BE][u8 type][payload]        length counts type + payload
//
// Deliberately dull. Length-prefixed because a socketpair is a byte stream: a
// frame can arrive split across reads or several frames coalesced into one, the
// same truncation trap that corrupted WebSocket voice data before Phase 2.
enum BusType : uint8_t {
    BUS_TEXT            = 1,   // payload: message bytes
    BUS_BINARY          = 2,   // payload: message bytes
    BUS_TARGET_TEXT     = 3,   // payload: [u16 idLen BE][client id][data]
    BUS_TARGET_BINARY   = 4,   // payload: [u16 idLen BE][client id][data]

    // ── client roster (opt-in; see sua.server.limits({"ws_roster": true})) ──
    // Every roster frame opens with [u8 worker index], so a receiver can keep
    // each worker's set separately and replace it wholesale without having to
    // reason about which entries came from where.
    BUS_ROSTER_ADD      = 5,   // payload: [u8 worker][client id]
    BUS_ROSTER_DEL      = 6,   // payload: [u8 worker][client id]
    BUS_ROSTER_REQ      = 7,   // payload: [u8 worker]  -- "send me your list"
    BUS_ROSTER_FULL     = 8,   // payload: [u8 worker][ids joined by \n]
};

// Largest frame the relay will carry. A broadcast bigger than this is dropped
// at the sender with a counted drop rather than being partially relayed.
static const size_t kBusMaxFrame = 16 * 1024 * 1024;

// How much a single bus endpoint may hold before it starts dropping. A wedged
// peer must degrade the bus, not exhaust memory.
static const size_t kBusMaxBuffered = 8 * 1024 * 1024;

inline void busEncode(std::string& out, uint8_t type, const char* payload, size_t n) {
    uint32_t len = (uint32_t)(n + 1);
    char hdr[5];
    hdr[0] = (char)((len >> 24) & 0xFF);
    hdr[1] = (char)((len >> 16) & 0xFF);
    hdr[2] = (char)((len >> 8) & 0xFF);
    hdr[3] = (char)(len & 0xFF);
    hdr[4] = (char)type;
    out.append(hdr, 5);
    out.append(payload, n);
}

// Pull one complete frame off the front of `buf`. Returns false when the buffer
// does not yet hold a whole frame (the caller waits for more bytes).
inline bool busDecode(std::string& buf, uint8_t& type, std::string& payload) {
    if (buf.size() < 5) return false;
    uint32_t len = ((uint32_t)(uint8_t)buf[0] << 24) | ((uint32_t)(uint8_t)buf[1] << 16)
                 | ((uint32_t)(uint8_t)buf[2] << 8)  |  (uint32_t)(uint8_t)buf[3];
    if (len < 1 || len > kBusMaxFrame) {   // desynchronised or hostile
        buf.clear();
        return false;
    }
    if (buf.size() < 4 + (size_t)len) return false;
    type = (uint8_t)buf[4];
    payload.assign(buf, 5, (size_t)len - 1);
    buf.erase(0, 4 + (size_t)len);
    return true;
}

// Encode/decode the [u16 idLen][id][data] payload of a targeted message.
inline void busPackTarget(std::string& out, const std::string& id,
                          const char* data, size_t n) {
    uint16_t l = (uint16_t)(id.size() > 0xFFFF ? 0xFFFF : id.size());
    out.push_back((char)((l >> 8) & 0xFF));
    out.push_back((char)(l & 0xFF));
    out.append(id, 0, l);
    out.append(data, n);
}
inline bool busUnpackTarget(const std::string& payload, std::string& id, std::string& data) {
    if (payload.size() < 2) return false;
    size_t l = ((size_t)(uint8_t)payload[0] << 8) | (size_t)(uint8_t)payload[1];
    if (payload.size() < 2 + l) return false;
    id.assign(payload, 2, l);
    data.assign(payload, 2 + l, payload.size() - 2 - l);
    return true;
}

#if defined(_WIN32)

// ─── Windows ───────────────────────────────────────────────────────────────
// No fork(), and no way to hand a listening socket to a child without a
// different mechanism entirely. Single-worker Windows is not a regression
// against anything that shipped; see §12.5.
inline bool supported() { return false; }
inline bool kernelBalancesAccepts() { return false; }
inline int  cpuCount()  { return 1; }
struct Ctx { int index = 0; int busFd = -1; int workers = 1; };
inline bool start(int, Ctx&) { return true; }
inline bool reusePort(int)   { return false; }

#else

inline bool supported() {
#ifdef SO_REUSEPORT
    return true;
#else
    return false;
#endif
}

// Does SO_REUSEPORT actually DISTRIBUTE incoming TCP connections, or does it
// merely permit several sockets to share the port?
//
// This distinction is the whole ballgame and the two behaviours look identical
// until you measure. Linux 3.9+ hashes each connection's 4-tuple to one of the
// listening sockets, which is the load balancing NGINX's model depends on.
// macOS and the BSDs allow the bind but wake a single socket -- FreeBSD later
// added a SEPARATE option, SO_REUSEPORT_LB, precisely because plain
// SO_REUSEPORT does not balance, and macOS has no equivalent at all.
//
// Measured on macOS 24.6 before this existed: four workers all listening, and
// every one of twelve connections delivered to worker 0. So where the kernel
// does not balance, we fall back to the classic pre-fork model -- ONE listening
// socket created before the fork and inherited by every worker, each accepting
// from it. That trades a mild thundering herd for actual multi-core use, which
// is the right trade when the alternative is one busy worker and N-1 idle ones.
inline bool kernelBalancesAccepts() {
#if defined(__linux__) && defined(SO_REUSEPORT)
    return true;
#else
    return false;
#endif
}

inline int cpuCount() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
}

// Set SO_REUSEPORT so every worker can bind the same port and let the kernel
// distribute accepts. Without it the workers would have to share one listening
// socket, which is the thundering herd this design exists to avoid.
inline bool reusePort(int fd) {
#ifdef SO_REUSEPORT
    int on = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) == 0;
#else
    (void)fd;
    return false;
#endif
}

struct Ctx {
    int index   = 0;    // this worker's index, 0..workers-1
    int busFd   = -1;   // socketpair end to the parent, non-blocking
    int workers = 1;
};

// ─── The parent's side of one worker ──────────────────────────────────────
struct Slot {
    pid_t pid = -1;
    int   fd  = -1;     // parent end of the socketpair
    std::string in;     // partial inbound frame
    std::string out;    // pending outbound, when the worker is slow
    size_t outPos = 0;
    bool   wantWrite = false;
};

// Set by the parent's signal handler. volatile sig_atomic_t is the only type
// the standard lets a handler touch.
static volatile sig_atomic_t gStopping = 0;
inline void onStopSignal(int) { gStopping = 1; }

// ═══════════════════════════════════════════════════════════════════════════
//  start(n, out)
//
//  In a CHILD: fills `out` and returns true. The caller goes on to run the
//  server exactly as it would single-process.
//
//  In the PARENT: never returns. Supervises and relays until every worker is
//  gone, then _exit()s.
// ═══════════════════════════════════════════════════════════════════════════
inline bool start(int n, Ctx& out) {
    if (n < 1) n = 1;

    std::vector<Slot> slots((size_t)n);

    // Fork one worker into slot i. Returns true in the child.
    auto spawn = [&](size_t i) -> bool {
        int sp[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
            std::fprintf(stderr, "  [SERVER] worker %zu: socketpair failed: %s\n",
                         i, strerror(errno));
            return false;
        }
        pid_t pid = fork();
        if (pid < 0) {
            std::fprintf(stderr, "  [SERVER] worker %zu: fork failed: %s\n",
                         i, strerror(errno));
            close(sp[0]); close(sp[1]);
            return false;
        }
        if (pid == 0) {
            // ── child ──
            close(sp[0]);
            // Close every OTHER worker's parent-side fd inherited through the
            // fork. Leaving them open would keep a dead worker's pipe alive and
            // defeat the parent's EOF-based death detection.
            for (size_t j = 0; j < slots.size(); j++)
                if (slots[j].fd >= 0) close(slots[j].fd);
            bantu_loop::setNonBlocking(sp[1]);
            out.index = (int)i;
            out.busFd = sp[1];
            out.workers = n;
            // A worker must die on Ctrl-C like a normal process; the parent
            // forwards the signal and reaps.
            signal(SIGINT,  SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            return true;
        }
        // ── parent ──
        close(sp[1]);
        bantu_loop::setNonBlocking(sp[0]);
        slots[i].pid = pid;
        slots[i].fd  = sp[0];
        slots[i].in.clear();
        slots[i].out.clear();
        slots[i].outPos = 0;
        slots[i].wantWrite = false;
        return false;
    };

    for (size_t i = 0; i < slots.size(); i++)
        if (spawn(i)) return true;          // we are a child: go run the server

    // ═══ parent: supervisor + relay ═══
    signal(SIGINT,  onStopSignal);
    signal(SIGTERM, onStopSignal);
    signal(SIGPIPE, SIG_IGN);               // a dying worker must not kill us

    auto backend = bantu_loop::makeBackend();
    for (auto& s : slots)
        if (s.fd >= 0) backend->add(s.fd, true, false);

    std::fprintf(stderr, "  [SERVER] %d workers (pids:", n);
    for (auto& s : slots) std::fprintf(stderr, " %d", (int)s.pid);
    std::fprintf(stderr, ") supervisor pid %d\n", (int)getpid());
    std::fflush(stderr);

    // Queue a frame for one worker, dropping if that worker is too far behind.
    auto relayTo = [&](Slot& s, const std::string& frame) {
        if (s.fd < 0) return;
        if (s.out.size() - s.outPos + frame.size() > kBusMaxBuffered) return;  // drop
        s.out.append(frame);
        if (!s.wantWrite) { backend->mod(s.fd, true, true); s.wantWrite = true; }
    };

    auto flush = [&](Slot& s) {
        while (s.outPos < s.out.size()) {
            ssize_t w = write(s.fd, s.out.data() + s.outPos, s.out.size() - s.outPos);
            if (w > 0) { s.outPos += (size_t)w; continue; }
            if (bantu_loop::wouldBlock()) break;
            break;                          // worker is gone; reaping handles it
        }
        if (s.outPos >= s.out.size()) { s.out.clear(); s.outPos = 0; }
        bool want = s.outPos < s.out.size();
        if (want != s.wantWrite) { backend->mod(s.fd, true, want); s.wantWrite = want; }
    };

    auto dropSlot = [&](Slot& s) {
        if (s.fd >= 0) { backend->del(s.fd); close(s.fd); s.fd = -1; }
        s.in.clear(); s.out.clear(); s.outPos = 0; s.wantWrite = false;
    };

    std::vector<bantu_loop::Event> events;
    while (true) {
        if (gStopping) {
            for (auto& s : slots) if (s.pid > 0) kill(s.pid, SIGTERM);
            break;
        }

        backend->wait(events, 200);

        for (const auto& ev : events) {
            size_t idx = slots.size();
            for (size_t i = 0; i < slots.size(); i++)
                if (slots[i].fd == ev.fd) { idx = i; break; }
            if (idx == slots.size()) continue;
            Slot& s = slots[idx];

            if (ev.readable) {
                char buf[65536];
                bool eof = false;
                for (;;) {
                    ssize_t r = read(s.fd, buf, sizeof(buf));
                    if (r > 0) { s.in.append(buf, (size_t)r); continue; }
                    if (r == 0) { eof = true; break; }
                    if (bantu_loop::wouldBlock()) break;
                    eof = true; break;
                }
                // Fan out every complete frame to the OTHER workers. The sender
                // already delivered to its own clients, so echoing it back would
                // duplicate every broadcast.
                uint8_t type; std::string payload;
                while (busDecode(s.in, type, payload)) {
                    std::string frame;
                    busEncode(frame, type, payload.data(), payload.size());
                    for (size_t j = 0; j < slots.size(); j++)
                        if (j != idx) relayTo(slots[j], frame);
                }
                for (auto& o : slots) if (o.fd >= 0) flush(o);
                if (eof) dropSlot(s);
            }
            if (ev.writable && s.fd >= 0) flush(s);
            if (ev.error && s.fd >= 0)    dropSlot(s);
        }

        // ── reap and respawn ──
        // Respawning forks from the parent, whose interpreter state is exactly
        // what it was at the original fork -- so a replacement worker is
        // identical to the one it replaces, not one that inherited any
        // request-handling state.
        for (;;) {
            int status = 0;
            pid_t gone = waitpid(-1, &status, WNOHANG);
            if (gone <= 0) break;
            for (size_t i = 0; i < slots.size(); i++) {
                if (slots[i].pid != gone) continue;
                dropSlot(slots[i]);
                slots[i].pid = -1;

                // Tell the survivors that this worker's clients are gone. Only
                // the supervisor can: the dead worker cannot send its own
                // farewell, and without this its ids would sit in every other
                // worker's roster until the process exited.
                {
                    std::string payload(1, (char)(uint8_t)i);
                    std::string frame;
                    busEncode(frame, BUS_ROSTER_FULL, payload.data(), payload.size());
                    for (size_t j = 0; j < slots.size(); j++)
                        if (j != i && slots[j].fd >= 0) relayTo(slots[j], frame);
                    for (auto& o : slots) if (o.fd >= 0) flush(o);
                }

                if (gStopping) break;
                std::fprintf(stderr, "  [SERVER] worker %zu (pid %d) exited (%s %d); restarting\n",
                             i, (int)gone,
                             WIFSIGNALED(status) ? "signal" : "status",
                             WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
                std::fflush(stderr);
                if (spawn(i)) return true;                 // we are the new child
                if (slots[i].fd >= 0) backend->add(slots[i].fd, true, false);
                break;
            }
        }

        bool anyLeft = false;
        for (auto& s : slots) if (s.pid > 0) anyLeft = true;
        if (!anyLeft) break;
    }

    // Drain children so they do not become zombies, then leave.
    for (int i = 0; i < 200; i++) {
        bool anyLeft = false;
        for (auto& s : slots) {
            if (s.pid <= 0) continue;
            int status = 0;
            pid_t g = waitpid(s.pid, &status, WNOHANG);
            if (g == s.pid || g < 0) s.pid = -1; else anyLeft = true;
        }
        if (!anyLeft) break;
        usleep(10000);
    }
    for (auto& s : slots) if (s.pid > 0) kill(s.pid, SIGKILL);
    _exit(0);
}

#endif // !_WIN32

} // namespace bantu_workers
