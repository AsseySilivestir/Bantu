#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  event_loop.hpp — readiness-based I/O multiplexing for the sua server.
//
//  WHY THIS EXISTS
//  ---------------
//  sua handled each connection on its own detached thread. That works until it
//  doesn't: a WebSocket connection is held open by design, so it pins an OS
//  thread — roughly 8 MB of stack — for its entire life. A few thousand idle
//  chat clients exhaust memory, and the context-switch storm under load destroys
//  the sub-50 ms latency the WebSocket support exists to deliver. It also meant
//  every connection ran the interpreter concurrently, which corrupted state.
//
//  A readiness loop replaces the thread with a ~4 KB struct: one thread watches
//  every socket and does work only on the ones that are actually ready. This is
//  what NGINX and Node do, and it is the natural shape for an interpreter that
//  has exactly one execution context.
//
//  DESIGN
//  ------
//   • Self-contained: kqueue / epoll / poll directly, no libuv. The project
//     links no dependency it can write itself (see the from-scratch P-256).
//   • One Backend interface over three kernel APIs, so io_uring can be added
//     later as a fourth without touching the loop.
//   • Level-triggered throughout. Edge-triggered is faster in microbenchmarks
//     and much easier to get wrong: miss one drain and the connection hangs
//     forever. Level-triggered tolerates partial reads by construction.
//   • Coalesced events: one Event per fd per wait(), never two. Otherwise a
//     handler that closes a socket on the read event leaves the write event
//     pointing at a dead fd.
//
//  See docs/sua-architecture.md for the full rationale.
// ════════════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
  #include <winsock2.h>
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <sys/socket.h>
  #if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    #define BANTU_LOOP_KQUEUE 1
    #include <sys/event.h>
    #include <sys/time.h>
  #elif defined(__linux__)
    #define BANTU_LOOP_EPOLL 1
    #include <sys/epoll.h>
  #endif
  #include <poll.h>
#endif

