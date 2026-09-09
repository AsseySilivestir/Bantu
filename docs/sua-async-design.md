# Suspending handlers without async syntax — the design

**Status: design only. No code written. One decision is needed before starting (§7).**

This is the successor to `sua-architecture.md` §12.4, which recorded that `sua.http.all` narrows the
blocking problem but does not remove it, and that coroutines are the real fix. This document does
the research §12.4 deferred and answers the six questions.

---

## 1. The problem, precisely

Each worker is one event loop on one thread. A handler runs to completion on that thread, so while
it waits on anything, **every other connection on that worker waits too**.

The blocking call sites reachable from a handler, counted in `evaluator.hpp`:

| call | sites | can it be made to suspend? |
|---|---|---|
| `curl_easy_perform` (`sua.http.*`) | 1 | **yes** — curl_multi exposes its fds |
| `curl_multi_poll` (`sua.http.all`) | 1 | **yes** — same |
| `sqlite3_step` (`sua.sqlite.*`) | 6 | **no** — synchronous by design; needs offload |
| `std::this_thread::sleep_for` | 1 | **yes** — trivially a loop timer |

`SO_REUSEPORT` workers already cut the blast radius to 1/n, and `sua.http.all` collapses a fan-out
into one wait. Neither helps a single slow outbound call on a busy worker.

## 2. Why C++20 coroutines are the wrong tool here

The obvious answer is wrong, and it is worth writing down why so it is not proposed again.

C++20 `co_await` is a **compile-time** transformation: a function containing `co_await` becomes a
state machine, and any function that awaits it must itself be a coroutine. Bantu is a tree-walking
interpreter — `evalNode` recurses through `evalBinary`, `evalCall`, `evalBlock` and thirty siblings.
Making a builtin suspend would require **every one of those to become a coroutine**, because the
suspension has to propagate up the whole C++ call stack.

That is a rewrite of the entire evaluator, and it would put heap-allocated coroutine frames on the
hot path of *all* evaluation — including the arithmetic in a `while` loop that never suspends.
Rejected on both counts.

The requirement is **stackful** suspension: freeze a real C++ call stack mid-recursion and resume it
later. Three mechanisms do that.

## 3. The three real options

### 3.1 `ucontext` — the classic, and deprecated

`makecontext`/`swapcontext` were **removed from POSIX in 2008**. macOS still ships them but marks
them deprecated (they need `_XOPEN_SOURCE` and warn), Windows has no equivalent (Fibers instead), and
they interact badly with sanitizers and with stack-unwinding for C++ exceptions — which matters here,
because Bantu handler errors propagate as C++ exceptions.

Building on a deprecated API for a production language is the opposite of the long-term-support
requirement. **Rejected.**

### 3.2 Hand-rolled assembly context switches

What Boost.Context does. Fastest (~20ns), and correct — but it means per-architecture assembly for
x86-64, arm64, and whatever comes next, plus unwind-table registration so exceptions still work.
That is a permanent maintenance burden in a project that deliberately has no third-party runtime
dependencies. **Rejected** — the cost is not repaid, since suspensions happen at I/O frequency, not
in a hot loop.

### 3.3 Threads with a strict handoff baton — **recommended**

Each suspendable handler runs on its own OS thread, but **only one thread is ever runnable at a
time**, enforced by a mutex and condition variable. The loop hands the baton to a handler; the
handler hands it back when it suspends or finishes.

This is not the thread-per-connection model that Phase 2 deleted. The difference is the whole point:

| | thread-per-connection (deleted) | strict handoff |
|---|---|---|
| threads | one per **connection** | one per **suspended handler** |
| run simultaneously? | **yes** — hence the data races | **never** — the baton is the interpreter lock |
| count at 10k idle connections | 10,000 | **0** |
| interpreter races | §2.1, §2.2 | impossible by construction |

A connection costs nothing until its handler actually suspends. The thread count tracks *concurrent
outbound I/O*, which is tens, not thousands — and it is capped (§6).

It uses only `std::thread`, `std::mutex`, `std::condition_variable`: C++11, stable for a decade and
a half, no new dependency, portable to Windows unchanged, and **debuggable with ordinary tools** —
a suspended handler is a real thread with a real backtrace in gdb or lldb, which is not true of any
of the alternatives. It also works under ThreadSanitizer, which the deprecated and assembly routes
do not.

## 4. What a suspended handler must carry

This is the finding that makes the whole thing tractable, and it was measured rather than assumed.
`Evaluator`'s mutable state is **five fields**:

| field | live across a suspend? |
|---|---|
| `env_` | **yes** — the current scope chain, swapped on every call |
| `currentClassName_` | **yes** — `super()` resolution, live inside any method |
| `filePathStack_`, `loadedModules_`, `includeDepth_` | only during `include`, which cannot suspend today |

`globalEnv_` and `classRegistry_` are effectively immutable once the program has loaded, so they are
shared rather than saved.

