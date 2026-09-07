# sua server architecture

The reference design for sua's HTTP + WebSocket server: **what** it is, **how** it works, and **why**
each decision was made. Read this before changing `event_loop.hpp` or the server functions in
`evaluator.hpp`. Where a decision rejected an obvious alternative, the reason is recorded — those
notes are the point of this document.

**Status:** Phase 1 (correctness scaffolding) landed. Phase 2 (the event loop) in progress.
The staging is in §8.

---

## 1. What we are building, and why

sua's server started as a synchronous accept loop: one connection handled to completion before the
next was accepted. Simple and correct, but a single WebSocket connection would block the server
forever, because a WS connection is *held open by design*.

Upstream fixed that by giving each connection its own detached thread. That unblocked WebSockets and
introduced three defect classes, all measured (§2). More importantly, thread-per-connection is the
wrong shape for the goal: **real-time WebSocket performance at 100k+ devices**.

The target architecture is a **single-threaded event loop per worker process**, with `SO_REUSEPORT`
spreading workers across cores. This is NGINX's model and Node's model. It is chosen because it is
the only one of the candidates that is simultaneously fast, scalable, secure, maintainable and
invisible to the people writing Bantu apps.

### Design goals, in priority order

1. **Correctness.** No data races. Not "unlikely" — impossible by construction.
2. **Real-time latency.** Sub-50 ms broadcast under load; no lock contention, no context-switch storm.
3. **Scale.** 100k concurrent connections on one machine; millions across a fleet.
4. **Client battery.** A design that lets a phone's radio sleep (§6).
5. **Security.** Bounded resources; RFC-conformant framing; no CSWSH.
6. **Invisibility.** Bantu app code does not change. At all.

---

## 2. The defects this replaces

Measured on `upstream-sync` before Phase 1.

### 2.1 The interpreter raced on itself

`bantuCallFunction` does `env_ = callEnv; … env_ = prevEnv;` on a single shared `Evaluator` member,
while every connection ran on its own thread. There was not one mutex in `evaluator.hpp`.

Identical workload, 20 concurrent requests to a handler that accumulates a per-request value:

| build | corrupted/lost replies | handler errors |
|---|---|---|
| single-threaded (pre-merge) | 0 | 0 |
| threaded (upstream) | **17** | **16** |

Failures surfaced as `[REFERENCE ERROR] Undefined variable: n` — one request's scope torn out from
under another. **Silent data corruption, not a crash**, which is the worst failure mode: a server
that returns confidently wrong answers.

### 2.2 The WebSocket client registry raced

`bantuWsTable()` is a bare `std::unordered_map`, inserted on connect, erased on disconnect, and
iterated by six `sua.ws.*` builtins — from many threads, unguarded. Concurrent insert/erase on an
`unordered_map` is undefined behaviour: corrupted buckets, infinite loops, or a crash.
`bantuNextWsId` / `bantuNextUdpId` / `bantuNextFileId` were non-atomic `++`.

### 2.3 Security holes

| Finding | Impact |
|---|---|
| No `Origin` check on WS upgrade | **Cross-Site WebSocket Hijacking** — any site can open an authenticated socket with the victim's cookies |
| Client→server masking read but never enforced | RFC 6455 §5.1 requires closing on an unmasked client frame; skipping it enables cache poisoning through intermediaries |
| No frame or message size cap | Frames silently truncate; per-byte `payload += c` is quadratic |
| No connection cap | **Trivial DoS**: N connections → N threads → ~8 MB stack each → OOM |
| 16 KB single `recv` for headers | Larger header blocks silently truncated |

### 2.4 Why locking alone was not the answer

A global interpreter lock fixes 2.1 and 2.2 and nothing else. It leaves ~8 MB of stack per idle
connection, leaves the context-switch storm, and leaves every security gap. It was taken as
**temporary scaffolding** (§8, Phase 1) purely so the branch is safe to test while the real fix
lands — and it is deleted in Phase 2.

---

## 3. How it works

### 3.1 The loop

One thread per worker process:

```
  register listener fd
  loop:
      events = backend.wait(timeout = next_timer_deadline)
      for each ready fd:
          listener  -> accept, create Connection, register
          readable  -> read into buffer, advance the state machine
          writable  -> drain the write queue
      run expired timers (ping, idle, handshake)
```

No thread ever touches another connection's state, and **all Bantu execution happens on this
thread** — which is what makes §2.1 and §2.2 impossible rather than merely guarded.

### 3.2 Connections are state machines, not threads

```
struct Connection {
    int fd;
    Kind kind;                  // Http | WebSocket
    std::string readBuf;        // accumulates until a full request/frame is present
    std::string writeBuf;       // pending bytes; drained on writable
    ParseState state;           // ReadingHeaders | ReadingBody | Dispatching | Writing | Closing
    uint64_t lastActivityMs;    // drives idle timeout
    // WebSocket only:
    std::string fragmentBuf;    // assembled continuation frames
    bool awaitingPong;
};
```

