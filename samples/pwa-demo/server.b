// ════════════════════════════════════════════════════════════════════════
//  pwa-demo — an installable, offline-capable, push-enabled Bantu web app.
//
//  Run:   bantu run samples/pwa-demo/server.b
//  Open:  http://localhost:3100
//
//  Service workers need a secure context, and localhost counts as one — so
//  everything here works over plain http during development. Deploy behind
//  HTTPS and it works unchanged.
// ════════════════════════════════════════════════════════════════════════

// ── VAPID keys ──────────────────────────────────────────────────────────
// Generated on first run and reused thereafter. They must stay STABLE: the
// public key is baked into every subscription a browser creates, so generating
// a fresh pair on each restart would silently invalidate all of them.
$keys = sua.push.keys("./samples/pwa-demo/vapid.json");

// ── PWA configuration ───────────────────────────────────────────────────
// One flat dict, mirroring django-pwa's PWA_APP_* settings. This alone makes
// the app installable: it registers /manifest.json, /serviceworker.js,
// /offline and /pwa.js.
sua.pwa.configure({
    "name": "Bantu PWA Demo",
    "short_name": "Bantu PWA",
    "description": "An installable, offline-capable Bantu app with push notifications",
    "theme_color": "#5b21b6",
    "background_color": "#0f0f13",
    "display": "standalone",
    "start_url": "/",
    "scope": "/",
    "orientation": "portrait",
    "lang": "en-US",
    "icons": [
        {"src": "/icons/icon-192.png", "sizes": "192x192", "type": "image/png"},
        {"src": "/icons/icon-512.png", "sizes": "512x512", "type": "image/png"},
        {"src": "/icons/icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "maskable"}
    ],
    "shortcuts": [
        {"name": "Send a test push", "url": "/?action=notify", "description": "Fire a notification at every subscriber"}
    ],
    "categories": ["utilities"],
    // Fetched and cached during install, so these work with no network at all.
    "precache": ["/", "/css/app.css", "/icons/icon-192.png"],
    "debug": true
});

// ── Push ────────────────────────────────────────────────────────────────
if ($keys != null) {
    $pc = sua.push.configure({
        "public_key": $keys.public_key,
        "private_key": $keys.private_key,
        "subject": "mailto:demo@example.com",
        "db": "./samples/pwa-demo/subscriptions.db"
    });
    if (!$pc.ok) { print("  [demo] push configuration failed: " + str($pc.error)); }
}

// ── Routes ──────────────────────────────────────────────────────────────

sua.server.get("/api/status", def($req, $res) {
    $res.json({
        "app": "Bantu PWA Demo",
        "push_available": has_native("webpush"),
        "subscribers": sua.push.count(),
        "vapid_public_key": $keys.public_key
    });
});

// Fire a notification at every stored subscriber. send_all prunes any
// subscription the push service reports as gone (404/410).
sua.server.post("/api/notify", def($req, $res) {
    $title = "Hello from Bantu";
    $body = "This notification was encrypted and signed by the interpreter itself.";
    if ($req.body != null) {
        if ($req.body.title != null) { $title = $req.body.title; }
        if ($req.body.body != null) { $body = $req.body.body; }
    }
    $result = sua.push.send_all({
        "head": $title,
        "body": $body,
        "icon": "/icons/icon-192.png",
        "url": "/"
    }, {"ttl": 3600, "urgency": "normal"});

    $res.json({
        "ok": $result.ok,
        "sent": $result.sent,
        "failed": $result.failed,
        "pruned": $result.pruned
    });
});

// A cache-busting endpoint so you can see the offline fallback working:
// stop the server, reload, and the service worker serves /offline.
sua.server.get("/api/time", def($req, $res) {
    $res.json({"now": clock()});
});

sua.server.static("./samples/pwa-demo/public");

print("");
print("  ┌──────────────────────────────────────────────────────┐");
print("  │  Bantu PWA Demo                                      │");
print("  │  http://localhost:3100                               │");
print("  │                                                      │");
print("  │  1. Open it in Chrome or Edge                        │");
print("  │  2. Click \"Install\" (or the address-bar install icon) │");
print("  │  3. Click \"Enable notifications\", then \"Send a push\"  │");
print("  │  4. Stop this server and reload to see /offline       │");
print("  └──────────────────────────────────────────────────────┘");
print("");

sua.server.listen(3100);
