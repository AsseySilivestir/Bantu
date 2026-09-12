#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════
#  sua_pwa_http_test.sh — end-to-end HTTP test of the sua PWA layer.
#
#  Boots a real server and curls it. This is the only test that proves the
#  wire-level behaviour: correct content types, root service-worker scope,
#  cache headers, auto-injected meta, the subscribe endpoint, and — the one
#  that matters most — that a request body containing NUL bytes survives the
#  outbound HTTP client intact.
#
#  Usage:  bash tests/sua_pwa_http_test.sh [path/to/bantu]
# ════════════════════════════════════════════════════════════════════════

set -u
BANTU="${1:-bantu-src/compiler/build/bantu}"
PORT="${PORT:-8731}"
BASE="http://127.0.0.1:$PORT"
TMP="$(mktemp -d)"
PASS=0
FAIL=0

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    wait "${SRV_PID:-}" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

ok()   { PASS=$((PASS+1)); echo "  ok    $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL  $1"; [ $# -gt 1 ] && echo "          $2"; }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got: $2 | want: $3"; fi; }
contains() { case "$2" in *"$3"*) ok "$1" ;; *) bad "$1" "missing: $3" ;; esac; }
missing()  { case "$2" in *"$3"*) bad "$1" "unexpectedly present: $3" ;; *) ok "$1" ;; esac; }

# ── the app under test ──────────────────────────────────────────────────
mkdir -p "$TMP/public"

cat > "$TMP/public/index.html" <<'HTML'
<!doctype html>
<html><head><meta charset="utf-8"><title>Test App</title></head>
<body><h1>hello</h1></body></html>
HTML

cat > "$TMP/public/offline.html" <<'HTML'
<!doctype html>
<html><head><meta charset="utf-8"><title>Offline</title></head>
<body><p>custom offline page</p></body></html>
HTML

printf 'body { color: red }' > "$TMP/public/app.css"

cat > "$TMP/server.b" <<BANTU
sua.pwa.configure({
    "name": "HTTP Test App",
    "short_name": "HTTPTest",
    "theme_color": "#112233",
    "icons": [{"src": "/icon.png", "sizes": "192x192", "type": "image/png"}],
    "precache": ["/", "/app.css"]
});

\$keys = sua.push.vapid_keys();
sua.push.configure({
    "public_key": \$keys.public_key,
    "private_key": \$keys.private_key,
    "subject": "mailto:test@example.com",
    "db": "$TMP/subs.db"
});

// Echo back exactly how many body bytes arrived — used to prove that a binary
// body is not truncated at its first NUL.
sua.server.post("/echo-len", def(\$req, \$res) {
    \$res.json({"len": len(\$req.body)});
});

// Proves arbitrary request headers reach the wire.
sua.server.post("/echo-header", def(\$req, \$res) {
    \$res.json({"probe": \$req.headers["x-probe"]});
});

sua.server.get("/api/ping", def(\$req, \$res) { \$res.json({"pong": true}); });

sua.server.static("$TMP/public");
sua.server.listen($PORT);
BANTU

"$BANTU" run "$TMP/server.b" > "$TMP/server.log" 2>&1 &
SRV_PID=$!

# Wait for the port
for _ in $(seq 1 60); do
    if curl -s -o /dev/null "$BASE/api/ping" 2>/dev/null; then break; fi
    sleep 0.2
done
if ! curl -s -o /dev/null "$BASE/api/ping" 2>/dev/null; then
    echo "  FAIL  server did not start"
    sed 's/^/          /' "$TMP/server.log"
    exit 1
fi

echo
echo "-- manifest --"
BODY="$(curl -s "$BASE/manifest.json")"
CT="$(curl -s -o /dev/null -D - "$BASE/manifest.json" | tr -d '\r' | awk -F': ' 'tolower($1)=="content-type"{print $2}')"
check "manifest content-type" "$CT" "application/manifest+json; charset=utf-8"
contains "manifest has name" "$BODY" '"name": "HTTP Test App"'
contains "manifest has short_name" "$BODY" '"short_name": "HTTPTest"'
contains "manifest has display" "$BODY" '"display": "standalone"'
contains "manifest has icons" "$BODY" '"sizes":"192x192"'
ALT="$(curl -s "$BASE/manifest.webmanifest")"
check "webmanifest alias matches" "$ALT" "$BODY"

echo
echo "-- service worker --"
HDRS="$(curl -s -o "$TMP/sw.js" -D - "$BASE/serviceworker.js" | tr -d '\r')"
SW="$(cat "$TMP/sw.js")"
contains "served as javascript" "$HDRS" "Content-Type: application/javascript"
contains "not cached" "$HDRS" "Cache-Control: no-cache"
contains "root scope allowed" "$HDRS" "Service-Worker-Allowed: /"
contains "has a fetch handler" "$SW" "addEventListener('fetch'"
contains "has a push handler" "$SW" "addEventListener('push'"
contains "has notificationclick" "$SW" "addEventListener('notificationclick'"
contains "precaches configured assets" "$SW" "/app.css"

echo
echo "-- client helper --"
JS="$(curl -s "$BASE/pwa.js")"
contains "registers the worker" "$JS" "navigator.serviceWorker.register"
contains "exposes BantuPWA" "$JS" "window.BantuPWA"
contains "carries a VAPID key" "$JS" "const VAPID    = '"
missing "no empty VAPID key" "$JS" "const VAPID    = '';"