An HTTP connection walks `ReadingHeaders → ReadingBody → Dispatching → Writing → keepalive|close`.
A WebSocket connection is simply one that stays registered and re-enters on each readable frame.
This is the whole reason a WS connection costs ~4 KB instead of ~8 MB.

**Parsing is incremental.** The current code assumes a request's headers and a WS frame each arrive
whole in one `recv` — true in testing, false under real networks and the source of the 16 KB
truncation. The state machine reads what is available and resumes.

### 3.3 The backend abstraction

```
struct Backend {
    bool add(int fd, bool readable, bool writable);
    bool modify(int fd, bool readable, bool writable);
    bool remove(int fd);
    int  wait(std::vector<Event>& out, int timeoutMs);
};
```

| Platform | Implementation | Why |
|---|---|---|
| macOS / BSD | `kqueue` | O(1) readiness; unifies sockets, timers and signals under one API — exactly what the ping/idle timers need |
| Linux | `epoll` | O(1); the decades-stable default |
| Windows | `WSAPoll` (IOCP later) | `WSAPoll` is already in the tree from the UDP work; IOCP is a *completion* model and needs a different shape, so it is deferred rather than faked |

`poll.h` and `WSAPoll` are already included (`evaluator.hpp`), so the portability shim was
half-built before this work started.

---

## 4. Why this architecture — the alternatives, and why they lost

| Alternative | Verdict | Reason |
|---|---|---|
| **Global interpreter lock** | Rejected as the answer; kept as temporary scaffolding | Fixes correctness only. Leaves 8 MB/connection, the context-switch storm, and every security gap. Serialises all Bantu execution forever. |
| **Revert to single-threaded** | Rejected | Effectively deletes `sua.ws`: one open WebSocket holds the loop and the server serves nobody else. |
| **libuv** | Rejected | Adds a runtime dependency to every Bantu binary. This project chose a **from-scratch P-256 over linking OpenSSL** for exactly this reason; the same standard applies here. A self-contained loop is ~400 lines against three well-documented kernel APIs. |
| **io_uring now** | Deferred, not designed out | Linux-only, wants kernel 6.1+, and changes failure modes. A naive swap measures **1.06–1.10×** — noise; the headline 2.3× needs registered buffers and zero-copy re-engineering. Current guidance is that **epoll remains the correct default in 2026**. The `Backend` interface exists so it can be added later as one more implementation. |
| **Per-connection Evaluator (true multi-core in one process)** | Out of scope | A language-semantics change, not a server change: what does a shared global `$counter` mean across parallel requests? `SO_REUSEPORT` delivers the multi-core win without answering that. |

---

## 5. Scaling

### 5.1 Within a machine: `SO_REUSEPORT`

N worker processes, one per core, each with **its own event loop and its own interpreter**, all
listening on the same port. The **kernel** load-balances accepts.

- Removes the thundering herd — each worker has its own listening socket rather than N workers
  contending on one.
- Connections tend to be processed by the core that accepted them (cache locality).
- **No shared mutable state between workers**, so §2.1 and §2.2 cannot recur as the codebase grows.

Caveat, recorded honestly: when a worker stalls on a blocking call, it stalls both the connections it
already holds *and* the new ones the kernel has already assigned to it. That is the §7 trade-off, and
`SO_REUSEPORT` reduces the blast radius to 1/N rather than eliminating it.

### 5.2 Memory

With tuned TCP buffers, per-connection RAM goes **under 3.5 KB**; a single 64 GB node has been shown
to hold **10M+ concurrent WebSockets**. Against ~8 MB per thread today that is roughly a **2,000×**
improvement per connection.

- **100k devices:** comfortably one machine.
- **Millions:** multiple machines plus a pub/sub bus for cross-worker and cross-machine broadcast.

### 5.3 Cross-worker broadcast

`sua.ws.broadcast` reaches only the clients on the calling worker. Once `SO_REUSEPORT` lands, a
broadcast bus is required for correctness, not just scale. Phase 4.

---

## 6. Client battery — why this shaped the protocol defaults

This changed the design, so it is recorded in full.

For a mobile client the dominant energy cost is **keeping the radio awake**, not bytes transferred.
The evidence:

- **Client-initiated pings prevent the modem powering down.** The ntfy Android client measured
  **2–3× longer battery life** purely by disabling client-side ping.
- A 45 s keepalive is *barely* enough for the modem to idle.
- But **anything under ~60 s breaks users** behind firewalls and NAT that reap silent TCP
  connections — so the interval cannot simply be raised without bound.
- Persistent sockets beat polling for *frequent* updates (chat, collaboration, voice signalling).
  For *rare* updates a held socket costs more battery than it saves.

**Therefore:**

1. **Ping is server-initiated only.** Clients never ping. The server sends WS ping frames and the
   client's TCP stack answers without waking the application.
2. **Default interval 60 s**, configurable. Below 60 s is permitted but documented as harmful.
3. **Backgrounded clients should close the socket and rely on Web Push.** `sua.push` already ships
   the full RFC 8291/8292 stack, so the battery-correct pattern is available today:
   **foreground → WebSocket; background → push.**

