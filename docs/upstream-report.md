# Upstream report — draft for review before sending

**Status: NOT SENT.** Nothing has been pushed to `AsseySilivestir/Bantu` and no issue or PR has been
opened. This file is the text to review first.

Target branch: **`sua.udp-feature`** (`1f8d973`, "fix: multi-threaded HTTP accept loop — WebSocket
sync works", 2026-09-06). Not `main` — `main` predates the `def($req, $res)` parameter syntax and
cannot run any of the reproducers below.

Everything here was reproduced by building `upstream/sua.udp-feature` unmodified in a worktree and
running it on macOS 24.6.

---

## Recommended shape: one issue, then one small PR

A 28-commit architectural PR is not a reasonable thing to hand a maintainer cold. Proposed:

1. **An issue** with the three reproducers below. It is genuinely useful on its own — the crash is
   trivially triggerable by anyone.
2. **A one-line PR** for the SIGPIPE kill, which is unconditionally safe and independent of any
   architecture argument.
3. **Offer** the event-loop branch in the issue thread, for the maintainer to take or leave.

The reason for that order is finding 4: the one-line fix is necessary but **not sufficient**, and
saying so up front is more useful than shipping a patch that appears to fix things.

---

## Finding 1 — one unauthenticated request kills the server

Any client that sends a request and closes without reading the reply makes the server's next
`send()` raise `SIGPIPE`, whose default action terminates the process.

```python
import socket, struct
for i in range(50):
    s = socket.socket(); s.connect(("127.0.0.1", PORT))
    s.sendall(b"GET /big HTTP/1.1\r\nHost: x\r\n\r\n")
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))  # RST, not FIN
    s.close()
```

Against a handler returning ~40 KB: **dead at attempt 10, exit 141** (128 + 13 = SIGPIPE). Every
subsequent connection is refused. No handshake, no credentials, no authentication.

**Fix** (the whole thing):

```c
#else
    signal(SIGPIPE, SIG_IGN);
#endif
```

at the top of `bantuStartHttpServer`, beside the existing `WSAStartup` branch, plus `#include
<csignal>`. `send()` then returns `-1`/`EPIPE`, which the existing error paths already handle.

Worth scoping it to server startup rather than process startup, so `bantu run script.b | head` still
terminates on a closed pipe like any other CLI program.

## Finding 2 — the interpreter races on itself, and one client is enough

`bantuCallFunction` does `env_ = callEnv; … env_ = prevEnv;` on one shared `Evaluator` member, while
every connection runs on a detached thread. There are no mutexes in `evaluator.hpp`.

Handler looping 400 times over a local `$i`, 40 concurrent requests:

| | result |
|---|---|
| requests with no reply | **32 of 40** |
| corrupted replies | 2 of 40 — req 16 wanted 6400, got 5664; req 33 wanted 13200, got 9042 |
| handler errors | 25 × `[REFERENCE ERROR] Undefined variable: i` |

That last one is one request's scope being torn out from under another. It is silent data
corruption, not a crash — a value-checking test is the only thing that finds it.

**This is not only a concurrency problem.** The reproducer for finding 1 opens connections strictly
one at a time, and it still produces `Undefined variable: i` in the log, because detached thread
lifetimes overlap: the next connection's handler starts while the previous thread is still finishing.
A single sequential user is enough.

## Finding 3 — WebSocket security and framing

Running an RFC 6455 conformance suite against the branch: **12 of 15 assertions fail.**

| | |
|---|---|
| `Origin` not checked on upgrade | **Cross-Site WebSocket Hijacking** — any website can open an authenticated WebSocket to a Bantu server using the visitor's cookies |
| masking read but never enforced | RFC 6455 §5.1 requires closing on an unmasked client frame |
| no frame or message size cap | memory exhaustion; `payload += c` per byte is also quadratic |
| frames parsed from a single `recv()` | **anything larger than one TCP segment is silently truncated** — a 200 KB message arrives mangled, which is constant for binary/voice frames |
| continuation frames never reassembled | fragmented messages are lost |
| no UTF-8 validation on text frames | RFC 6455 §8.1 |
| header block read with one 16 KB `recv` | larger header blocks truncated rather than answered `431` |

The truncation one is worth separating from the security items: it is not an attack, it is ordinary
traffic being corrupted in normal use.

## Finding 4 — the one-line fix is necessary but not sufficient

Applying the SIGPIPE fix to `sua.udp-feature` and re-running the same reproducer:

| | before fix | after fix |
|---|---|---|
| disconnects survived | 9 | 141 / 150 / 154 / 155 |
| exit | 141 (SIGPIPE) | **139 (SIGSEGV), 134 (SIGABRT)** |

The crash signature is **nondeterministic across runs**, and every abort is immediately preceded in
the log by `[REFERENCE ERROR] Undefined variable: i`. So the signal was masking memory corruption
from finding 2; removing it moves the failure later and makes it less predictable, not absent.

This is why the SIGPIPE patch should be offered as what it is — a strict improvement that closes a
trivial remote kill — rather than as a fix for the crash.

---

## What we did on our side, offered if useful

`minjaezekiel/Bantu`, branch `upstream-sync`. Thread-per-connection replaced with a single-threaded
event loop (kqueue / epoll / poll behind one interface, no new dependency), which removes findings 2
and 3 by construction: one thread owns every connection, so there is no shared interpreter state to
race on and every lock could be deleted rather than added.

Measured against the thread-per-connection build, same machine, same load:

- 3,000 idle WebSocket connections: **0.6 KB/conn on 1 thread**, vs 21.9 KB/conn on 1,647 threads.
- 300 active clients: 12% more throughput, **2.8× better worst-case latency** (73 ms vs 208 ms).
- p99 under load is slightly *worse* (57 vs 45 ms) — the honest cost of FIFO fairness.

Plus `SO_REUSEPORT` worker processes for multi-core, and a supervisor that respawns a dead worker.

One portability note that may save time: **`SO_REUSEPORT` does not load-balance TCP accepts on
macOS.** Four workers all bound and listened, and every test connection went to worker 0. Linux 3.9+
hashes the 4-tuple across sockets; the BSDs added a separate `SO_REUSEPORT_LB` precisely because
plain `SO_REUSEPORT` does not balance, and macOS has no equivalent. The portable answer is the
classic pre-fork model — one shared listening socket created before the fork and inherited — which
took a macOS 4-worker split from 12/0/0/0 to 103/99/99/99 over 400 concurrent requests.

Tests that come with it, if wanted: an RFC 6455 conformance suite (15), a race reproducer that
asserts `want == got` on every reply (4), an event-loop backend suite covering both kqueue and the
portable `poll` fallback (28), and worker/bus tests (15).

Happy to split any of it into smaller reviewable pieces, or to just leave the reproducers.
