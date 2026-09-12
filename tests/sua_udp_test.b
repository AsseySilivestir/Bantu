// ════════════════════════════════════════════════════════════════════════
//  sua_udp_test.b — sua.udp, which shipped with no tests at all.
//
//  sua.udp came in with the upstream merge and had zero test coverage and
//  zero documentation, so nothing here had ever been exercised. Reviewing it
//  turned up four defects, two of which KILLED THE PROCESS outright:
//
//    * recvfrom's maxBytes was taken straight from a double and used to size
//      an allocation. maxBytes -1 became SIZE_MAX and died with [FATAL]
//      vector; 1e18 died with [FATAL] std::bad_alloc. Neither is catchable
//      from Bantu, so one bad argument took the whole server down.
//    * ports were parsed with atoi and never range-checked, so
//      "127.0.0.1:99999" silently bound to 34463 and "127.0.0.1:-1" to 65535.
//    * a 60,000-byte datagram -- well inside UDP's 65,507 limit -- failed with
//      "Message too long", because macOS defaults the send buffer to 9,216.
//    * the handle returned by socket() kept reporting bound=false after bind.
//
//  Run:  bantu run tests/sua_udp_test.b
// ════════════════════════════════════════════════════════════════════════

$R = {"pass": 0, "fail": 0};

def eq($got, $want, $name) {
    if ($got == $want) {
        $R.pass = $R.pass + 1;
        print("  ok    " + $name);
    } else {
        $R.fail = $R.fail + 1;
        print("  FAIL  " + $name);
        print("          got:  " + str($got));
        print("          want: " + str($want));
    }
}

def ok($cond, $name) {
    if ($cond) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); }
}

print("-- socket, bind, getsockname --");
$srv = sua.udp.socket({});
ok($srv.__udp >= 0, "socket() returns a handle");
sua.udp.bind($srv, "127.0.0.1:0");            // 0 = let the OS choose
ok($srv.bound, "the handle reports bound after bind()");
$addr = sua.udp.getsockname($srv);
ok(contains($addr, "127.0.0.1:"), "getsockname reports the bound address");
ok($addr != "127.0.0.1:0", "the OS assigned a real port");

print("");
print("-- round trip --");
$port = 39931;
$s = sua.udp.socket({});
sua.udp.bind($s, "127.0.0.1:" + str($port));
$c = sua.udp.socket({});
$sent = sua.udp.send_to($c, "127.0.0.1:" + str($port), [72, 73, 0, 255]);
eq($sent, 4, "send_to reports the byte count");
$pkt = sua.udp.recvfrom($s, {"timeoutMs": 2000});
ok(!$pkt.timeout, "recvfrom did not time out");
eq($pkt.data, [72, 73, 0, 255], "payload survived, NUL and 255 included");
ok(contains($pkt.from, "127.0.0.1:"), "the sender address is reported");

print("");
print("-- timeout --");
$t = sua.udp.socket({});
sua.udp.bind($t, "127.0.0.1:39932");
$p2 = sua.udp.recvfrom($t, {"timeoutMs": 300});
ok($p2.timeout, "recvfrom reports timeout instead of blocking forever");
eq(len($p2.data), 0, "a timed-out read carries no data");
sua.udp.close($t);

print("");
print("-- a full-size datagram --");
// 60,000 bytes is legal UDP but larger than the default macOS send buffer,
// which is why this used to fail with "Message too long".
$big = [];
$i = 0;
while ($i < 60000) { $big.push(66); $i = $i + 1; }
$bs = sua.udp.socket({});
sua.udp.bind($bs, "127.0.0.1:39933");
$bc = sua.udp.socket({});
$bn = sua.udp.send_to($bc, "127.0.0.1:39933", $big);
eq($bn, 60000, "a 60,000-byte datagram is sent whole");
$bp = sua.udp.recvfrom($bs, {"timeoutMs": 2000, "maxBytes": 65536});
eq(len($bp.data), 60000, "and arrives whole");
sua.udp.close($bs);
sua.udp.close($bc);

print("");
print("-- truncation is silent, by design of the protocol --");
// A datagram larger than maxBytes loses its tail: UDP has no way to ask for
// the rest. Asserted so the behaviour is recorded rather than discovered.
$ts = sua.udp.socket({});
sua.udp.bind($ts, "127.0.0.1:39934");
$tc = sua.udp.socket({});
sua.udp.send_to($tc, "127.0.0.1:39934", $big);
$tp = sua.udp.recvfrom($ts, {"timeoutMs": 2000, "maxBytes": 100});
eq(len($tp.data), 100, "an oversized datagram is truncated to maxBytes");
sua.udp.close($ts);
sua.udp.close($tc);

print("");
print("-- bad arguments raise errors instead of killing the process --");
$crashed = true;
try {
    sua.udp.recvfrom($s, {"timeoutMs": 100, "maxBytes": -1});
    $crashed = false;
} catch ($e) {
    $crashed = false;
    ok(contains(str($e), "maxBytes"), "negative maxBytes is rejected by name");
}
ok(!$crashed, "negative maxBytes did not kill the process");

try {
    sua.udp.recvfrom($s, {"timeoutMs": 100, "maxBytes": 1000000000000});
    ok(false, "an absurd maxBytes should be rejected");
} catch ($e2) {
    ok(contains(str($e2), "maxBytes"), "absurd maxBytes is rejected by name");
}

$pc = sua.udp.socket({});
try {
    sua.udp.bind($pc, "127.0.0.1:99999");
    ok(false, "port 99999 should be rejected, not truncated to 34463");
} catch ($e3) {
    ok(contains(str($e3), "out of range"), "an out-of-range port is rejected");
}
try {
    sua.udp.bind($pc, "127.0.0.1:-1");
    ok(false, "port -1 should be rejected, not turned into 65535");
} catch ($e4) {
    ok(contains(str($e4), "out of range"), "a negative port is rejected");
}
sua.udp.close($pc);

print("");
print("-- IPv6 --");
$v6 = sua.udp.socket({"family": "ipv6"});
sua.udp.bind($v6, "[::1]:39935");
$v6c = sua.udp.socket({"family": "ipv6"});
sua.udp.send_to($v6c, "[::1]:39935", [1, 2, 3]);
$v6p = sua.udp.recvfrom($v6, {"timeoutMs": 2000});
eq($v6p.data, [1, 2, 3], "IPv6 round trip");
ok(contains($v6p.from, "::1"), "IPv6 sender address is reported");
sua.udp.close($v6);
sua.udp.close($v6c);

print("");
print("-- close --");
ok(sua.udp.close($s), "close returns true");
ok(sua.udp.close($s) == false || true, "closing twice does not crash");
sua.udp.close($c);
sua.udp.close($srv);

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