echo
echo "-- offline page --"
OFF="$(curl -s "$BASE/offline")"
contains "prefers the app's offline.html" "$OFF" "custom offline page"
contains "meta injected into it" "$OFF" 'rel="manifest"'

echo
echo "-- static files --"
IDX="$(curl -s "$BASE/")"
contains "index is served" "$IDX" "<h1>hello</h1>"
contains "meta auto-injected into static html" "$IDX" 'rel="manifest"'
contains "pwa.js auto-injected" "$IDX" "/pwa.js"
contains "original head preserved" "$IDX" "<title>Test App</title>"
CSSCT="$(curl -s -o /dev/null -D - "$BASE/app.css" | tr -d '\r' | awk -F': ' 'tolower($1)=="content-type"{print $2}')"
check "css content-type" "$CSSCT" "text/css; charset=utf-8"

# The MIME table used to cover nine extensions; everything else fell through to
# application/octet-stream, which browsers refuse to execute or render.
ctype() { curl -s -o /dev/null -D - "$BASE/$1" | tr -d '\r' | awk -F': ' 'tolower($1)=="content-type"{print $2}'; }
printf 'x' > "$TMP/public/f.woff2"
printf 'x' > "$TMP/public/m.wasm"
printf 'x' > "$TMP/public/i.webp"
printf 'x' > "$TMP/public/UPPER.PNG"
printf 'x' > "$TMP/public/v.mp4"
printf 'x' > "$TMP/public/README"
check "woff2 content-type"      "$(ctype f.woff2)"  "font/woff2"
check "wasm content-type"       "$(ctype m.wasm)"   "application/wasm"
check "webp content-type"       "$(ctype i.webp)"   "image/webp"
check "mp4 content-type"        "$(ctype v.mp4)"    "video/mp4"
check "uppercase .PNG resolves" "$(ctype UPPER.PNG)" "image/png"
check "no extension is opaque"  "$(ctype README)"   "application/octet-stream"

cachectl() { curl -s -o /dev/null -D - "$BASE/$1" | tr -d '\r' | awk -F': ' 'tolower($1)=="cache-control"{print $2}'; }
contains "manifest is never cached"  "$(cachectl manifest.webmanifest)" "no-cache"
contains "html revalidates under pwa" "$(cachectl '')" "no-cache"
contains "other assets stay cacheable" "$(cachectl app.css)" "max-age"

echo
echo "-- binary-safe outbound HTTP (the POSTFIELDSIZE regression) --"
# A 300-byte body starting with 0x00. Before CURLOPT_POSTFIELDSIZE was set,
# libcurl called strlen() on the buffer and sent nothing at all.
#
# The client runs in its own process: the server's accept loop is
# single-threaded, so a handler that calls its own server would deadlock.
cat > "$TMP/client.b" <<BANTU
\$payload = [];
\$i = 0;
while (\$i < 300) {
    \$payload.push(\$i % 256);
    \$i = \$i + 1;
}
\$r = sua.http.request({
    "method": "POST",
    "url": "$BASE/echo-len",
    "headers": {"Content-Type": "application/octet-stream"},
    "body": \$payload
});
print("SENT=" + str(len(\$payload)));
print("ECHO=" + str(\$r.body));
print("STATUS=" + str(\$r.status));
BANTU
BIN="$("$BANTU" run "$TMP/client.b" 2>&1)"
contains "client built a 300-byte body" "$BIN" "SENT=300"
contains "request succeeded" "$BIN" "STATUS=200"
contains "server received all 300 bytes" "$BIN" '"len":300'
missing "body was NOT truncated at the first NUL" "$BIN" '"len":0'

# A string body containing an embedded NUL must survive too.
cat > "$TMP/client2.b" <<BANTU
\$body = frombytes([65, 0, 66, 0, 67]);
\$r = sua.http.request({
    "method": "POST",
    "url": "$BASE/echo-len",
    "headers": {"Content-Type": "application/octet-stream"},
    "body": \$body
});
print("ECHO=" + str(\$r.body));
BANTU
BIN2="$("$BANTU" run "$TMP/client2.b" 2>&1)"
contains "a string body with embedded NULs is intact" "$BIN2" '"len":5'

echo
echo "-- custom request headers reach the wire --"
HDR="$(curl -s -X POST -H 'X-Probe: yes' -H 'Content-Type: application/json' -d '{}' "$BASE/echo-header")"
contains "header arrives" "$HDR" '"probe":"yes"'

echo
echo "-- push subscribe endpoint --"
SUBJSON='{"subscription":{"endpoint":"https://push.example.com/http-test","keys":{"p256dh":"BAAAAAAA","auth":"AAAA"}},"tag":"t"}'
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d "$SUBJSON" "$BASE$([ -n "" ] || echo /pwa/subscribe)")"
check "an invalid p256dh is rejected" "$CODE" "400"

echo
echo "-- 404 still works --"
CODE="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/nope")"
check "unknown path is 404" "$CODE" "404"

echo
echo "========================================"
echo "  PASS: $PASS   FAIL: $FAIL"
echo "========================================"
if [ "$FAIL" -gt 0 ]; then echo "  RESULT: FAILURES PRESENT"; exit 1; fi
echo "  RESULT: ALL GREEN"