namespace bantu_loop {

// ── time ────────────────────────────────────────────────────────────────────
inline uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── non-blocking sockets ────────────────────────────────────────────────────
// Every fd in the loop must be non-blocking. A blocking recv() on one connection
// would stall every other connection this thread owns — the failure the loop
// exists to prevent.
inline bool setNonBlocking(int fd) {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// True when a failed recv/send simply means "nothing ready right now", as
// opposed to a real error. Level-triggered polling can still hand us a spurious
// wakeup, so this is not merely theoretical.
inline bool wouldBlock() {
#if defined(_WIN32)
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

// ── the backend interface ───────────────────────────────────────────────────

struct Event {
    int  fd       = -1;
    bool readable = false;
    bool writable = false;
    bool error    = false;     // hangup or socket error; the owner should close
};

class Backend {
public:
    virtual ~Backend() {}
    virtual bool add(int fd, bool readable, bool writable) = 0;
    virtual bool mod(int fd, bool readable, bool writable) = 0;
    virtual bool del(int fd) = 0;
    // Blocks up to timeoutMs (-1 = forever). Returns the number of events, or
    // -1 on a real error. `out` is cleared first and holds at most one entry
    // per fd.
    virtual int  wait(std::vector<Event>& out, int timeoutMs) = 0;
    virtual const char* name() const = 0;
};

// ── kqueue (macOS, BSD) ─────────────────────────────────────────────────────
#if defined(BANTU_LOOP_KQUEUE)
class KqueueBackend : public Backend {
public:
    KqueueBackend() { kq_ = kqueue(); }
    ~KqueueBackend() override { if (kq_ >= 0) ::close(kq_); }
    bool ok() const { return kq_ >= 0; }

    // Both filters are registered once and then enabled/disabled, rather than
    // added and deleted repeatedly — EV_DELETE on a filter that was never added
    // returns ENOENT, which is noisy and easy to mishandle.
    bool add(int fd, bool r, bool w) override {
        struct kevent ev[2];
        EV_SET(&ev[0], fd, EVFILT_READ,  EV_ADD | (r ? EV_ENABLE : EV_DISABLE), 0, 0, nullptr);
        EV_SET(&ev[1], fd, EVFILT_WRITE, EV_ADD | (w ? EV_ENABLE : EV_DISABLE), 0, 0, nullptr);
        return kevent(kq_, ev, 2, nullptr, 0, nullptr) != -1;
    }
    bool mod(int fd, bool r, bool w) override { return add(fd, r, w); }
    bool del(int fd) override {
        struct kevent ev[2];
        EV_SET(&ev[0], fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
        EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        kevent(kq_, ev, 2, nullptr, 0, nullptr);   // ENOENT here is benign
        return true;
    }

    int wait(std::vector<Event>& out, int timeoutMs) override {
        out.clear();
        struct kevent evs[512];
        struct timespec ts, *pts = nullptr;
        if (timeoutMs >= 0) {
            ts.tv_sec  = timeoutMs / 1000;
            ts.tv_nsec = (long)(timeoutMs % 1000) * 1000000L;
            pts = &ts;
        }
        int n = kevent(kq_, nullptr, 0, evs, 512, pts);
        if (n < 0) return (errno == EINTR) ? 0 : -1;

        // kqueue reports EVFILT_READ and EVFILT_WRITE as separate events for the
        // same fd; collapse them so a caller that closes on read never sees a
        // second event for a dead descriptor.
        idx_.clear();
        for (int i = 0; i < n; i++) {
            int fd = (int)evs[i].ident;
            auto it = idx_.find(fd);
            if (it == idx_.end()) {
                idx_[fd] = out.size();
                out.push_back(Event{fd, false, false, false});
                it = idx_.find(fd);
            }
            Event& e = out[it->second];
            if (evs[i].filter == EVFILT_READ)  e.readable = true;
            if (evs[i].filter == EVFILT_WRITE) e.writable = true;
            if (evs[i].flags & EV_EOF)   e.error = true;
            if (evs[i].flags & EV_ERROR) e.error = true;
        }
        return (int)out.size();
    }
    const char* name() const override { return "kqueue"; }

private:
    int kq_ = -1;
    std::unordered_map<int, size_t> idx_;
};
#endif

// ── epoll (Linux) ───────────────────────────────────────────────────────────
#if defined(BANTU_LOOP_EPOLL)
class EpollBackend : public Backend {
public:
    EpollBackend() { ep_ = epoll_create1(0); }
    ~EpollBackend() override { if (ep_ >= 0) ::close(ep_); }
    bool ok() const { return ep_ >= 0; }

    bool add(int fd, bool r, bool w) override { return ctl(EPOLL_CTL_ADD, fd, r, w); }
    bool mod(int fd, bool r, bool w) override {
        // A descriptor we never added (or that was closed) gives ENOENT; adding
        // is the correct recovery rather than treating it as fatal.
        if (ctl(EPOLL_CTL_MOD, fd, r, w)) return true;
        return ctl(EPOLL_CTL_ADD, fd, r, w);
    }
    bool del(int fd) override {
        epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
        return true;
    }

    int wait(std::vector<Event>& out, int timeoutMs) override {
        out.clear();
        struct epoll_event evs[512];
        int n = epoll_wait(ep_, evs, 512, timeoutMs);
        if (n < 0) return (errno == EINTR) ? 0 : -1;
        out.reserve((size_t)n);
        for (int i = 0; i < n; i++) {
            Event e;
            e.fd       = evs[i].data.fd;
            e.readable = (evs[i].events & EPOLLIN)  != 0;
            e.writable = (evs[i].events & EPOLLOUT) != 0;
            e.error    = (evs[i].events & (EPOLLERR | EPOLLHUP)) != 0;
            out.push_back(e);
        }
        return n;
    }
    const char* name() const override { return "epoll"; }

private:
    bool ctl(int op, int fd, bool r, bool w) {
        struct epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.data.fd = fd;
        ev.events  = (r ? EPOLLIN : 0) | (w ? EPOLLOUT : 0);
        return epoll_ctl(ep_, op, fd, &ev) == 0;
    }
    int ep_ = -1;
};
#endif

// ── poll (portable fallback; Windows uses WSAPoll) ──────────────────────────
// O(n) per wait rather than O(1), so it does not scale to 100k descriptors.
// It exists so the server runs everywhere, not so it runs fast everywhere.
class PollBackend : public Backend {
public:
    bool ok() const { return true; }

    bool add(int fd, bool r, bool w) override {
        if (idx_.count(fd)) return mod(fd, r, w);
        idx_[fd] = fds_.size();
#if defined(_WIN32)
        WSAPOLLFD p;
        p.fd = (SOCKET)fd;
#else
        struct pollfd p;
        p.fd = fd;
#endif
        p.events = (short)((r ? POLLIN : 0) | (w ? POLLOUT : 0));
        p.revents = 0;
        fds_.push_back(p);
        return true;
    }
    bool mod(int fd, bool r, bool w) override {
        auto it = idx_.find(fd);
        if (it == idx_.end()) return add(fd, r, w);
        fds_[it->second].events = (short)((r ? POLLIN : 0) | (w ? POLLOUT : 0));
        return true;
    }
    bool del(int fd) override {
        auto it = idx_.find(fd);
        if (it == idx_.end()) return true;
        size_t pos = it->second, last = fds_.size() - 1;
        if (pos != last) {                       // swap-erase, then fix the index
            fds_[pos] = fds_[last];
            idx_[(int)fds_[pos].fd] = pos;
        }
        fds_.pop_back();
        idx_.erase(it);
        return true;
    }

    int wait(std::vector<Event>& out, int timeoutMs) override {
        out.clear();
        if (fds_.empty()) return 0;
#if defined(_WIN32)
        int n = WSAPoll(fds_.data(), (ULONG)fds_.size(), timeoutMs);
#else
        int n = poll(fds_.data(), (nfds_t)fds_.size(), timeoutMs);
#endif
        if (n < 0) return wouldBlock() ? 0 : -1;
        for (const auto& p : fds_) {
            if (!p.revents) continue;
            Event e;
            e.fd       = (int)p.fd;
            e.readable = (p.revents & POLLIN)  != 0;
            e.writable = (p.revents & POLLOUT) != 0;
            e.error    = (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            out.push_back(e);
        }
        return (int)out.size();
    }
    const char* name() const override { return "poll"; }

private:
#if defined(_WIN32)
    std::vector<WSAPOLLFD> fds_;
#else
    std::vector<struct pollfd> fds_;
#endif
    std::unordered_map<int, size_t> idx_;
};

// Pick the best backend the platform offers, falling back to poll if the
// preferred one cannot be created (a low fd limit, a restricted sandbox).
inline std::unique_ptr<Backend> makeBackend() {
#if defined(BANTU_LOOP_KQUEUE)
    {
        auto b = std::unique_ptr<KqueueBackend>(new KqueueBackend());
        if (b->ok()) return std::unique_ptr<Backend>(b.release());
    }
#elif defined(BANTU_LOOP_EPOLL)
    {
        auto b = std::unique_ptr<EpollBackend>(new EpollBackend());
        if (b->ok()) return std::unique_ptr<Backend>(b.release());
    }
#endif
    return std::unique_ptr<Backend>(new PollBackend());
}

} // namespace bantu_loop
