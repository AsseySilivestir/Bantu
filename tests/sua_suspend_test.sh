#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════
#  sua_suspend_test.sh — opt-in suspending handlers.
#
#  A sua worker is one event loop on one thread, so a handler that waits on
#  anything used to make every other connection on that worker wait too. A
#  route marked {"suspend": true} now hands the worker back while it waits.
#
#  Four claims, and the first two are a matched pair -- either alone would
#  pass for the wrong reason:
#
#    1. a marked route does NOT block the worker    (suspension works)
#    2. an unmarked route STILL DOES                (opt-in is real, and the
#       atomicity existing programs rely on is intact -- docs/sua-async-design
#       §6. Without this the test would pass just as well if suspension had
#       been turned on globally, which is the outcome that silently breaks
#       working programs.)
#    3. locals survive a suspension                 (the interpreter's scope
#       chain is saved and restored: ten handlers suspend interleaved and each
#       must come back with its OWN parameter, not another's)
#    4. a handler can call its own server           (only possible if the
#       worker is genuinely free while the outbound call is in flight; without
#       suspension this self-request deadlocks until curl times out)
#    5. a client that disconnects mid-suspension     (the fd number can be
#       reissued to the next connection before the handler resumes, so the
#       late response must be dropped rather than written to a stranger)
#    6. a full pool falls back to running inline     (over max_suspended_handlers
#       every request is still served -- the cap bounds memory, it never fails
#       a request)
#
#  Run:  BANTU=./bantu-src/compiler/build/bantu bash tests/sua_suspend_test.sh
# ════════════════════════════════════════════════════════════════════════
set -u
BANTU="${BANTU:-bantu}"
PORT="${PORT:-39951}"
PORT2=$((PORT + 1))
TMP="$(mktemp -d)"