So the coroutine context is a five-field struct, saved on suspend and restored on resume. Compare
that with the alternative of per-connection `Evaluator` instances, which `sua-architecture.md` §4
rejected as a language-semantics change: this is strictly smaller.

## 5. The six questions

**Scalable to 100k–millions of devices?** For the property in question, yes. Connections stay at the
event loop's cost (0.6 KB measured); threads appear only for handlers actually waiting on I/O, and
that number is bounded by a configurable cap, not by connection count. Thread stacks are virtual and
committed lazily, so a suspended handler's real cost is its interpreter stack — kilobytes. No effect
on battery: nothing about the client protocol changes.

**Maintainable, long-term support?** Yes, and this is the strongest argument for §3.3 over §3.1 and
§3.2. `std::thread` and `condition_variable` are C++11 and are not going anywhere; `ucontext` is
already removed from the standard it came from, and hand-rolled assembly is a per-architecture
commitment forever.

**Easy to use, and the Bantu way?** Yes — **no syntax changes at all**. No `async`, no `await`, no
callbacks. A handler that suspends looks exactly like one that does not:

```bantu
sua.server.get("/proxy", def($req, $res) {
    $r = sua.http.get("https://slow.example/thing");   // suspends the handler,
    $res.json($r);                                     // not the worker
});
```

That is the same promise §11 already makes, kept.

**Documentable and testable cleanly?** Yes, and unusually so: because handoff is explicit rather than
preemptive, suspension points are deterministic and tests reproduce instead of flaking. The gate is
concrete — one handler makes a 2 s outbound call while a second handler is served in the meantime;
today the second waits 2 s, after this it waits milliseconds.

**Efficient?** A condvar handoff is roughly 1–5 µs, paid only at suspension points — I/O frequency,
not per request. Against a network round trip of milliseconds that is noise. Handlers that never
suspend never pay anything, because they never leave the loop thread. A thread pool avoids repeated
thread creation.

**Secure?** Neutral on memory safety and slightly positive on DoS — the pool is capped, so a slow
backend cannot spawn unbounded threads; over the cap, handlers queue or fail fast. But there is one
genuine hazard, and it is not a memory-safety one. It is §6.

## 6. The hazard: handlers stop being atomic

Today a handler runs start-to-finish with no other Bantu code interleaved. **Suspension breaks
that**, and it can break working programs silently:

```bantu
$stock = 1;
sua.server.post("/buy", def($req, $res) {
    if ($stock > 0) {
        $ok = sua.http.post("https://payments/charge", $body);   // suspends here
        $stock = $stock - 1;                                     // another request already passed the check
        $res.json({"ok": true});
    }
});
```

Two buyers both see `$stock > 0`, both charge, and stock goes to `-1`. This is a **logical** race,
not a memory race — no sanitizer finds it, and it appears only under concurrency. It is the classic
cost of cooperative multitasking and it is exactly what Node, Python asyncio and Go all live with.

The difference is that those languages never promised atomicity. **Bantu currently does**, implicitly,
because a handler cannot be interrupted. Turning suspension on globally would quietly invalidate that
promise for every program already written against it.

## 7. The decision needed

How suspension is enabled. This changes what existing programs do, so it is not mine to pick.

| | behaviour | risk to existing apps |
|---|---|---|
| **A. Per-handler opt-in** (recommended) | `sua.server.get(path, handler, {"suspend": true})` — only marked handlers suspend | **none**; unmarked handlers stay atomic |
| **B. Global opt-in** | `sua.server.concurrency("cooperative")` — one switch, all handlers | none until enabled, then all-or-nothing |
| **C. Always on** | every handler can suspend | **silently breaks** any program relying on atomicity |

**Recommendation: A**, with B available later as a convenience. It is additive, feature-detectable
and reversible per route, which is how every other capability in this codebase has been introduced —
and it lets an app adopt suspension exactly where it has a slow outbound call, without auditing
handlers that never make one.

C should not be offered even as an option flag, because the failure mode is silent.

## 8. Staging, if approved

| step | what | gate |
|---|---|---|
| 1 | `coroutine.hpp`: the baton, a capped thread pool, save/restore of the five fields | unit tests on suspend/resume ordering |
| 2 | `sleep` suspends via a loop timer | two handlers, one sleeping, both complete concurrently |
| 3 | `sua.http.*` suspends via curl_multi fds registered with `bantu_loop::Backend` | a 2 s outbound call does not delay a second request |
| 4 | `sqlite3_step` offloaded to a worker thread (it cannot be made non-blocking) | long query does not stall the loop |
| 5 | docs: the atomicity hazard in `sua.md`, prominently | — |

Step 3 is where the real work is: curl's `curl_multi_socket_action` interface has to drive, and be
driven by, the same event loop that owns the connections. That integration is the part to get right,
and it is also the part that makes `sua.http.all` fold naturally into the same machinery.

**Estimated risk: medium.** The runtime is small and testable; the integration in step 3 touches the
loop, which is the most load-bearing code in the server. Steps 1–2 are safe to land alone and prove
the mechanism before step 3 touches anything that matters.
