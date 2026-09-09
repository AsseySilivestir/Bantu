# sua server architecture

The reference design for sua's HTTP + WebSocket server: **what** it is, **how** it works, and **why**
each decision was made. Read this before changing `event_loop.hpp` or the server functions in
`evaluator.hpp`. Where a decision rejected an obvious alternative, the reason is recorded — those
notes are the point of this document.

**Status:** Phases 1, 2 and 3 landed — the event loop is live and the Phase 1 locks are gone.
Phase 4 (SO_REUSEPORT, non-blocking I/O builtins) next. The staging is in §8.

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
listening on the same port.

**On Linux** the kernel load-balances accepts across the workers' sockets. **It does not do this
everywhere**, and the difference is invisible until measured — see §12.1, where four macOS workers
all listened happily and every test connection went to worker 0.

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

### 5.4 Measured, not assumed

Same machine, same workload, event loop vs the thread-per-connection build it replaced
(commit `d306c8f`, built in a worktree so both were measured on equal terms).

**3,000 idle WebSocket connections:**

| | event loop | thread-per-connection |
|---|---|---|
| RSS growth | 4.05 → 5.80 MB | 3.99 → 69.8 MB |
| per connection | **0.6 KB** | 21.9 KB |
| OS threads | **1** | 1,647 |

**36× less memory per connection**, and one thread instead of sixteen hundred. (macOS commits
thread stacks lazily, so the threaded build's RSS is far below its 8 MB-per-thread reservation —
the address space and scheduler pressure are the harder limits.)

**Latency, 2,000 idle connections + one probe:** p50 0.05 ms, p99 0.10 ms — against 0.04/0.08 ms
threaded. **Equivalent, not better**, and worth stating plainly: an idle thread blocked in `recv`
costs nothing, so there is no context-switch storm to win against until the connections are busy.

**300 concurrently active clients, 4 s:**

| | event loop | thread-per-connection |
|---|---|---|
| messages echoed | **134,964** | 120,090 |
| probe p50 | **14.9 ms** | 16.5 ms |
| probe p99 | 57.5 ms | **45.5 ms** |
| probe max | **73.3 ms** | 207.6 ms |

12% more throughput and a **2.8× better worst case** — no connection gets starved waiting for the
scheduler. The event loop's p99 is slightly worse, which is the honest cost of strict FIFO fairness:
threads let a lucky request jump ahead, which flatters p99 while producing that 207 ms tail.

The headline is not latency. It is that the same box now holds 36× the connections, on one thread,
with no locks anywhere — and the correctness properties that follow from that.

### 5.3 Cross-worker broadcast

`sua.ws.broadcast` reaches only the clients on the calling worker, so a broadcast bus is required
for correctness, not just scale. Designed and built in **§12.3**.

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

This is the same constraint Node has. Phase 4 was expected to remove it by driving `sua.http.*`
from the loop with curl-multi; **that turned out not to be achievable without coroutines, and §12.4
records why, including the re-entrancy shortcut that must not be taken.** What Phase 4 does deliver
is `sua.http.all` (many outbound requests in parallel within one call, so the stall is `max(t)` not
`sum(t)`) and `SO_REUSEPORT`, which limits the damage to one worker of N.

It is worth being clear that this is **strictly better than what it replaces**. Today a blocking
handler stalls a thread *and* corrupts other requests' scopes.

---

## 8. Delivery staging

| Phase | What | State |
|---|---|---|
| 0 | This document | done |
| 1 | Correctness scaffolding: recursive-mutex interpreter lock, WS table mutex, atomic counters, race reproducer as a committed test | done |
| 2 | `event_loop.hpp` + non-blocking rewrite of the three server functions; **Phase 1 locks deleted** | done — see §5.4 |
| 3 | Security hardening (§9), each item with a test | done — `tests/sua_ws_security_test.sh` 13/13 |
| 4 | `SO_REUSEPORT` workers; broadcast bus; parallel `sua.http.all` | done — see §12 |
| 5 | Upstream report to `AsseySilivestir/Bantu` | drafted, **not sent** — [upstream-report.md](upstream-report.md) |

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

All are configurable through `sua.server.limits({...})`; the defaults are the safe ones:

```bantu
sua.server.limits({
    "max_connections":      10000,
    "max_header_bytes":     65536,
    "max_ws_frame_bytes":   1048576,
    "max_ws_message_bytes": 8388608,
    "header_timeout_ms":    10000,
    "ws_check_origin":      true,
    "ws_allowed_origins":   ["https://app.example.com"]   // [] = same-origin only
});
```

Called with no argument it reports the current settings plus `live_connections`.

**Landed in Phase 3** (`tests/sua_ws_security_test.sh`, 13/13): the `Origin` allowlist,
masking enforcement, frame and message caps, control-frame rules, RSV rejection, UTF-8
validation, the connection cap with 503 load shedding, the header-block cap with 431, and a
receive timeout as the Slowloris defence.

Two correctness bugs fell out of the same work, both of which had been corrupting data silently:

