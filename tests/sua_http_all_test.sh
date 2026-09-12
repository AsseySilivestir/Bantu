#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════
#  sua_http_all_test.sh — sua.http.all: many requests in one wait.
#
#  The claim being tested is specific: N requests take max(t), not sum(t),
#  and the responses come back in REQUEST order however they complete. That
#  ordering is the part most likely to break silently -- curl_multi reports
#  completions in whatever order they finish, so a naive implementation
#  returns the fast ones first and quietly mismatches every result with the
#  wrong request.
#
#  Backed by a local Python server that sleeps a requested number of
#  milliseconds, so the timing assertion is about our scheduling and not
#  about somebody's network.
#
#  Run:  BANTU=./bantu-src/compiler/build/bantu bash tests/sua_http_all_test.sh
# ════════════════════════════════════════════════════════════════════════
set -u
BANTU="${BANTU:-bantu}"
PORT="${PORT:-39941}"
TMP="$(mktemp -d)"

cleanup() { [ -n "${BACKEND:-}" ] && kill "$BACKEND" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

# ── a backend that can be slow on demand ──
PORT="$PORT" python3 - <<'PY' > "$TMP/backend.log" 2>&1 &
import http.server, socketserver, time, os, json, threading
PORT=int(os.environ["PORT"])
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _go(self):
        # /delay/<ms>  -> sleeps, then echoes the path
        parts=self.path.strip("/").split("/")
        body=b""
        n=int(self.headers.get("content-length") or 0)
        if n: body=self.rfile.read(n)
        if parts and parts[0]=="delay":
            time.sleep(int(parts[1])/1000.0)
            out=json.dumps({"path":self.path,"slept":parts[1]}).encode()
        elif parts and parts[0]=="echo":
            # report the body length, so a NUL-truncated body is visible
            out=json.dumps({"len":len(body)}).encode()
        else:
            out=json.dumps({"path":self.path}).encode()
        self.send_response(200)
        self.send_header("Content-Type","application/json")
        self.send_header("Content-Length",str(len(out)))
        self.end_headers()
        self.wfile.write(out)
    do_GET=_go
    do_POST=_go
class S(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=True
    request_queue_size=128        # absorb a burst instead of refusing it
S(("127.0.0.1",PORT),H).serve_forever()
PY
BACKEND=$!

for _ in $(seq 1 60); do curl -s -o /dev/null "http://127.0.0.1:$PORT/x" 2>/dev/null && break; sleep 0.2; done
if ! curl -s -o /dev/null "http://127.0.0.1:$PORT/x" 2>/dev/null; then
    echo "  FAIL  backend did not start"; cat "$TMP/backend.log"; exit 1
fi

PUSHDB="$TMP/push.db"
cat > "$TMP/t.b" <<BEOF
\$P = "http://127.0.0.1:$PORT";
// Counters live in an OBJECT: a bare global assigned from inside def() writes
// a local and the summary silently stays 0 -- which reported ALL GREEN with
// every assertion passing but nothing counted. Object fields mutate through
// the shared reference, which is the pattern the other suites use.
\$R = {"pass": 0, "fail": 0};
def ok(\$cond, \$label) {
    if (\$cond) { \$R.pass = \$R.pass + 1; print("  ok    " + \$label); }
    else { \$R.fail = \$R.fail + 1; print("  FAIL  " + \$label); }
}

print("-- ordering --");
// Deliberately slowest-first: if results were returned in COMPLETION order,
// this is exactly the case that would come back reversed.
\$rs = sua.http.all([
    {"url": \$P + "/delay/600"},
    {"url": \$P + "/delay/400"},
    {"url": \$P + "/delay/200"},
    {"url": \$P + "/delay/50"}
]);
ok(len(\$rs) == 4, "four responses for four requests");
ok(contains(\$rs[0].body, "600"), "result 0 matches request 0 (slowest)");
ok(contains(\$rs[1].body, "400"), "result 1 matches request 1");
ok(contains(\$rs[2].body, "200"), "result 2 matches request 2");
ok(contains(\$rs[3].body, "50"),  "result 3 matches request 3 (fastest)");
ok(\$rs[0].ok, "responses carry ok/status");
ok(\$rs[0].status == 200, "status is 200");

print("");
print("-- parallel, not sequential --");
// Measured RELATIVE to one 500ms request rather than against a fixed
// millisecond budget: the claim is "six requests cost about what one costs",
// and an absolute threshold turns machine load into a flaky failure.
\$b0 = clock();
sua.http.all([{"url": \$P + "/delay/500"}]);
\$baseline = clock() - \$b0;
\$t0 = clock();
\$rs2 = sua.http.all([
    {"url": \$P + "/delay/500"}, {"url": \$P + "/delay/500"},
    {"url": \$P + "/delay/500"}, {"url": \$P + "/delay/500"},
    {"url": \$P + "/delay/500"}, {"url": \$P + "/delay/500"}
]);
\$elapsed = clock() - \$t0;
print("        1 x 500ms = " + str(\$baseline) + "ms; 6 x 500ms = " + str(\$elapsed)
      + "ms (sequential would be ~6x)");
ok(len(\$rs2) == 6, "six responses");
ok(\$elapsed < \$baseline * 3, "six requests cost under 3x one request, not 6x");

print("");
print("-- errors keep their slot --");
\$rs3 = sua.http.all([
    {"url": \$P + "/delay/10"},
    {"nourl": true},
    {"url": \$P + "/delay/10"}
]);
ok(len(\$rs3) == 3, "a malformed element still occupies its position");
ok(\$rs3[0].ok, "element 0 succeeded");
ok(\$rs3[1].ok == false, "element 1 reported an error");
ok(\$rs3[2].ok, "element 2 succeeded and was not shifted up");

print("");
print("-- binary-safe bodies --");
// A body whose first byte is NUL: the bug that truncated push payloads.
\$bytes = [0, 1, 2, 3, 0, 255, 65, 66];
\$rs4 = sua.http.all([
    {"method": "POST", "url": \$P + "/echo", "body": \$bytes,
     "content_type": "application/octet-stream"}
]);
ok(contains(\$rs4[0].body, "\\"len\\": 8") || contains(\$rs4[0].body, "\\"len\\":8"),
   "8-byte body with leading NUL arrived whole");

print("");
print("-- push fan-out is parallel --");
// sua.push.send_all() runs on the same machinery. Six subscriptions pointed at
// a black-holed address: each connect burns the full 5s connect timeout, so
// sequential would be ~30s and parallel ~5s. This is the workload the whole
// feature exists for -- N independent HTTPS POSTs to N push services.
\$vk = sua.push.vapid_keys();
sua.push.configure({
    "public_key": \$vk.public_key,
    "private_key": \$vk.private_key,
    "subject": "mailto:test@example.com",
    "db": "$PUSHDB"
});
\$i = 0;
while (\$i < 6) {
    \$kp = webpush_keygen();
    sua.push.save({
        "endpoint": "https://10.255.255.1/push/" + str(\$i),
        "keys": {"p256dh": \$kp.public_key, "auth": b64url_encode(randbytes(16))}
    });
    \$i = \$i + 1;
}
ok(sua.push.count() == 6, "six subscriptions stored");
\$pt0 = clock();
\$pr = sua.push.send_all({"head": "hi", "body": "there"});
\$pel = clock() - \$pt0;
print("        6 unreachable endpoints took " + str(\$pel) + "ms (sequential would be ~30000ms)");
ok(len(\$pr.results) == 6, "a result per subscription");
ok(\$pr.failed == 6, "all six failed to connect, as intended");
ok(\$pel < 15000, "fan-out was parallel, not sequential");

print("");
print("-- empty and single --");
ok(len(sua.http.all([])) == 0, "empty list returns empty list");
ok(len(sua.http.all([{"url": \$P + "/delay/10"}])) == 1, "single request works");

print("");
print("========================================");
print("  PASS: " + str(\$R.pass) + "   FAIL: " + str(\$R.fail));
print("========================================");
if (\$R.fail == 0 && \$R.pass > 0) { print("  RESULT: ALL GREEN"); }
else { print("  RESULT: FAILURES PRESENT"); }
BEOF

"$BANTU" run "$TMP/t.b" 2>&1 | grep -vE '^\s*\[HTTP\]|Running:|────|Executed in'
"$BANTU" run "$TMP/t.b" 2>&1 | grep -q 'ALL GREEN'