cleanup() {
    [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
    [ -n "${CAP:-}" ] && kill "$CAP" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

cat > "$TMP/srv.b" <<BEOF
// Marked: may suspend.
sua.server.get("/slow", def(\$req, \$res) {
    sleep(1200);
    \$res.json({"who": "slow"});
}, {"suspend": true});

// NOT marked: must still hold the worker, exactly as before.
sua.server.get("/blocking", def(\$req, \$res) {
    sleep(1200);
    \$res.json({"who": "blocking"});
});

sua.server.get("/fast", def(\$req, \$res) {
    \$res.json({"who": "fast"});
});

// Locals across a suspension: \$n is a parameter, and the reply has to carry
// the caller's own value back however many other handlers suspended in
// between. Doubling it proves the value was still usable after resuming and
// not merely echoed from somewhere.
sua.server.get("/echo/:n", def(\$req, \$res) {
    \$n = num(\$req.params.n);
    sleep(300);
    \$res.json({"n": \$n, "double": \$n * 2});
}, {"suspend": true});

// A request from a handler back into the same worker. With the worker blocked
// this cannot complete; it completes only because the handler suspends.
sua.server.get("/self", def(\$req, \$res) {
    \$r = sua.http.get("http://127.0.0.1:$PORT/fast");
    \$res.json({"inner": \$r.body, "status": \$r.status});
}, {"suspend": true});

sua.server.listen($PORT);
BEOF

// A second server whose pool holds two handlers, for the cap test below.
cat > "$TMP/cap.b" <<BEOF
sua.server.limits({"max_suspended_handlers": 2});
sua.server.get("/echo/:n", def(\$req, \$res) {
    \$n = num(\$req.params.n);
    sleep(300);
    \$res.json({"n": \$n, "suspended": sua.server.stats().suspended,
               "cap": sua.server.stats().max_suspended});
}, {"suspend": true});
sua.server.listen($PORT2);
BEOF

"$BANTU" run "$TMP/srv.b" > "$TMP/srv.log" 2>&1 &
SRV=$!
"$BANTU" run "$TMP/cap.b" > "$TMP/cap.log" 2>&1 &
CAP=$!

for _ in $(seq 1 60); do
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/fast" && break
    sleep 0.2
done
if ! curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/fast"; then
    echo "  FAIL  server did not start"; cat "$TMP/srv.log"; exit 1
fi

for _ in $(seq 1 60); do
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT2/echo/1" && break
    sleep 0.2
done

PORT="$PORT" PORT2="$PORT2" python3 - <<'PY'
import json, os, socket, time, urllib.request
import concurrent.futures as cf

PORT = int(os.environ["PORT"])
PORT2 = int(os.environ["PORT2"])
BASE = "http://127.0.0.1:%d" % PORT
CAPBASE = "http://127.0.0.1:%d" % PORT2
R = {"pass": 0, "fail": 0}

def ok(cond, label):
    if cond: R["pass"] += 1; print("  ok    " + label)
    else:    R["fail"] += 1; print("  FAIL  " + label)

def get(path, timeout=20, base=None):
    with urllib.request.urlopen((base or BASE) + path, timeout=timeout) as r:
        return r.read().decode()

def timed(path):
    t = time.time()
    body = get(path)
    return (time.time() - t) * 1000.0, body

# ── 1. a marked route yields the worker ──────────────────────────────────
print("-- a suspended handler does not hold the worker --")
with cf.ThreadPoolExecutor(2) as ex:
    slow = ex.submit(timed, "/slow")
    time.sleep(0.25)                       # let /slow get in and suspend
    fast_ms, fast_body = timed("/fast")
    slow_ms, slow_body = slow.result()
print("        /fast answered in %dms while /slow (1200ms) was in flight" % fast_ms)
ok(fast_ms < 600, "/fast was served while a suspended handler waited")
ok("fast" in fast_body, "/fast returned its own body")
ok(slow_ms >= 1100, "/slow still took its full 1200ms")
ok("slow" in slow_body, "/slow returned its own body")

# ── 2. an unmarked route still blocks: opt-in is real ────────────────────
print("")
print("-- an unmarked handler still holds the worker --")
with cf.ThreadPoolExecutor(2) as ex:
    blk = ex.submit(timed, "/blocking")
    time.sleep(0.25)
    fast2_ms, _ = timed("/fast")
    blk.result()
print("        /fast answered in %dms while /blocking (1200ms) ran" % fast2_ms)
ok(fast2_ms > 600, "/fast waited for the unmarked handler, as it always has")

# ── 3. locals survive suspension ─────────────────────────────────────────
print("")
print("-- the scope chain is restored on resume --")
t = time.time()
with cf.ThreadPoolExecutor(10) as ex:
    got = list(ex.map(lambda n: (n, get("/echo/%d" % n)), range(1, 11)))
ten_ms = (time.time() - t) * 1000.0
print("        ten 300ms handlers took %dms (serialised would be ~3000)" % ten_ms)
ok(ten_ms < 1500, "ten suspended handlers overlapped instead of queueing")
bad = []
for n, body in got:
    d = json.loads(body)
    if d.get("n") != n or d.get("double") != n * 2:
        bad.append((n, body))
ok(not bad, "ten interleaved suspensions each kept their own locals"
            + (" -- got %r" % bad[:3] if bad else ""))

# ── 4. a handler can reach its own worker ────────────────────────────────
print("")
print("-- a handler can call back into its own server --")
try:
    t = time.time()
    body = get("/self", timeout=15)
    ms = (time.time() - t) * 1000.0
    d = json.loads(body)
    ok(d.get("status") == 200, "the self-request completed (%dms)" % ms)
    ok("fast" in d.get("inner", ""), "and returned the inner handler's body")
except Exception as e:
    ok(False, "self-request failed: %r" % e)
    ok(False, "self-request body")

# ── 5. the client vanishes while the handler is suspended ────────────────
print("")
print("-- a client that disconnects mid-suspension --")
# Ask for the 1.2s route, then hang up immediately. The handler is still
# suspended; when it resumes, its connection is gone and the fd number may
# already belong to somebody else.
for _ in range(5):
    sk = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    sk.sendall(b"GET /slow HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    sk.close()
time.sleep(0.3)
# Meanwhile the server must keep answering, and the replies must be OUR
# replies -- a response written to a reissued fd would surface here.
mid = [get("/echo/%d" % n) for n in (41, 42)]
ok(all(json.loads(b)["n"] == n for b, n in zip(mid, (41, 42))),
   "the server kept serving correct replies while orphaned handlers ran")
time.sleep(1.4)                            # let the orphans resume and give up
ok("fast" in get("/fast"), "the server is healthy after the orphans resumed")

# ── 6. a full pool degrades to inline, never to an error ─────────────────
print("")
print("-- over max_suspended_handlers, handlers run inline --")
t = time.time()
with cf.ThreadPoolExecutor(6) as ex:
    capped = list(ex.map(lambda n: (n, get("/echo/%d" % n, base=CAPBASE)), range(1, 7)))
cap_ms = (time.time() - t) * 1000.0
parsed = [(n, json.loads(b)) for n, b in capped]
print("        six 300ms handlers, pool of 2: %dms" % cap_ms)
ok(all(d["n"] == n for n, d in parsed), "all six requests were served, none refused")
ok(all(d["cap"] == 2 for _, d in parsed), "the cap was actually 2")
# Two at a time suspend, the rest run inline: strictly slower than an
# unbounded pool (310ms) and strictly faster than fully serialised (1800ms).
ok(cap_ms > 400, "a pool of 2 did not overlap all six")

print("")
print("  %d passed, %d failed" % (R["pass"], R["fail"]))
raise SystemExit(1 if R["fail"] else 0)
PY
rc=$?

if [ $rc -ne 0 ]; then echo "── server log ──"; cat "$TMP/srv.log"; echo "── cap log ──"; cat "$TMP/cap.log"; fi
exit $rc
