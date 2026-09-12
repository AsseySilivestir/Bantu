#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════
#  sua_memory_test.sh — the server's memory must not grow with traffic.
#
#  This exists because it did. $res methods return $res so that calls chain,
#  and each method captured the response object by shared_ptr -- while being
#  stored INSIDE that same object. A reference cycle: the refcount never
#  reached zero, so every request leaked its whole $res graph (the map, six
#  closures, the response state, every string in it).
#
#  Measured before the fix, single worker, one route:
#
#      10,000 requests  ->  RSS 9 MB  -> 65 MB
#      20,000 requests  ->             123 MB
#      30,000 requests  ->             180 MB
#      40,000 requests  ->             238 MB
#
#  Perfectly linear, ~2-3 KB per request, and `heap` confirmed exactly one
#  orphaned BantuHttpResponseState per request. At the ~8,800 req/s this
#  server sustains that is 27 MB/s: a long-running sua server would be killed
#  by the OOM reaper, and the symptom -- a server that dies after a few hours
#  under load -- gives no hint of the cause. It affected every release that
#  shipped sua.
#
#  A leak has no failing assertion of its own, so this measures RSS directly.
#  Warm-up first: allocators and the interpreter settle in the first few
#  hundred requests, and counting that as growth would make the test flaky.
#
#  Run:  BANTU=./bantu-src/compiler/build/bantu bash tests/sua_memory_test.sh
# ════════════════════════════════════════════════════════════════════════
set -u
BANTU="${BANTU:-bantu}"
PORT="${PORT:-39961}"
TMP="$(mktemp -d)"
PASS=0; FAIL=0

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

ok() { if [ "$1" = "1" ]; then PASS=$((PASS+1)); echo "  ok    $2"; else FAIL=$((FAIL+1)); echo "  FAIL  $2"; fi }

cat > "$TMP/srv.b" <<BEOF
// Exercises the chaining path, which is the one that built the cycle.
sua.server.get("/", def(\$req, \$res) {
    \$res.status(200).set("X-T", "1").json({"ok": true, "n": 1});
});
// Same, on a route that suspends: the response is built on a task thread and
// carried across a park, so it allocates strictly more per request.
sua.server.get("/s", def(\$req, \$res) {
    sleep(1);
    \$res.json({"ok": true});
}, {"suspend": true});
sua.server.listen($PORT);
BEOF

"$BANTU" run "$TMP/srv.b" > "$TMP/srv.log" 2>&1 &
SRV=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/" && break
    sleep 0.2
done
if ! curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/"; then
    echo "  FAIL  server did not start"; cat "$TMP/srv.log"; exit 1
fi

rss() { ps -o rss= -p "$SRV" | tr -d ' '; }

hammer() { # hammer <path> <count> <threads>
    PORT="$PORT" python3 - "$1" "$2" "$3" <<'PY'
import socket, sys, concurrent.futures as cf, os
port=int(os.environ["PORT"]); path=sys.argv[1]; total=int(sys.argv[2]); threads=int(sys.argv[3])
req=("GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" % path).encode()
def worker(n):
    bad=0
    for _ in range(n):
        try:
            s=socket.create_connection(("127.0.0.1",port), timeout=10)
            s.sendall(req)
            buf=b""
            while True:
                d=s.recv(65536)
                if not d: break
                buf+=d
            s.close()
            if b'"ok":true' not in buf: bad+=1
        except Exception: bad+=1
    return bad
per=total//threads
with cf.ThreadPoolExecutor(threads) as ex: bad=sum(ex.map(worker,[per]*threads))
print(bad)
PY
}

echo "-- plain requests --"
warm=$(hammer / 1000 8)                 # settle the allocator
before=$(rss)
bad=$(hammer / 10000 16)
sleep 2
after=$(rss)
growth=$(( after - before ))
echo "        RSS ${before}KB -> ${after}KB over 10,000 requests (${growth}KB)"
ok "$([ "$bad" = "0" ] && echo 1 || echo 0)" "all 10,000 requests answered correctly ($bad bad)"
# The leak was +20,000KB per 10,000 requests. Fixed it is a few hundred KB of
# allocator noise. 8MB separates the two by a wide margin in both directions.
ok "$([ "$growth" -lt 8000 ] && echo 1 || echo 0)" "RSS grew less than 8MB over 10,000 requests"

echo ""
echo "-- the same, repeated: a leak is linear, noise is not --"
mid=$(rss)
bad2=$(hammer / 10000 16)
sleep 2
end=$(rss)
growth2=$(( end - mid ))
echo "        RSS ${mid}KB -> ${end}KB over another 10,000 (${growth2}KB)"
ok "$([ "$bad2" = "0" ] && echo 1 || echo 0)" "all of the second 10,000 answered correctly ($bad2 bad)"
ok "$([ "$growth2" -lt 8000 ] && echo 1 || echo 0)" "the second round did not grow either"

echo ""
echo "-- suspended handlers --"
sbefore=$(rss)
bad3=$(hammer /s 3000 32)
sleep 2
safter=$(rss)
growth3=$(( safter - sbefore ))
echo "        RSS ${sbefore}KB -> ${safter}KB over 3,000 suspended requests (${growth3}KB)"
ok "$([ "$bad3" = "0" ] && echo 1 || echo 0)" "all 3,000 suspended requests answered correctly ($bad3 bad)"
# Thread stacks are committed lazily and up to max_suspended_handlers threads
# may be created, so a larger allowance here -- but it must still be bounded.
ok "$([ "$growth3" -lt 12000 ] && echo 1 || echo 0)" "suspended handlers released their memory too"

echo ""
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || { echo "── server log ──"; tail -20 "$TMP/srv.log"; }
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