- **WebSocket frames were parsed from whatever a single `recv()` returned**, so any frame
  spanning more than one TCP segment was truncated. A 200 KB message now round-trips intact;
  before, voice frames were being mangled routinely.
- **Continuation frames were not reassembled at all.** Fragmented messages are now joined,
  bounded by `max_ws_message_bytes`.

Write backpressure (a 4 MiB per-connection cap, so a client that stops reading cannot make the
server buffer without bound) and the idle timeout both landed with the loop in Phase 2.

**Per-IP connection caps** now close the last item: `max_connections_per_ip` bounds how much of the
table one source address can take. It is **off by default**, and that default is a judgement rather
than an oversight — behind a reverse proxy every connection shares the proxy's address, so a per-IP
cap would throttle the entire site at once. It belongs on when the server is directly internet-facing.

Two limits remain inherent rather than open: the cap is **per worker**, so the effective
process-group limit is n x the value; and it keys on the source address, which under
`sua.server.workers(n)` also means n separate tables.

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

## 12. Phase 4 — multi-core, and what a worker actually shares

Phase 2 gave one loop that holds tens of thousands of connections on one thread. That thread is
still **one core**. Phase 4 is about the other fifteen.

### 12.1 The worker model

`sua.server.workers(n)` forks `n` processes, each running its own loop and its own interpreter.

**How they receive connections depends on the platform, and this was got wrong first.** The original
design said "each worker creates its own listening socket with `SO_REUSEPORT`; the kernel
load-balances across them." That is true on Linux and false on macOS. Measured: four workers, all
four confirmed listening by `lsof`, and **all twelve test connections delivered to worker 0** while
the other three sat idle. Linux 3.9+ hashes each connection's 4-tuple across the listening sockets;
macOS and the BSDs allow the shared bind but wake one socket. FreeBSD later added a *separate*
option, `SO_REUSEPORT_LB`, precisely because plain `SO_REUSEPORT` does not balance — and macOS has
no equivalent.

So there are two strategies, chosen by `bantu_workers::kernelBalancesAccepts()`:

| | listener | distribution | cost |
|---|---|---|---|
| **Linux** | one socket **per worker**, `SO_REUSEPORT`, created after the fork | kernel hashes the 4-tuple | none — NGINX's model |
| **macOS / BSD** | **one shared socket**, created before the fork and inherited | workers race to `accept()` | a mild thundering herd |

The pre-fork shared listener is the classic Apache model. It trades a few wasted wakeups for
actually using the cores, which is the right trade when the alternative is one busy worker and N-1
idle ones. Measured after the fix, 400 concurrent requests across 4 macOS workers: **103 / 99 / 99 /
99**. Under strictly *sequential* requests the same setup skews hard (183 / 11 / 6 / 0), because
whichever worker wakes first always wins an uncontended race — that is expected, and it is also the
load where distribution does not matter.

The fork point is load-bearing: **after** the Bantu program has run (routes registered, handlers
defined, globals initialised) and **before** anything is accepted. Every worker therefore starts
from an identical, fully-configured interpreter, and no request has been served yet by anyone.

```
  bantu run app.b
        │
        ├── program executes: routes registered, $config loaded
        │
        └── sua.server.listen(3000)
                 │
                 ├── fork ──► worker 0 ── listener ── own loop ── own interpreter
                 ├── fork ──► worker 1 ── listener ── own loop ── own interpreter
                 ├── fork ──► worker N ...
                 │            (Linux: one SO_REUSEPORT socket each.
                 │             macOS/BSD: one shared socket, inherited.)
                 │
                 └── parent: never accepts. Supervises + relays the bus.
```

The parent deliberately **does not serve traffic**. It supervises (respawns a worker that dies) and
relays the broadcast bus. Keeping it out of the accept path means a crash in request handling can
never take down the thing that restarts request handling.

### 12.2 What this costs you: global state is per-worker

This is the one thing an app author must understand, so it is stated bluntly.

```bantu
$hits = 0;
sua.server.get("/hit", def($req, $res) { $hits = $hits + 1; $res.send(str($hits)); });
sua.server.workers(4);
```

With 4 workers this counts to roughly `$hits/4` per worker. **`$hits` is not shared.** After the
fork each worker has its own copy, and writes never meet.

That is not a defect to be patched later; it is the direct consequence of the property that makes
the model safe — no shared mutable state means §2.1 and §2.2 cannot come back. Shared state belongs
in something built for it: a database, or the broadcast bus below. **`workers(1)` (the default)
keeps the single-process semantics**, so nothing changes for anyone who does not opt in.

### 12.3 The broadcast bus

With N workers, a WebSocket client is connected to exactly one of them. `sua.ws.broadcast` would
otherwise reach a fraction of the room — a correctness bug, not a scaling limit. So each worker gets
a `socketpair` to the parent, and the parent fans out:

```
   worker 1 ──┐                        ┌──► worker 0   deliver locally
   (broadcast)└──► parent (relay) ─────┼──► worker 2   deliver locally
                                       └──► worker 3   deliver locally
```

The originating worker delivers to its own clients directly and does **not** receive its own frame
back. Wire format is deliberately dull:

```
  [u32 length BE][u8 type][payload]      type 1 text, 2 binary,
                                              3 targeted text, 4 targeted binary
  targeted payload: [u16 idLen BE][client id][data]
```

Both ends are non-blocking with their own outbound buffer, because the relay must never let one
wedged worker stall every other worker's broadcasts. A worker whose bus buffer exceeds its cap
**drops the message and counts the drop** (`sua.server.stats().bus_dropped`) rather than growing
without bound — a broadcast storm must degrade, not OOM.

No Redis, no message broker, no new dependency. Same discipline as the from-scratch P-256: a
socketpair and 200 lines beat a service to operate. Cross-*machine* broadcast is where a real broker
belongs, and that boundary is where it will be added.

Client ids carry the worker index under multiple workers (`ws-2-7` rather than `ws-7`). The id
counter is per-process, so without this **every worker minted `ws-1`** — and a targeted send routed
over the bus would have been delivered to a different person's socket on another worker. Single-
worker mode keeps the original `ws-N` form, so nothing existing changes.

`sua.ws.clients()` enumerates **this worker's** clients unless the roster is switched on with
`sua.server.limits({"ws_roster": true})`. With it, workers publish join/leave deltas on the bus
(`BUS_ROSTER_ADD` / `DEL`) and each keeps the others' ids, so `clients()` returns the union.

Three details that make it correct rather than merely present:

- **Deltas, not periodic dumps** — one small frame per connect/disconnect, the same order as the
  traffic that caused it.
- **A restarted worker asks.** It starts with an empty roster while the others already hold
  connections, so it broadcasts `BUS_ROSTER_REQ` on startup and everyone replies with a full set.
  Without this a respawned worker would never learn about existing clients.
- **The supervisor buries the dead.** A worker that is killed cannot send its own farewell, so the
  supervisor relays an empty `BUS_ROSTER_FULL` for that index — otherwise its clients would sit in
  every other worker's roster until the process exited.

It is **off by default because it is not free**: every worker holds every client id, which for 100k
clients across 8 workers is 800k strings. And it is **eventually consistent** — a client that
connected microseconds ago elsewhere may not appear yet. That is the honest shape of a roster over
an asynchronous bus; making it synchronous would mean blocking the loop on every other worker, which
is exactly the property this architecture exists to avoid.

### 12.4 `sua.http.*` and curl-multi — what is achievable, and what is not

The plan said Phase 4 would "move `sua.http.*` to curl-multi driven by the loop" so that a handler
waiting on an outbound request no longer stalls its worker. **On investigation that is not
achievable without a language change, and it is worth writing down why.**

`sua.http.get()` is a synchronous builtin: the C++ stack runs loop → dispatch → evaluator → builtin →
`curl_easy_perform`. To return to the loop while that request is in flight, that stack has to be
suspended. There are exactly three ways:

1. **A thread per in-flight handler** — reintroduces concurrent interpreter execution, which is
   precisely §2.1. Rejected.
2. **Pump the event loop from inside the blocking builtin** — re-entrant handler execution: two
   Bantu handlers live on one stack, interleaving `env_` swaps. This resurrects §2.1 in a form that
   is *harder* to debug than the original. Rejected, and specifically warned against here because it
   is the tempting shortcut.
3. **Coroutines** — each handler on its own small stack, with `env_` saved and restored as part of
   the context switch. This is the correct long-term answer (it is what Go does) and it preserves
   the synchronous syntax that §11 promises app authors. It is also a real interpreter change, and
   the plan explicitly put per-handler evaluator state out of scope.

So Phase 4 ships the part that is safe and genuinely useful: **`sua.http.all([...])`**, which issues
many requests concurrently through `curl_multi` inside a single builtin call and returns the
responses in request order. The worker still blocks — but for `max(t)` instead of `sum(t)`.

That is not a consolation prize. The heaviest outbound workload sua has is **Web Push fan-out**,
which is N independent HTTPS POSTs to N subscribers; sequentially that is N round trips, and it is
the operation most likely to stall a worker in practice. `sua.http.all` turns it into one.

Coroutine-based handler suspension is recorded as the successor, with its prerequisite named:
per-coroutine `env_` save/restore in the evaluator.

### 12.5 Platform

`fork` and `SO_REUSEPORT` are POSIX. On Windows, `sua.server.workers(n)` logs that multi-worker mode
is unavailable and runs single-worker; every other guarantee is unchanged. Multi-worker Windows
would need a different mechanism entirely (a shared listening handle passed to child processes),
and single-worker Windows is not a regression against anything that shipped.


---

## 13. References

- RFC 6455 — The WebSocket Protocol (§5.1 masking, §8.1 UTF-8, close codes)
- RFC 8030 / 8291 / 8292 — Web Push, and the battery-correct background path (see
  [pwa-research.md](pwa-research.md))
- NGINX socket sharding / `SO_REUSEPORT` — the multi-worker model
- Autobahn TestSuite — WebSocket conformance
- `kqueue(2)`, `epoll(7)`, `WSAPoll`
