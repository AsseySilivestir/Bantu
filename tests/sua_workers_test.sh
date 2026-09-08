#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════
#  sua_workers_test.sh — multi-worker mode: distribution, the cross-worker
#  broadcast bus, supervision, and the single-worker fallback.
#
#  What each assertion is actually protecting:
#
#    * connections REACH more than one worker. The first cut of this passed
#      12/12 on the broadcast test while every client sat on worker 0 and the
#      bus was never exercised -- so spread is asserted before delivery.
#    * a broadcast crosses workers. Without the bus a client only ever hears
#      from peers that happen to share its process.
#    * client ids are unique across workers. The counter is per-process, so
#      before this every worker minted "ws-1" and a targeted send could be
#      delivered to the wrong person's socket.
#    * a killed worker comes back. The supervisor exists for this.
#
#  Run:  BANTU=./bantu-src/compiler/build/bantu bash tests/sua_workers_test.sh
# ════════════════════════════════════════════════════════════════════════
set -u
BANTU="${BANTU:-bantu}"
PORT="${PORT:-39931}"
WORKERS="${WORKERS:-4}"
TMP="$(mktemp -d)"
BASE="http://127.0.0.1:$PORT"

cleanup() {
    [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
    pkill -f "$TMP/server.b" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

cat > "$TMP/server.b" <<BEOF
sua.ws.on("connect", def(\$c) {
    \$s = sua.server.stats();
    sua.ws.send(\$c.id, "WORKER:" + str(\$s.worker) + ":" + \$c.id);
});
sua.ws.on("message", def(\$m) { sua.ws.broadcast("ALL:" + \$m.data); });
sua.server.get("/who", def(\$req, \$res) {
    \$s = sua.server.stats();
    \$i = 0; \$x = 0;
    while (\$i < 3000) { \$x = \$x + \$i; \$i = \$i + 1; }
    \$res.send(str(\$s.worker));
});
sua.server.get("/stats", def(\$req, \$res) { \$res.json(sua.server.stats()); });
sua.server.workers($WORKERS);
sua.server.listen($PORT);
BEOF

"$BANTU" run "$TMP/server.b" > "$TMP/server.log" 2>&1 &
SRV=$!
for _ in $(seq 1 60); do curl -s -o /dev/null "$BASE/who" 2>/dev/null && break; sleep 0.2; done
if ! curl -s -o /dev/null "$BASE/who" 2>/dev/null; then
    echo "  FAIL  server did not start"; sed 's/^/          /' "$TMP/server.log"; exit 1
fi

PORT="$PORT" WORKERS="$WORKERS" TMP="$TMP" python3 - <<'PY'
import socket, base64, os, struct, sys, json, subprocess, time, collections
PORT=int(os.environ["PORT"]); WANT=int(os.environ["WORKERS"]); TMP=os.environ["TMP"]
P=F=0
def check(label, got, want):
    global P,F
    if str(got)==str(want): P+=1; print("  ok    %s" % label)
    else: F+=1; print("  FAIL  %s\n          got: %s | want: %s" % (label,got,want))
def check_true(label, cond, detail=""):
    global P,F
    if cond: P+=1; print("  ok    %s" % label)
    else: F+=1; print("  FAIL  %s%s" % (label, ("\n          "+detail) if detail else ""))

def ws():
    """Open a WS connection, consuming EXACTLY the handshake.

    The server's on(connect) handler sends a frame immediately, which the
    kernel happily coalesces with the 101 response -- so a single recv() here
    swallows the first data frame and every later read times out. Read to the
    header terminator and hand back whatever came after it.
    """
    s=socket.create_connection(("127.0.0.1",PORT),5); s.settimeout(5)
    k=base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n"%(PORT,k)).encode())
    buf=b""
    while b"\r\n\r\n" not in buf:
        d=s.recv(1)
        if not d: break
        buf+=d
    return s, b""

def frame(op,p):
    b=bytearray(); b.append(0x80|op); m=os.urandom(4); n=len(p)
    if n<126: b.append(0x80|n)
    else: b.append(0x80|126); b+=struct.pack("!H",n)
    b+=m; b+=bytes(p[i]^m[i%4] for i in range(n))
    return bytes(b)

def unframe(d):
    if len(d)<2: return b""
    n=d[1]&0x7F; off=2
    if n==126: n=struct.unpack("!H",d[2:4])[0]; off=4
    elif n==127: n=struct.unpack("!Q",d[2:10])[0]; off=10
    return d[off:off+n]

print("-- HTTP distribution across workers --")
import concurrent.futures as cf
def one(_):
    try:
        c=socket.create_connection(("127.0.0.1",PORT),10); c.settimeout(10)
        c.sendall(b"GET /who HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        buf=b""
        while True:
            d=c.recv(4096)
            if not d: break
            buf+=d
        c.close()
        return buf.split(b"\r\n\r\n",1)[1].decode().strip()
    except Exception as e:
        return "ERR"
with cf.ThreadPoolExecutor(32) as ex:
    got=list(ex.map(one, range(400)))
dist=collections.Counter(got)
errs=dist.pop("ERR",0)
check("400 concurrent requests all answered", errs, 0)
check_true("every reply is a valid worker index",
           all(v.isdigit() and 0<=int(v)<WANT for v in dist),
           "saw: %s" % dict(dist))
check_true("work reached every worker (%d)" % WANT, len(dist)==WANT,
           "distribution: %s" % dict(dist))
# Balance, loosely: no worker may take more than half of everything.
worst = max(dist.values()) if dist else 0
check_true("no worker took >50% of the load", worst <= 200,
           "distribution: %s" % dict(dist))

print("\n-- WebSocket clients spread across workers --")
clients=[]
homes=[]
ids=[]
for _ in range(16):
    s,rest=ws(); clients.append(s)
    try:
        if not rest: rest=s.recv(500)
        msg=unframe(rest).decode()
        _,w,cid = msg.split(":",2)
        homes.append(w); ids.append(cid)
    except Exception as e:
        homes.append("?"); ids.append("?")
spread=collections.Counter(homes)
check_true("WS clients landed on >1 worker", len(spread)>1, "homes: %s" % dict(spread))
# The bug this protects: a per-process counter made every worker mint "ws-1".
check("client ids are unique across workers", len(set(ids)), len(ids))

print("\n-- broadcast crosses the worker boundary --")
clients[0].sendall(frame(0x1,b"hello"))
time.sleep(1.5)
recv=0
for c in clients:
    try:
        c.settimeout(1.5)
        if b"ALL:hello" in c.recv(1000): recv+=1
    except Exception: pass
check("every client received the broadcast", recv, len(clients))
# And prove it genuinely crossed: the sender's own worker holds only a fraction.
sender_home_count = spread[homes[0]]
check_true("broadcast reached beyond the sender's worker",
           recv > sender_home_count,
           "sender worker held %d clients, %d received" % (sender_home_count, recv))

print("\n-- stats --")
c=socket.create_connection(("127.0.0.1",PORT),5); c.settimeout(5)
c.sendall(b"GET /stats HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
buf=b""
while True:
    d=c.recv(4096)
    if not d: break
    buf+=d
st=json.loads(buf.split(b"\r\n\r\n",1)[1].decode())
check("stats.workers", st.get("workers"), WANT)
check_true("stats.worker in range", 0 <= st.get("worker",-1) < WANT, str(st))
check("bus attached", st.get("bus"), True)
check_true("no bus drops", st.get("bus_dropped",1)==0, str(st.get("bus_dropped")))

print("\n-- supervision --")
# Find a worker pid from the supervisor's startup line and kill it.
pids=[]
for line in open(TMP+"/server.log"):
    if "workers (pids:" in line:
        pids=[int(x) for x in line.split("pids:")[1].split(")")[0].split()]
check("supervisor reported %d worker pids" % WANT, len(pids), WANT)
if pids:
    os.kill(pids[0], 9)
    time.sleep(2.0)
    # The service must still answer, and the supervisor must have replaced it.
    ok=0
    for _ in range(20):
        try:
            c=socket.create_connection(("127.0.0.1",PORT),5); c.settimeout(5)
            c.sendall(b"GET /who HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            if b"200" in c.recv(200): ok+=1
            c.close()
        except Exception: pass
    check_true("service survives a killed worker", ok>=18, "%d/20 answered" % ok)
    log=open(TMP+"/server.log").read()
    check_true("supervisor logged the restart", "restarting" in log,
               log[-300:])

print("\n========================================")
print("  PASS: %d   FAIL: %d" % (P,F))
print("========================================")
print("  RESULT: " + ("ALL GREEN" if F==0 else "FAILURES PRESENT"))
sys.exit(1 if F else 0)
PY
RC=$?

# ── single-worker mode must be untouched by any of the above ──
echo
echo "-- single-worker fallback --"
PORT2=$((PORT+1))
cat > "$TMP/single.b" <<BEOF
sua.server.get("/stats", def(\$req, \$res) { \$res.json(sua.server.stats()); });
sua.server.listen($PORT2);
BEOF
"$BANTU" run "$TMP/single.b" > "$TMP/single.log" 2>&1 &
SRV2=$!
for _ in $(seq 1 40); do curl -s -o /dev/null "http://127.0.0.1:$PORT2/stats" 2>/dev/null && break; sleep 0.2; done
OUT="$(curl -s --max-time 5 "http://127.0.0.1:$PORT2/stats")"
kill $SRV2 2>/dev/null
if echo "$OUT" | grep -q '"workers":1' && echo "$OUT" | grep -q '"bus":false'; then
    echo "  ok    single worker: no bus, workers=1"
else
    echo "  FAIL  single worker reported: $OUT"; RC=1
fi

exit $RC
