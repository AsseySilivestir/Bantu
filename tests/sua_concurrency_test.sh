#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════
#  sua_concurrency_test.sh — the regression gate for sua's server
#  concurrency model. See docs/sua-architecture.md §2 and §10.
#
#  WHAT THIS CATCHES
#  Upstream's accept loop runs every connection on its own detached thread,
#  but the interpreter has ONE shared env_ that bantuCallFunction mutates.
#  Concurrent requests tore each other's scope out from under them. The
#  failure was SILENT: the server returned confidently wrong answers rather
#  than crashing, so only a value-checking test finds it.
#
#  Each request computes a value only it can know (n * ITERATIONS, built by
#  a loop so the interpreter really does the work in a local scope) and
#  returns both what it wanted and what it got. Any mismatch, any missing
#  reply, or any handler error means the isolation is broken.
#
#  Before the fix: 17/20 corrupted, 16 handler errors.
#  Required now:   0 and 0.
#
#  Run:  bash tests/sua_concurrency_test.sh
#        BANTU=./bantu-src/compiler/build/bantu bash tests/sua_concurrency_test.sh
#
#  KNOWN INTERMITTENT, recorded rather than papered over: this suite failed
#  once in roughly eight runs when executed immediately after the whole rest of
#  the test batch, and passed every time in isolation. It was NOT reproducible
#  in eight further attempts. Ruled out: leftover server processes (zero after
#  the workers suite) and ephemeral port exhaustion (TIME_WAIT peaked at 529
#  against a 16,384-port range). Cause still unknown.
#
#  Deliberately NOT retried in CI and NOT loosened: this suite is the gate on a
#  data race that was real and silent, so an intermittent failure here is
#  exactly the signal that must not be hidden. ci.yml echoes each failing
#  assertion as an annotation, so the next occurrence will name itself.
#        CONC=200 bash tests/sua_concurrency_test.sh
# ════════════════════════════════════════════════════════════════════════
set -u

BANTU="${BANTU:-bantu}"
CONC="${CONC:-40}"
ITER="${ITER:-400}"
PORT="${PORT:-39901}"
TMP="$(mktemp -d)"
BASE="http://127.0.0.1:$PORT"
PASS=0; FAIL=0

ok()  { PASS=$((PASS+1)); echo "  ok    $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL  $1"; [ $# -gt 1 ] && echo "          $2"; }

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

cat > "$TMP/server.b" <<BANTU_EOF
// Each request accumulates a value in its OWN scope. If another request's
// scope leaks in, \$acc or \$n goes missing and the reply mismatches.
sua.server.get("/work/:n", def(\$req, \$res) {
    \$n = num(\$req.params["n"]);
    \$acc = 0;
    \$i = 0;
    while (\$i < $ITER) { \$acc = \$acc + \$n; \$i = \$i + 1; }
    \$res.json({"n": \$n, "want": \$n * $ITER, "got": \$acc});
});
sua.server.get("/ping", def(\$req, \$res) { \$res.json({"ok": true}); });
sua.server.listen($PORT);
BANTU_EOF

"$BANTU" run "$TMP/server.b" > "$TMP/server.log" 2>&1 &
SRV=$!

for _ in $(seq 1 60); do
    curl -s -o /dev/null "$BASE/ping" 2>/dev/null && break
    sleep 0.2
done
if ! curl -s -o /dev/null "$BASE/ping" 2>/dev/null; then
    echo "  FAIL  server did not start"; sed 's/^/          /' "$TMP/server.log"; exit 1
fi

echo
echo "-- $CONC concurrent requests, $ITER interpreter iterations each --"

# xargs -P, not shell job control: `wait` would also block on the server.
seq 1 "$CONC" | xargs -P "$CONC" -I{} \
    curl -s --max-time 60 "$BASE/work/{}" -o "$TMP/reply_{}.json"

MISSING=0; MISMATCH=0; UNPARSED=0
for i in $(seq 1 "$CONC"); do
    f="$TMP/reply_$i.json"
    if [ ! -s "$f" ]; then MISSING=$((MISSING+1)); continue; fi
    read -r n want got < <(python3 - "$f" <<'PY'
import json,sys
try:
    d=json.load(open(sys.argv[1]))
    print(d.get("n"), d.get("want"), d.get("got"))
except Exception:
    print("ERR","ERR","ERR")
PY
)
    if [ "$want" = "ERR" ]; then UNPARSED=$((UNPARSED+1)); continue; fi
    if [ "$n" != "$i" ] || [ "$want" != "$got" ]; then
        MISMATCH=$((MISMATCH+1))
        [ "$MISMATCH" -le 3 ] && echo "          req $i -> n=$n want=$want got=$got"
    fi
done

[ "$MISSING"  -eq 0 ] && ok "every request got a reply ($CONC/$CONC)" \
                      || bad "requests with no reply" "$MISSING of $CONC"
[ "$UNPARSED" -eq 0 ] && ok "every reply is valid JSON" \
                      || bad "replies that did not parse" "$UNPARSED of $CONC"
[ "$MISMATCH" -eq 0 ] && ok "no request saw another request's scope" \
                      || bad "corrupted replies" "$MISMATCH of $CONC — the interpreter raced"

# grep -c prints 0 AND exits 1 when there are no matches, so `|| echo 0` would
# append a second line and break the integer test below.
HERR=$(grep -c 'Handler error' "$TMP/server.log" 2>/dev/null; true)
HERR=${HERR:-0}
[ "$HERR" -eq 0 ] && ok "no handler errors" \
                  || bad "handler errors in the server log" "$HERR (e.g. $(grep -m1 'Handler error' "$TMP/server.log"))"

echo
echo "========================================"
echo "  PASS: $PASS   FAIL: $FAIL"
echo "========================================"
if [ "$FAIL" -gt 0 ]; then echo "  RESULT: FAILURES PRESENT"; exit 1; fi
echo "  RESULT: ALL GREEN"
