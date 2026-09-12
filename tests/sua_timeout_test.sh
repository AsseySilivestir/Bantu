#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════
#  sua_timeout_test.sh — idle connections are reaped, live ones are not.
#
#  Nothing covered this before, which was a gap big enough to hide a
#  catastrophic regression: a WebSocket is re-filed from the HTTP timeout
#  class to the WebSocket one when it upgrades, and if that ever stops
#  happening EVERY WebSocket silently dies after header_timeout_ms. No other
#  suite would notice -- they all finish in under a second.
#
#  It matters more now that the reaper walks a last-activity-ordered list and
#  stops at the first entry still inside its timeout, instead of scanning
#  every connection. That is what makes two million connections affordable
#  (33ms per pass at 2M became O(expired)), but it means "the walk stopped too
#  early" is a new way to be wrong, and it fails silently in the safe
#  direction: connections leak instead of erroring.
#
#  Five claims:
#    1. a half-sent request header is reaped              (Slowloris defence)
#    2. a WebSocket SURVIVES well past header_timeout_ms  (class re-filing)
#    3. an idle WebSocket is reaped at idle_timeout_ms
#    4. MANY idle connections are all reaped              (the walk does not
#       stop early and leave a backlog behind the first entry)
#    5. a suspended handler's connection is NOT reaped, even past its timeout
#
#  Run:  BANTU=./bantu-src/compiler/build/bantu bash tests/sua_timeout_test.sh
# ════════════════════════════════════════════════════════════════════════
set -u
BANTU="${BANTU:-bantu}"
PORT="${PORT:-39971}"
TMP="$(mktemp -d)"

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

cat > "$TMP/srv.b" <<BEOF
sua.server.limits({"header_timeout_ms": 1500, "idle_timeout_ms": 5000});
sua.server.get("/", def(\$req, \$res) { \$res.json({"ok": true}); });
sua.server.get("/stats", def(\$req, \$res) { \$res.json(sua.server.stats()); });
// Longer than header_timeout_ms: its connection must survive the wait.
sua.server.get("/slow", def(\$req, \$res) {
    sleep(3000);
    \$res.json({"ok": true, "slow": true});
}, {"suspend": true});
sua.ws.on("message", def(\$m) { sua.ws.send(\$m.client, "r:" + \$m.data); });
sua.server.listen($PORT);
BEOF

"$BANTU" -q run "$TMP/srv.b" > "$TMP/srv.log" 2>&1 &
SRV=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/" && break
    sleep 0.2
done
if ! curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/"; then
    echo "  FAIL  server did not start"; cat "$TMP/srv.log"; exit 1
fi

PORT="$PORT" python3 -u - <<'PY'
import base64, json, os, socket, struct, time, urllib.request
import concurrent.futures as cf

PORT = int(os.environ["PORT"])
R = {"pass": 0, "fail": 0}
def ok(c, label):
    if c: R["pass"] += 1; print("  ok    " + label)
    else: R["fail"] += 1; print("  FAIL  " + label)

def stats():
    return json.loads(urllib.request.urlopen(
        "http://127.0.0.1:%d/stats" % PORT, timeout=10).read())

def ws_open():
    s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    k = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % k).encode())
    b = b""
    while b"\r\n\r\n" not in b: b += s.recv(1)
    return s

def dead(s, wait):
    """True if the peer closed the socket within `wait` seconds."""
    s.settimeout(wait)
    try:    return s.recv(1) == b""
    except Exception: return False

# ── 1. half a request header, then silence ───────────────────────────────
print("-- a half-sent header is reaped (Slowloris) --")
slow = socket.create_connection(("127.0.0.1", PORT), timeout=5)
slow.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n")        # no terminating blank line
t = time.time()
closed = dead(slow, 6)
ok(closed, "reaped after %.1fs (header_timeout_ms = 1.5s)" % (time.time() - t))
slow.close()

# ── 2. a WebSocket must NOT be reaped at the header timeout ──────────────
print("")
print("-- a WebSocket outlives header_timeout_ms --")
w = ws_open()
time.sleep(3.0)                                        # twice the header timeout
alive = not dead(w, 0.3)
ok(alive, "still connected after 3s, twice header_timeout_ms")
mk = os.urandom(4); p = b"ping"
w.sendall(bytes([0x81, 0x80 | len(p)]) + mk + bytes(c ^ mk[i % 4] for i, c in enumerate(p)))
w.settimeout(5)
try:
    h = w.recv(2); body = w.recv(h[1] & 127)
except Exception:
    body = b""
ok(body == b"r:ping", "and still works -- got %r" % body)

# ── 3. but an idle WebSocket is eventually reaped ────────────────────────
print("")
print("-- an idle WebSocket is reaped at idle_timeout_ms --")
t = time.time()
gone = dead(w, 8)
ok(gone, "reaped after %.1fs idle (idle_timeout_ms = 5s)" % (time.time() - t))
w.close()

# ── 4. a backlog of idle connections is fully drained ────────────────────
print("")
print("-- many idle connections are ALL reaped, not just the first --")
socks = []
with cf.ThreadPoolExecutor(32) as ex:
    def mk_idle(_):
        s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n")
        return s
    socks = list(ex.map(mk_idle, range(300)))
time.sleep(0.5)
peak = stats()["live_connections"]
time.sleep(5.0)
left = stats()["live_connections"]
print("        live_connections: %d at peak -> %d after the timeout" % (peak, left))
ok(peak >= 300, "all 300 were accepted (peak %d)" % peak)
ok(left <= 5, "all but the in-flight request were reaped (%d left)" % left)
for s in socks: s.close()

# ── 5. a suspended handler is not idle ───────────────────────────────────
print("")
print("-- a suspended handler's connection survives its timeout --")
t = time.time()
try:
    body = urllib.request.urlopen("http://127.0.0.1:%d/slow" % PORT, timeout=15).read().decode()
    el = time.time() - t
    ok('"slow":true' in body, "answered after %.1fs, twice header_timeout_ms" % el)
    ok(el >= 2.9, "and really did wait (%.1fs)" % el)
except Exception as e:
    ok(False, "suspended handler was reaped mid-flight: %r" % e)
    ok(False, "suspended handler timing")

print("")
print("  %d passed, %d failed" % (R["pass"], R["fail"]))
raise SystemExit(1 if R["fail"] else 0)
PY
rc=$?
[ $rc -ne 0 ] && { echo "── server log ──"; tail -20 "$TMP/srv.log"; }
exit $rc