That last point is the real payoff: the PWA/push work and the WebSocket work are two halves of one
story, and an app that uses both correctly is dramatically kinder to a phone than one holding a
socket open in the background.

---

## 7. The trade-off, stated plainly

**A blocking builtin stalls its worker's loop.** There are 8 blocking call sites reachable from a
handler (`curl_easy_perform`, `sqlite3_step`, `sleep`).

This is the same constraint Node has, and it is managed the same way: Phase 4 moves `sua.http.*` to
curl-multi driven by the loop, and offloads long sqlite work. Until then it is documented, and
`SO_REUSEPORT` limits the damage to one worker of N.

It is worth being clear that this is **strictly better than what it replaces**. Today a blocking
handler stalls a thread *and* corrupts other requests' scopes.

---

## 8. Delivery staging

| Phase | What | State |
|---|---|---|
| 0 | This document | done |
| 1 | Correctness scaffolding: recursive-mutex interpreter lock, WS table mutex, atomic counters, race reproducer as a committed test | done |
| 2 | `event_loop.hpp` + non-blocking rewrite of the three server functions; **Phase 1 locks deleted** | in progress |
| 3 | Security hardening (§9), each item with a test | |
| 4 | `SO_REUSEPORT` workers; curl-multi `sua.http.*`; broadcast bus | |
| 5 | Upstream PR to `AsseySilivestir/Bantu` with reproducer and fix | |

### Phase 1's lock ordering (while it exists)

Interpreter lock **then** WS table lock, never the reverse. The table lock is **never held across a
`bantuWsCallback` call** — that inversion is the one deadlock available in this design. Phase 2
removes both locks, and with them the rule.

---

## 9. Security model

An event loop is *inherently* more DoS-resistant than thread-per-connection: a slow or idle client
costs a ~4 KB struct instead of an 8 MB thread, so Slowloris-style attacks stop being fatal. Owning
the connection lifecycle explicitly is also what makes the rest of this enforceable at all.

| Control | Default | Fixes |
|---|---|---|
| `Origin` allowlist on WS upgrade | same-origin | CSWSH (§2.3) |
| Enforce client→server masking; close 1002 | on | RFC 6455 §5.1 |
| Max frame size | 1 MiB | memory exhaustion |
| Max assembled message (continuations) | 8 MiB | unbounded fragmentation |
| Max header block | 64 KiB | the 16 KB truncation |
| Max body | 8 MiB | memory exhaustion |
| Max connections (process) | 10,000 | fd/memory exhaustion |
| Per-IP connection cap | 100 | single-client DoS |
| Handshake / header timeout | 10 s | Slowloris |
| Idle timeout | 300 s | connection leaks |
| Write backpressure cap | 4 MiB | slow-reader memory growth |
| UTF-8 validation on text frames | on | RFC 6455 §8.1 |

All are configurable through `sua.server.limits({...})`; the defaults are the safe ones.

---

## 10. Testing

| Gate | What it proves |
|---|---|
| `tests/sua_concurrency_test.sh` | The race reproducer: a handler looping 400×, N concurrent clients via `xargs -P`, asserting `want == got` on every reply. Failed **17/20** before Phase 1; must be 0 and stay 0. |
| Autobahn TestSuite | ~500-case RFC 6455 conformance — objective proof of framing correctness, including every edge §2.3 lists |
| ThreadSanitizer build | Races the reproducer misses |
| 100k idle connections | RSS stays flat; the scale claim in §5.2 |
| p50/p99 broadcast latency | Beats the threaded build under load |
| Full regression | webpush selftest 91, sua_pwa 106, webpush 53, arctic 72, orm 61, scope 8, lang_oop 15, `sua_pwa_http_test.sh` 41 — on default **and** Arrow+sodium builds |
| Browser interop | Binary voice frames, ping/keepalive, abrupt disconnect |

A single-threaded event loop is materially **easier** to test than threads: it is deterministic, so
failures reproduce instead of appearing one run in fifty.

---

## 11. What does not change

Bantu application code. This is a runtime change, not an API change:

```bantu
sua.server.get("/api/x", def($req, $res) { $res.json({"ok": true}); });
sua.ws.on("message", def($m) { sua.ws.broadcast($m.data); });
sua.server.listen(3000);
```

Handlers keep their synchronous shape — no callbacks, no promises, no `async`. The loop lives
entirely beneath the runtime. Someone learning sua after this change learns exactly what they would
have learned before it.

---

## 12. References

- RFC 6455 — The WebSocket Protocol (§5.1 masking, §8.1 UTF-8, close codes)
- RFC 8030 / 8291 / 8292 — Web Push, and the battery-correct background path (see
  [pwa-research.md](pwa-research.md))
- NGINX socket sharding / `SO_REUSEPORT` — the multi-worker model
- Autobahn TestSuite — WebSocket conformance
- `kqueue(2)`, `epoll(7)`, `WSAPoll`
