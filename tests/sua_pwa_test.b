// ════════════════════════════════════════════════════════════════════════
//  sua_pwa_test.b — the PWA layer of the sua web framework.
//  Run:  bantu run tests/sua_pwa_test.b
//
//  Covers what django-pwa provides: a manifest rendered from flat settings, a
//  service worker served at the root, an offline page, and a <head> meta block.
//  The HTTP end of it (routes actually serving these) is covered by
//  tests/sua_pwa_http_test.sh, which boots a server and curls it.
// ════════════════════════════════════════════════════════════════════════

$R = {"pass": 0, "fail": 0};
def eq($got, $want, $name) {
    if ($got == $want) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); print("          got:  " + str($got)); print("          want: " + str($want)); }
}
def ok($cond, $name) {
    if ($cond) { $R.pass = $R.pass + 1; print("  ok    " + $name); }
    else { $R.fail = $R.fail + 1; print("  FAIL  " + $name); }
}
def rep($ch, $n) {
    $s = "";
    $i = 0;
    while ($i < $n) { $s = $s + $ch; $i = $i + 1; }
    return $s;
}

print("");
print("-- availability --");
ok(has_native("pwa"), "has_native(\"pwa\")");

print("");
print("-- configure() --");
$cfg = sua.pwa.configure({
    "name": "Bantu Demo",
    "short_name": "Demo",
    "description": "A progressive web app written in Bantu",
    "theme_color": "#0A0302",
    "background_color": "#ffffff",
    "display": "standalone",
    "start_url": "/",
    "scope": "/",
    "orientation": "portrait",
    "lang": "en-GB",
    "dir": "ltr",
    "icons": [
        {"src": "/img/icon-192.png", "sizes": "192x192", "type": "image/png"},
        {"src": "/img/icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "any maskable"}
    ],
    "shortcuts": [{"name": "Inbox", "url": "/inbox", "description": "Jump to the inbox"}],
    "categories": ["productivity"],
    "precache": ["/", "/css/app.css"],
    "debug": true
});
ok($cfg.configured, "configure returns configured=true");
eq(len($cfg.routes), 5, "five routes registered");

$c = sua.pwa.config();
eq($c.name, "Bantu Demo", "name stored");
eq($c.short_name, "Demo", "short_name stored");
eq($c.theme_color, "#0A0302", "theme_color stored");
eq($c.offline_url, "/offline", "offline_url defaults to /offline");
ok($c.auto_inject, "auto_inject defaults on");

print("");
print("-- the manifest --");
$mtext = sua.pwa.manifest();
$m = json.parse($mtext);
eq($m.name, "Bantu Demo", "manifest name");
eq($m.short_name, "Demo", "manifest short_name");
eq($m.display, "standalone", "manifest display");
eq($m.start_url, "/", "manifest start_url");
eq($m.scope, "/", "manifest scope");
eq($m.theme_color, "#0A0302", "manifest theme_color");
eq($m.background_color, "#ffffff", "manifest background_color");
eq($m.orientation, "portrait", "manifest orientation");
eq($m.lang, "en-GB", "manifest lang");
eq($m.description, "A progressive web app written in Bantu", "manifest description");
eq(len($m.icons), 2, "manifest carries both icons");
eq($m.icons[0].sizes, "192x192", "icon sizes survive");
// Keys we don't model must pass through untouched.
eq($m.icons[1].purpose, "any maskable", "icon purpose passes through");
eq(len($m.shortcuts), 1, "shortcuts pass through");
eq($m.shortcuts[0].url, "/inbox", "shortcut url");
eq($m.categories[0], "productivity", "categories pass through");

print("");
print("-- the service worker --");
$sw = sua.pwa.serviceworker();
ok(len($sw) > 500, "service worker is non-trivial");
ok(contains($sw, "addEventListener('install'"), "handles install");
ok(contains($sw, "addEventListener('activate'"), "handles activate");
ok(contains($sw, "addEventListener('fetch'"), "handles fetch (required for installability)");
ok(contains($sw, "addEventListener('push'"), "handles push");
ok(contains($sw, "addEventListener('notificationclick'"), "handles notificationclick");
ok(contains($sw, "/offline"), "knows the offline url");
ok(contains($sw, "/css/app.css"), "precaches the configured assets");
ok(contains($sw, "caches.delete"), "cleans up old caches on activate");
ok(contains($sw, "showNotification"), "shows a notification on push");
ok(contains($sw, "const DEBUG       = true"), "debug flag threads through");

print("");
print("-- the client helper --");
$js = sua.pwa.client_js();
ok(contains($js, "navigator.serviceWorker.register"), "registers the worker");
ok(contains($js, "'/serviceworker.js'"), "registers from the ROOT (scope matters)");
ok(contains($js, "beforeinstallprompt"), "captures the install prompt");
ok(contains($js, "promptInstall"), "exposes promptInstall()");
ok(contains($js, "subscribePush"), "exposes subscribePush()");
ok(contains($js, "window.BantuPWA"), "publishes window.BantuPWA");
ok(contains($js, "userVisibleOnly: true"), "subscribes with userVisibleOnly");

print("");
print("-- the <head> meta block --");
$meta = sua.pwa.meta();
ok(contains($meta, "rel=\"manifest\""), "links the manifest");
ok(contains($meta, "name=\"theme-color\" content=\"#0A0302\""), "theme-color meta");
ok(contains($meta, "apple-mobile-web-app-capable"), "apple capable meta");
ok(contains($meta, "apple-mobile-web-app-title\" content=\"Demo\""), "apple title uses short_name");
ok(contains($meta, "apple-touch-icon"), "apple touch icon");
ok(contains($meta, "/pwa.js"), "loads the client helper");
ok(contains($meta, "img/icon-192.png"), "references the configured icon");

print("");
print("-- html escaping --");
// configure() MERGES into the existing config (like Django settings) rather
// than resetting it, so short_name must be set explicitly here — the meta tags
// render short_name, not name.
sua.pwa.configure({
    "name": "A \"quoted\" & <tagged> app",
    "short_name": "A \"quoted\" & <tagged> app",
    "theme_color": "#123456"
});
$meta2 = sua.pwa.meta();
ok(contains($meta2, "&quot;quoted&quot;"), "quotes are escaped in meta");
ok(contains($meta2, "&lt;tagged&gt;"), "angle brackets are escaped in meta");
ok(!contains($meta2, "<tagged>"), "no raw tag injection");
$m2 = json.parse(sua.pwa.manifest());
eq($m2.name, "A \"quoted\" & <tagged> app", "manifest json round-trips the name");

print("");
print("-- inject() --");
$page = "<html><head><title>Hi</title></head><body>hello</body></html>";
$injected = sua.pwa.inject($page);
ok(contains($injected, "rel=\"manifest\""), "injects the meta block");
ok(contains($injected, "<title>Hi</title>"), "keeps the original head");
ok(contains($injected, "hello"), "keeps the body");
// The manifest link must land inside <head>, before </head>.
ok(indexOf($injected, "rel=\"manifest\"") < indexOf($injected, "</head>"), "injects inside <head>");

// Idempotent: a page that already has the block is left alone.
eq(sua.pwa.inject($injected), $injected, "inject is idempotent");

$noHead = "<div>fragment</div>";
eq(sua.pwa.inject($noHead), $noHead, "a non-document is left untouched");

// A page that only MENTIONS the markers — in a comment, or in documentation —
// must still get the block. A plain substring guard silently stripped the PWA
// tags from any page whose source talked about them.
$commented = "<html><head><title>T</title><!-- we inject /pwa.js and rel=\"manifest\" here --></head><body></body></html>";
$ci = sua.pwa.inject($commented);
ok(contains($ci, "<script src=\"/pwa.js\" defer>"), "a commented-out mention does not block injection");
ok(contains($ci, "<link rel=\"manifest\""), "the manifest link is injected too");
// ...but a real tag still blocks it.
$realTag = "<html><head><link rel=\"manifest\" href=\"/manifest.json\"></head><body></body></html>";
eq(sua.pwa.inject($realTag), $realTag, "an existing manifest link blocks injection");

$upperHead = "<HTML><HEAD></HEAD><BODY></BODY></HTML>";
ok(contains(sua.pwa.inject($upperHead), "rel=\"manifest\""), "handles uppercase </HEAD>");

print("");
print("-- the offline page --");
$off = sua.pwa.offline_page();
ok(contains($off, "<!doctype html>"), "is a full document");
ok(contains($off, "offline"), "says it is offline");
ok(contains($off, "location.reload"), "offers a retry");
ok(contains($off, "rel=\"manifest\""), "carries the meta block too");

print("");
print("-- routes are registered on the server --");
$routes = sua.server.routes();
def hasRoute($rs, $path) {
    $found = false;
    each ($r in $rs) {
        if ($r.path == $path) { $found = true; }
    }
    return $found;
}
ok(hasRoute($routes, "/manifest.json"), "GET /manifest.json");
ok(hasRoute($routes, "/manifest.webmanifest"), "GET /manifest.webmanifest");
ok(hasRoute($routes, "/serviceworker.js"), "GET /serviceworker.js");
ok(hasRoute($routes, "/pwa.js"), "GET /pwa.js");
ok(hasRoute($routes, "/offline"), "GET /offline");

// configure() again must not duplicate them.
sua.pwa.configure({"name": "Bantu Demo"});
$routes2 = sua.server.routes();
$n1 = 0;
each ($r in $routes2) {
    if ($r.path == "/manifest.json") { $n1 = $n1 + 1; }
}
eq($n1, 1, "reconfiguring does not duplicate routes");

print("");
print("-- push configuration --");
ok(sua.push.available(), "sua.push.available()");
$keys = sua.push.vapid_keys();
ok($keys != null, "vapid_keys() generates a pair");
eq(len($keys.public_key), 87, "public key is 65 octets");
eq(len($keys.private_key), 43, "private key is 32 octets");

$bad = sua.push.configure({"public_key": $keys.public_key, "private_key": "nope"});
ok(!$bad.ok, "rejects a malformed private key");

$keys2 = sua.push.vapid_keys();
$mismatch = sua.push.configure({"public_key": $keys2.public_key, "private_key": $keys.private_key});
ok(!$mismatch.ok, "rejects a mismatched keypair");

$badsub = sua.push.configure({"public_key": $keys.public_key, "private_key": $keys.private_key, "subject": "dev@example.com"});
ok(!$badsub.ok, "requires a mailto: or https: subject");

$pc = sua.push.configure({
    "public_key": $keys.public_key,
    "private_key": $keys.private_key,
    "subject": "mailto:dev@example.com",
    "db": "/tmp/bantu_pwa_test_subs.db"
});
ok($pc.ok, "configure succeeds with a matching pair");
eq($pc.subscribe_url, "/pwa/subscribe", "default subscribe url");

// The public key must now reach the browser through /pwa.js.
ok(contains(sua.pwa.client_js(), $keys.public_key), "client_js carries the VAPID public key");

print("");
print("-- the subscription store --");
$sub = webpush_keygen();
$s1 = {"endpoint": "https://push.example.com/a", "keys": {"p256dh": $sub.public_key, "auth": b64url_encode(randbytes(16))}};
ok(sua.push.save($s1, "team-a"), "save a subscription");
eq(sua.push.count(), 1, "one stored");
eq(sua.push.count("team-a"), 1, "found by tag");
eq(sua.push.count("team-b"), 0, "not found under another tag");

// Re-saving the same endpoint updates rather than duplicating.
ok(sua.push.save($s1, "team-a"), "re-save the same endpoint");
eq(sua.push.count(), 1, "still one (endpoint is the primary key)");

$s2 = {"endpoint": "https://push.example.com/b", "keys": {"p256dh": $sub.public_key, "auth": b64url_encode(randbytes(16))}};
sua.push.save($s2, "team-b");
eq(sua.push.count(), 2, "two stored");

$list = sua.push.subscriptions("team-b");
eq(len($list), 1, "list by tag");
eq($list[0].endpoint, "https://push.example.com/b", "the right row");

ok(!sua.push.save({"endpoint": "https://x/y", "keys": {"p256dh": "bad", "auth": "z"}}, ""), "rejects an unusable p256dh");
eq(sua.push.count(), 2, "the bad subscription was not stored");

ok(sua.push.forget("https://push.example.com/a"), "forget a subscription");
eq(sua.push.count(), 1, "one left");
ok(!sua.push.forget("https://push.example.com/a"), "forgetting twice is false");

// Sending needs a reachable push service, so only the guard rails are asserted.
$bad2 = sua.push.send({"endpoint": "http://insecure/x", "keys": {"p256dh": $sub.public_key, "auth": b64url_encode(randbytes(16))}}, {"body": "hi"});
ok(!$bad2.ok, "send rejects a non-https endpoint");
ok(contains($bad2.error, "https"), "and says why");

$oversize = sua.push.send($s2, rep("x", 5000));
ok(!$oversize.ok, "send rejects an oversized payload");
ok(contains($oversize.error, "limit"), "and names the limit");

sua.push.forget("https://push.example.com/b");

print("");
print("========================================");
print("  PASS: " + str($R.pass) + "   FAIL: " + str($R.fail));
print("========================================");
if ($R.fail > 0) { print("  RESULT: FAILURES PRESENT"); }
if ($R.fail == 0) { print("  RESULT: ALL GREEN"); }
