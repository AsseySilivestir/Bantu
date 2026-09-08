#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════
#  sua_ws_security_test.sh — RFC 6455 conformance and the security controls
#  from docs/sua-architecture.md §9.
#
#  Each assertion here corresponds to a hole that was open:
#    * no Origin check          -> Cross-Site WebSocket Hijacking
#    * masking read, not checked -> cache poisoning via intermediaries
#    * no frame size cap         -> memory exhaustion
#    * single-recv frame parsing -> frames silently truncated at one TCP
#                                   segment (voice data corrupted constantly)
#    * 16KB single-recv headers  -> truncated header blocks
#    * unhandled SIGPIPE         -> one abrupt disconnect killed the server
#
#  Run:  BANTU=./bantu-src/compiler/build/bantu bash tests/sua_ws_security_test.sh
# ════════════════════════════════════════════════════════════════════════
set -u
BANTU="${BANTU:-bantu}"
PORT="${PORT:-39951}"
TMP="$(mktemp -d)"
BASE="http://127.0.0.1:$PORT"

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

cat > "$TMP/server.b" <<BEOF
sua.ws.on("message", def(\$m) { sua.ws.send(\$m.client, "len:" + str(len(\$m.data))); });
sua.server.get("/ping", def(\$req, \$res) { \$res.json({"ok": true}); });
sua.server.listen($PORT);
BEOF

"$BANTU" run "$TMP/server.b" > "$TMP/server.log" 2>&1 &
SRV=$!
for _ in $(seq 1 60); do curl -s -o /dev/null "$BASE/ping" 2>/dev/null && break; sleep 0.2; done
if ! curl -s -o /dev/null "$BASE/ping" 2>/dev/null; then
    echo "  FAIL  server did not start"; sed 's/^/          /' "$TMP/server.log"; exit 1
fi

PORT="$PORT" python3 - <<'PY'
import socket, base64, os, struct, sys, json
PORT=int(os.environ["PORT"])
P=F=0
def check(label, got, want):
    global P,F
    if str(got)==str(want): P+=1; print("  ok    %s" % label)
    else: F+=1; print("  FAIL  %s\n          got: %s | want: %s" % (label,got,want))

def ws():
    s=socket.create_connection(("127.0.0.1",PORT),5); s.settimeout(6)
    k=base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n"%(PORT,k)).encode())
    s.recv(1024); return s

def frame(op,payload,mask=True,fin=True):
    b=bytearray(); b.append((0x80 if fin else 0)|op)
    n=len(payload); m=0x80 if mask else 0
    if n<126: b.append(m|n)
    elif n<65536: b.append(m|126); b+=struct.pack("!H",n)
    else: b.append(m|127); b+=struct.pack("!Q",n)
    if mask:
        mk=os.urandom(4); b+=mk; b+=bytes(payload[i]^mk[i%4] for i in range(n))
    else: b+=payload
    return bytes(b)

def code(s):
    try:
        d=s.recv(64)
        if len(d)>=4 and (d[0]&0x0F)==0x8: return struct.unpack("!H",d[2:4])[0]
        return "no-close"
    except Exception: return "reset"

def run(fn, default="exception"):
    try: return fn()
    except Exception as e: return "%s:%s" % (default, type(e).__name__)

# ── the upgrade handshake ──
def upgrade(extra):
    s=socket.create_connection(("127.0.0.1",PORT),5); s.settimeout(5)
    k=base64.b64encode(os.urandom(16)).decode()
    req=("GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
         "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"%(PORT,k))
    if extra: req+=extra+"\r\n"
    req+="\r\n"
    s.sendall(req.encode())
    line=s.recv(200).split(b"\r\n")[0].decode(); s.close()
    return line.split(" ")[1]

print("\n-- Cross-Site WebSocket Hijacking (Origin) --")
check("same-origin upgrade accepted",  run(lambda: upgrade("Origin: http://127.0.0.1:%d"%PORT)), "101")
check("hostile origin rejected",       run(lambda: upgrade("Origin: https://evil.example.com")), "403")
check("no Origin (native client) ok",  run(lambda: upgrade(None)), "101")

print("\n-- RFC 6455 frame validation --")
def t(op,payload,**kw):
    s=ws(); s.sendall(frame(op,payload,**kw)); r=code(s); s.close(); return r
check("unmasked client frame -> 1002", run(lambda: t(0x1,b"hello",mask=False)), 1002)
check("invalid UTF-8 text -> 1007",    run(lambda: t(0x1,b"\xff\xfe")), 1007)
check("control frame >125 -> 1002",    run(lambda: t(0x9,b"P"*200)), 1002)
def t_rsv():
    s=ws(); f=bytearray(frame(0x1,b"hi")); f[0]|=0x40; s.sendall(bytes(f)); r=code(s); s.close(); return r
check("RSV bit set -> 1002",           run(t_rsv), 1002)
def t_over():
    s=ws(); h=bytearray([0x81,0xFF]); h+=struct.pack("!Q",64*1024*1024); h+=os.urandom(4)
    s.sendall(bytes(h)); r=code(s); s.close(); return r
check("oversized frame -> 1009",       run(t_over), 1009)
def t_stray():
    s=ws(); s.sendall(frame(0x0,b"orphan")); r=code(s); s.close(); return r
check("stray continuation -> 1002",    run(t_stray), 1002)

print("\n-- framing correctness (whole-frame reads) --")
def echo(payload, frames=None):
    s=ws()
    for f in (frames or [frame(0x1,payload)]): s.sendall(f)
    d=s.recv(300); p=d[2:] if d[1]<126 else d[4:]; s.close()
    return p.decode(errors="replace")
check("200KB message not truncated", run(lambda: echo(b"A"*200000)), "len:200000")
check("fragmented message reassembled",
      run(lambda: echo(None, [frame(0x1,b"part-one ",fin=False), frame(0x0,b"part-two")])), "len:17")

print("\n-- HTTP header block --")
def hdr(send_fn):
    s=socket.create_connection(("127.0.0.1",PORT),5); s.settimeout(5)
    send_fn(s); line=s.recv(200).split(b"\r\n")[0].decode(); s.close()
    return line.split(" ")[1]
def big(s):
    s.sendall(b"GET /ping HTTP/1.1\r\nHost: x\r\n"); s.sendall(b"X-Pad: "+b"A"*100000+b"\r\n\r\n")
def split(s):
    import time
    s.sendall(b"GET /ping HTTP/1.1\r\n"); s.sendall(b"Host: x\r\n"); time.sleep(0.3)
    s.sendall(b"X-Late: yes\r\n\r\n")
check("oversized header block -> 431", run(lambda: hdr(big)), "431")
check("header split across segments",  run(lambda: hdr(split)), "200")

# ── abrupt disconnect (SIGPIPE remote kill) ──────────────────────────
# A client that requests a body and closes without reading it makes the
# server's next send() raise SIGPIPE. The default action terminates the
# process, so ONE unauthenticated request killed the whole server. This
# predates the event loop: v1.3.0 dies to it identically, exit 141.
print("\n-- abrupt disconnect --")
def slam(n):
    for _ in range(n):
        c=socket.socket(); c.connect(("127.0.0.1",PORT))
        c.sendall(b"GET /ping HTTP/1.1\r\nHost: x\r\n\r\n")
        # SO_LINGER 0 => RST rather than a graceful FIN, the hostile case.
        c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii",1,0))
        c.close()
    return "sent"
check("200 abrupt disconnects accepted", run(lambda: slam(200)), "sent")
# The real assertion: the server is still alive and serving afterwards.
def alive():
    c=socket.create_connection(("127.0.0.1",PORT),5); c.settimeout(5)
    c.sendall(b"GET /ping HTTP/1.1\r\nHost: x\r\n\r\n")
    line=c.recv(200).split(b"\r\n")[0].decode(); c.close()
    return line.split(" ")[1]
check("server survives abrupt disconnects", run(alive), "200")

print("\n========================================")
print("  PASS: %d   FAIL: %d" % (P,F))
print("========================================")
print("  RESULT: " + ("ALL GREEN" if F==0 else "FAILURES PRESENT"))
sys.exit(1 if F else 0)
PY
