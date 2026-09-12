# `sua` — Bantu's web framework

`sua` is built into the interpreter, so there is nothing to install or include. Every Bantu program
has a global `sua` object.

```bantu
sua.server.get("/", def($req, $res) {
    $res.json({"hello": "world"});
});
sua.server.listen(3000);
```

**Contents** — [Routing](#routing) · [`$req`](#req--the-request) · [`$res`](#res--the-response) ·
[Static files](#static-files) · [PWA](#pwa--installable-offline-capable-apps) ·
[Push notifications](#push-notifications) · [HTTP client](#http-client) · [Databases](#databases) ·
[Limitations](#known-limitations)

---

## Routing

```bantu
sua.server.get(path, handler, options?)
sua.server.post(path, handler, options?)
sua.server.put(path, handler, options?)
sua.server.delete(path, handler, options?)
sua.server.patch(path, handler, options?)
sua.server.head(path, handler, options?)
sua.server.options(path, handler, options?)   // path defaults to "*"
sua.server.all(path, handler, options?)       // GET, POST, PUT, DELETE, PATCH
sua.server.routes()                           // -> [{method, path}, ...]
sua.server.listen(port)                       // blocks forever
```

The optional third argument currently understands one key, `{"suspend": true}` — see
[Suspending handlers](#suspending-handlers). Unknown keys are ignored.

A handler takes `($req, $res)`. Matching runs in three passes, first match wins:

1. **Exact** — `route.path == request.path`.
2. **`:param`** — segment counts must be equal; `:name` segments capture into `$req.params`.
3. **`OPTIONS *`** — a catch-all used for CORS preflight.

```bantu
sua.server.get("/users/:id/posts/:slug", def($req, $res) {
    $res.json({"user": $req.params["id"], "post": $req.params["slug"]});
});
```

There are no wildcards, regex routes, route prefixes, or trailing-slash normalisation. `/users` and
`/users/` are different paths.

**Routes beat static files.** Static files are only consulted for a GET that matched no route.

## `$req` — the request

| Field | Type | Notes |
|---|---|---|
| `$req.method` | string | `"GET"`, `"POST"`, … |
| `$req.path` | string | path only, no query string |
| `$req.url` | string | the full request target, query string included |
| `$req.params` | dict | captures from `:name` segments |
| `$req.query` | dict | parsed and URL-decoded query string |
| `$req.headers` | dict | **keys are lowercased** — `$req.headers["content-type"]` |
| `$req.body` | any | see below |

`$req.body` is parsed JSON when the request's `Content-Type` contains `json`, the raw string for any
other content type, and `null` when there is no body.

## `$res` — the response

Every method returns `$res`, so calls chain.

| Method | Effect |
|---|---|
| `$res.json(value)` | serialises to JSON, sets `application/json; charset=utf-8` |
| `$res.send(value)` | objects and lists become JSON; anything else is sent as `text/plain` |
| `$res.status(code)` | sets the status code |
| `$res.set(name, value)` | sets a response header |
| `$res.type(contentType)` | sets the Content-Type verbatim |
| `$res.redirect(url)` | 302 plus a `Location` header |

```bantu
sua.server.post("/users", def($req, $res) {
    if ($req.body == null) {
        $res.status(400).json({"error": "body required"});
        return null;
    }
    $res.status(201).set("Location", "/users/1").json({"ok": true});
});
```

To send HTML, set the type before the body — `send` only overrides the content type when it is still
the default:

```bantu
$res.type("text/html; charset=utf-8").send("<h1>Hi</h1>");
```

**Header caveat:** headers are stored in a map, one value per name, so you cannot emit two
`Set-Cookie` headers in one response.

CORS headers (`Access-Control-Allow-Origin: *` and friends) are added to every response
unconditionally.

## Static files

```bantu
sua.server.static("./public");
```

Serves files from the directory for GET requests that matched no route. `/` maps to `index.html`.
Multiple calls add multiple roots, searched in order.

Content types are chosen by extension and cover HTML, CSS, JS, JSON, `.webmanifest`, SVG, PNG, JPEG,
GIF, WebP, AVIF, ICO, WASM, PDF, WOFF/WOFF2/TTF/OTF, MP4/WebM, MP3/OGG/WAV, CSV, XML and plain text.
Anything else is `application/octet-stream`.

HTML and `.webmanifest` are served `no-cache`; everything else gets `public, max-age=300`.

---

## PWA — installable, offline-capable apps

`sua.pwa` turns any sua app into a Progressive Web App: an installable manifest, a service worker
with offline support, and the client glue. It is modelled on Python's **django-pwa** — see
[docs/pwa-research.md](pwa-research.md) for the design notes.

### Quick start

```bantu
sua.pwa.configure({
    "name": "My App",
    "short_name": "App",
    "theme_color": "#2563eb",
    "icons": [
        {"src": "/icons/icon-192.png", "sizes": "192x192", "type": "image/png"},
        {"src": "/icons/icon-512.png", "sizes": "512x512", "type": "image/png"}
    ],
    "precache": ["/", "/css/app.css"]
});

sua.server.static("./public");
sua.server.listen(8080);
```

That one call registers five routes:

| URL | Serves |
|---|---|
| `/manifest.json` | the web app manifest |
| `/manifest.webmanifest` | the same document, for servers that prefer this extension |
| `/serviceworker.js` | the generated service worker |
| `/pwa.js` | `window.BantuPWA` — registration, install prompt, push |
| `/offline` | `public/offline.html` if it exists, else a built-in page |

The service worker is served from the **root** deliberately: a worker's scope is the directory it is
served from, so only a root-served worker can control the whole site.

`bantu init --pwa <name>` scaffolds all of this, including placeholder icons.

### Configuration

Every key is optional. The names mirror django-pwa's `PWA_APP_*` settings with the prefix dropped.

| Key | Default | Purpose |
|---|---|---|
| `name` | `"Bantu App"` | full application name |
| `short_name` | `name` | home-screen label, and what the meta tags use |
| `description` | — | manifest description |
| `theme_color` | `"#000000"` | browser UI colour |
| `background_color` | `"#ffffff"` | splash-screen background |
| `display` | `"standalone"` | `fullscreen`, `standalone`, `minimal-ui`, `browser` |
| `scope` | `"/"` | URLs the app controls |
| `start_url` | `"/"` | where launching from the home screen goes |
| `orientation` | `"any"` | `portrait`, `landscape`, … |
| `lang` / `dir` | `"en-US"` / `"ltr"` | language and text direction |
| `status_bar_color` | `"default"` | iOS status-bar style |
| `icons` | `[]` | list of `{src, sizes, type, purpose}` |
| `icons_apple` | falls back to `icons` | `apple-touch-icon` links |
| `splash_screen` | `[]` | `{src, media}` for iOS startup images |
| `screenshots` / `shortcuts` / `categories` | — | passed through to the manifest verbatim |
| `precache` | `[]` | URLs cached during service-worker install |
| `offline_url` | `"/offline"` | the offline fallback |
| `service_worker` | — | path to your own worker, replacing the generated one |
| `cache_version` | `"v1"` | bump to invalidate every cached asset |
| `auto_inject` | `true` | patch the meta tags into served HTML |
| `auto_register` | `true` | register the worker from `/pwa.js` |
| `debug` | `false` | `console.log` tracing in the worker and client |

`configure()` **merges** into the existing configuration rather than replacing it, so you can call it
more than once. Calling it again re-registers the routes without duplicating them.

Unknown keys inside `icons`, `shortcuts` and `screenshots` pass through untouched, so
`"purpose": "maskable"` and anything the spec adds later just work.

### Meta tags

With `auto_inject` on (the default), the `<head>` block is patched into every HTML page you serve —
including static files — so an existing app becomes installable without editing a template. The
injection is skipped if the page already links a manifest, and mentions inside HTML comments do not
count.

To place the tags yourself:

```bantu
sua.pwa.configure({ "auto_inject": false, ... });

sua.server.get("/", def($req, $res) {
    $res.type("text/html").send("<html><head>" + sua.pwa.meta() + "</head><body>…</body></html>");
});
```

| Call | Returns |
|---|---|
| `sua.pwa.meta()` | the `<head>` block (django-pwa's `{% progressive_web_app_meta %}`) |
| `sua.pwa.manifest()` | the manifest as a JSON string |
| `sua.pwa.serviceworker()` | the generated worker source |
| `sua.pwa.client_js()` | the `/pwa.js` source |
| `sua.pwa.offline_page()` | the built-in offline page |
| `sua.pwa.inject(html)` | returns `html` with the block inserted into `<head>` |
| `sua.pwa.config()` | the effective configuration |

### The generated service worker

- **install** — caches `offline_url` plus everything in `precache`. Individual failures are skipped
  so one 404 cannot break the install.
- **activate** — deletes caches from previous `cache_version`s, then claims open clients.
- **fetch** — navigations are network-first (fresh content) falling back to cache and then the
  offline page; other assets are cache-first falling back to the network. Cross-origin requests are
  left alone.
- **push** / **notificationclick** — see below.

Supply your own with `"service_worker": "./public/sw.js"`.

### `window.BantuPWA`

Available on any page that loads `/pwa.js`.

| Method | Purpose |
|---|---|
| `register()` | register the service worker (automatic unless `auto_register` is false) |
| `isInstalled()` | is the app running standalone? |
| `canInstall()` / `onInstallPrompt(cb)` | is an install prompt available? |
| `promptInstall()` | show it; resolves to `"accepted"` or `"dismissed"` |
| `pushSupported()` / `permission()` | push capability and notification permission |
| `isSubscribed()` | is this browser subscribed? |
| `subscribePush(extra)` | ask permission, subscribe, POST the subscription to the server |
| `unsubscribePush()` | unsubscribe here and on the server |

---

## Push notifications

Real Web Push: RFC 8291 payload encryption (`aes128gcm`) and RFC 8292 VAPID, implemented natively.
`has_native("webpush")` reports whether the binary supports it.

### Setup

```bantu
$keys = sua.push.keys("./vapid.json");     // generated on first run, reused after

sua.push.configure({
    "public_key":  $keys.public_key,
    "private_key": $keys.private_key,
    "subject":     "mailto:you@example.com",   // must be mailto: or https:
    "db":          "./subscriptions.db"
});
```

> **Keep the keypair secret and stable.** The public key is embedded in every subscription a browser
> creates, so replacing it silently invalidates all of them. `sua.push.keys()` exists to make
> "generate once, then reuse" the easy path.

`configure()` also registers `POST` and `DELETE` on `/pwa/subscribe` (override with
`"subscribe_url"`), which is where `BantuPWA.subscribePush()` sends subscriptions.

### Sending

```bantu
sua.push.send_all({
    "head": "Build finished",
    "body": "All 312 tests passed.",
    "icon": "/icons/icon-192.png",
    "url":  "/builds/latest"
}, {"ttl": 3600, "urgency": "normal"});
```

The payload shape matches django-webpush and is what the generated worker expects: `head`, `body`,
`icon`, `badge`, `tag`, `url`, `renotify`, `requireInteraction`. `url` is where a click takes the
user.

| Call | Purpose |
|---|---|
| `sua.push.available()` | is Web Push usable in this binary? |
| `sua.push.keys(path)` | load or generate the VAPID keypair |
| `sua.push.vapid_keys()` | generate a keypair without saving it |
| `sua.push.configure(opts)` | set keys, subject and store; register the subscribe route |
| `sua.push.save(subscription, tag)` | store a subscription |
| `sua.push.forget(endpoint)` | delete one |
| `sua.push.subscriptions(tag)` / `sua.push.count(tag)` | read the store |
| `sua.push.send(subscription, payload, opts)` | send to one subscriber |
| `sua.push.send_all(payload, opts, tag)` | send to all (or all with `tag`) |

`send_all` returns `{ok, sent, failed, pruned, results}` and **deletes subscriptions the push service
reports as gone** (404/410).

Options: `ttl` (seconds, default 86400), `urgency` (`very-low`…`high`), `topic` (a later message with
the same topic replaces an undelivered earlier one).

**Payloads are capped at 3993 octets** — 4096 minus the 86-octet header, the padding delimiter and
the authentication tag. Oversized payloads are rejected before any network call.

`tag` groups subscriptions, the equivalent of django-webpush's groups:

```bantu
BantuPWA.subscribePush({ tag: "user-42" });      // in the browser
sua.push.send_all($payload, {}, "user-42");      // on the server
```

### Diagnosing failures

`send` returns `{ok, status, endpoint, bytes, expired, error}`.

| Status | Meaning |
|---|---|
| 201 | delivered to the push service |
| 400 | malformed request |
| 401 / 403 | VAPID rejected — check `subject`, and that the public key matches the one the browser subscribed with |
| 404 / 410 | the subscription is dead; `send_all` prunes these |
| 413 | payload too large |
| 429 | rate limited |

The single most common cause of a 401 is an `aud` claim built from the full endpoint URL rather than
its origin. `sua` derives it correctly; `webpush_aud(endpoint)` exposes the same logic.

### Low-level atoms

Used by `sua.push`; available directly for building something else.

```
webpush_selftest()                                -> {ok, ran, failed}
webpush_keygen()                                  -> {public_key, private_key}
webpush_public_key(private)                       -> public key
webpush_encrypt(p256dh, auth, plaintext)          -> byte-list body
webpush_decrypt(private, auth, body)              -> byte-list plaintext
webpush_jwt(private, aud, sub, exp)               -> signed ES256 JWT
webpush_vapid_header(private, aud, sub, exp)      -> "vapid t=…, k=…"
webpush_aud(endpoint)                             -> the origin for the aud claim
b64url_encode(bytes) / b64url_decode(string)
```

A known-answer selftest (FIPS 197, NIST GCM, RFC 5869, RFC 6979 §A.2.5, RFC 5903) runs once on first
use and **fails closed**: if any vector fails, `has_native("webpush")` is false and every entry point
returns `null` rather than emitting a broken or insecure push.

---

## HTTP client

```bantu
sua.http.get(url)
sua.http.post(url, body, contentType?)
sua.http.put(url, body, contentType?)
sua.http.patch(url, body, contentType?)
sua.http.delete(url)
sua.http.head(url)
```

For anything needing custom headers or a binary body, use the general form:

```bantu
$r = sua.http.request({
    "method":  "POST",
    "url":     "https://api.example.com/v1/items",
    "headers": {"Authorization": "Bearer " + $token},
    "body":    {"name": "widget"},          // string, byte-list, or object (auto-JSON)
    "timeout": 15
});

if ($r.ok) { print($r.body); }
```

Returns `{ok, status, statusText, body, headers, url, method}`, or `{ok: false, status: 0, error}` on
a transport failure.

### TLS

**Certificates are verified.** Pass `"insecure": true` on a single `sua.http.request()` for a
self-signed development endpoint — and only then:

```bantu
$r = sua.http.request({"url": "https://self-signed.internal/x", "insecure": true});
```

The six convenience helpers take no options object, so they have a global escape hatch instead. It
is deliberately loud — it prints a warning and applies to every subsequent call, so prefer the
per-request flag whenever you can scope it:

```bantu
sua.http.insecure(true);     // -> {insecure: true, verify: false}
sua.http.insecure(false);    // back to verifying
```

If verification fails, the returned `error` says so and names both ways forward.

### Bodies and logging

Bodies are length-explicit, so a body containing NUL bytes is transmitted intact — binary payloads
(images, protobuf, an `aes128gcm` push body) are safe.

Each request writes one trace line to **stderr**, naming only the origin:

```
  [HTTP] POST https://api.example.com -> 201 Created
```

The path and query are omitted on purpose: they routinely carry API tokens, and a Web Push endpoint's
path *is* the subscription identifier. Set `BANTU_HTTP_DEBUG=1` for the full URL while debugging, or
run under `bantu -q` to silence the trace entirely.

### Many requests at once

`sua.http.all(list, max_parallel?)` runs every request together and returns the responses **in
request order**, whatever order they finish in. Each element takes exactly the same options as
`sua.http.request`.

```bantu
$rs = sua.http.all([
    {"url": "https://a.example/one"},
    {"method": "POST", "url": "https://b.example/two", "body": {"n": 1}},
    {"url": "https://c.example/three", "timeout": 3}
]);
each ($r in $rs) { print($r.status); }
```

Six 500 ms requests take ~506 ms rather than ~3 s. A malformed element still occupies its slot, so
results always line up with inputs one for one.

This does **not** make your handler non-blocking — the call still waits, just once instead of N
times.

`sua.push.send_all()` uses the same machinery internally, so push fan-out to many subscribers is
already one wait rather than one round trip per subscriber. You do not have to do anything to get
that.

---

## Concurrency and workers

The server runs an **event loop**: one thread holds every connection, so an idle client costs its
buffers rather than an 8 MB thread stack. Handlers stay synchronous — no callbacks, no async.

```bantu
sua.server.workers(0);   // one process per core
sua.server.workers(4);   // exactly four
sua.server.listen(3000);
```

Call it **before** `listen()`. The fork happens inside `listen`, once your routes are registered.

### Globals are per-worker

This is the one thing to understand before turning workers on:

```bantu
$hits = 0;
sua.server.get("/hit", def($req, $res) { $hits = $hits + 1; $res.send(str($hits)); });
sua.server.workers(4);
```

With 4 workers each process has **its own `$hits`**, so each counts roughly a quarter and they never
meet. That is deliberate — no shared mutable state is what makes multi-worker safe — but it means
shared state belongs in a database, not a global. `workers(1)` (the default) is unchanged.

`sua.ws.broadcast` **does** cross workers: a broadcast reaches every client on every worker, not just
the ones that happen to share your process. `sua.ws.send(id, …)` likewise finds a client on another
worker. `sua.ws.clients()` lists only **this worker's** clients unless you enable `ws_roster` below.

**Counts are per worker, deliveries are not.** Under `workers(n)` these three report only what the
worker answering the call can see, even though the *message* reaches everyone:

| | what it counts |
|---|---|
| `sua.ws.broadcast(msg)` returns | clients on **this** worker. The others are reached over the bus and are not counted |
| `sua.server.stats().ws_clients` | clients on **this** worker |
| `sua.ws.clients()` | this worker's, plus the roster if `ws_roster` is on |

Measured with 4 workers and 64 clients: `broadcast` returned **49** and all **64** clients received
the frame. The number is not a delivery receipt — delivery across the bus is fire-and-forget, so no
exact total is available at the moment the call returns. Do not use the return value to decide
whether a broadcast worked; use it as a local hint, or count acknowledgements from clients.

Multi-worker mode is POSIX-only; on Windows it logs a notice and runs single-worker.

### Limits and stats

```bantu
sua.server.limits({"max_connections": 50000, "idle_timeout_ms": 120000});
$s = sua.server.stats();   // workers, worker, live_connections, ws_clients,
                           // bus, bus_sent, bus_received, bus_dropped,
                           // rejected_per_ip, distinct_ips,
                           // suspended, max_suspended, suspensions
```

`bus_dropped` above zero means broadcasts were shed to protect memory — the bus is saturated.

### Per-IP connection cap

`max_connections` is process-wide, so one host can occupy the whole table. `max_connections_per_ip`
stops that:

```bantu
sua.server.limits({"max_connections_per_ip": 64});
```

**It is off by default, deliberately.** Behind a reverse proxy — nginx, Cloudflare, a load balancer —
*every* connection arrives from the proxy's address, so any per-IP cap would throttle your whole site
at once. Turn it on when the server is directly internet-facing; leave it off behind a proxy and cap
there instead.

Rejected connections get `503` and are counted in `stats().rejected_per_ip`. Under
`sua.server.workers(n)` the cap is per worker, so the effective process-group limit is n × the value.

### Cross-worker client roster

`sua.ws.clients()` lists this worker's clients. To see every worker's:

```bantu
sua.server.limits({"ws_roster": true});
```

Workers then publish joins and leaves on the bus and each keeps the others' ids. It is **off by
default because it is not free**: one bus frame per connect/disconnect, and every worker holds every
client id — for 100k clients across 8 workers, 800k strings.

The roster is **eventually consistent**. A client that connected microseconds ago on another worker
may not appear yet, so treat the list as a snapshot rather than something to synchronise on. When a
worker dies the supervisor clears its entries from the others.

### Scaling to very large connection counts

What the **server** costs per connection, measured on this implementation:

| | measured |
|---|---|
| idle HTTP connection | **244 bytes** (10,000 held) |
| idle WebSocket connection | **702 bytes** (4,000 held) |
| broadcast throughput | **122,000 messages/second** (4,000 clients, 0 lost) |
| request throughput | **8,500 requests/second**, one worker |
| idle-connection reaping | **O(expired)**, not O(connections) |

At 702 bytes, two million WebSockets is about **1.4 GB of application memory** — comfortably inside a
4 KB-per-connection budget, with room to spare. Nothing in the server is O(connections) per event:
connections are held in last-activity order so the idle reaper stops at the first one that has not
expired, and `epoll`/`kqueue` report only the sockets that are actually ready.

**The remaining limits are the operating system's, not sua's**, and they dominate at that scale:

- **File descriptors.** One per connection, plus headroom. `ulimit -n`, and on Linux `fs.nr_open`
  and `fs.file-max`. macOS caps `kern.maxfilesperproc` far lower (61,440 by default), so large
  counts are a Linux exercise.
- **Kernel socket memory.** This is the dominant term, not the server's 702 bytes: each TCP socket
  carries kernel receive and send buffers whose *minimums* are `net.ipv4.tcp_rmem` and
  `net.ipv4.tcp_wmem` (4096 each by default), plus the socket structure itself. Budget several KB
  per idle connection and size `net.ipv4.tcp_mem` accordingly. Leave `tcp_moderate_rcvbuf` on so the
  kernel grows buffers only for connections that actually move data.
- **Accept backlog** — `net.core.somaxconn` and `net.core.netdev_max_backlog`, or connections are
  dropped during a surge rather than queued.
- **conntrack**, if a stateful firewall is in the path: `nf_conntrack_max` is a hard ceiling that
  fails in a confusing way when hit.
- **Client-side ports**, when load-testing from one machine: a single source address has roughly
  28,000 ephemeral ports. Test from several addresses or several hosts, or you will measure the
  client's limit and think it is the server's.

Use `sua.server.workers(0)` to put one event loop on each core. The per-connection costs above are
per worker, and connections are spread across them.

> **Honest limits of the numbers above.** They were measured up to 10,000 connections on one macOS
> machine, where `kern.maxfilesperproc` and loopback ephemeral ports prevent going further. The cost
> *per connection* is flat and the reaper is now O(expired), so nothing in the design degrades with
> count — but two million connections has not been run end to end, and the OS tuning above is what
> would decide it. The `poll` fallback backend (Windows, or any platform without epoll/kqueue) is
> O(connections) per wait by construction and will not scale to these numbers.

### Suspending handlers

A worker is one event loop on one thread, so a handler that waits on something makes every other
connection on that worker wait too. Mark a route `{"suspend": true}` and it hands the worker back
while it waits:

```bantu
sua.server.get("/proxy", def($req, $res) {
    $r = sua.http.get("https://slow.example/thing");   // suspends the handler,
    $res.json($r);                                     // not the worker
}, {"suspend": true});
```

No new syntax — no `async`, no `await`, no callbacks. The handler is the same handler; it just stops
holding the loop. Measured: ten handlers each waiting 300 ms complete in **310 ms** rather than 3 s,
and a fast request is served in **1 ms** while a 1.2 s handler is in flight.

These suspend: `sleep()`, `sua.http.*`, `sua.http.all`, and the two `sua.udp` calls that wait on the
network (`recvfrom` and the one-shot `send`). Everything else runs as it always has.

Only **routes** can opt in. `sua.ws.on(...)` handlers and the PWA/push routes always run on the loop,
so a `sleep()` or outbound call inside one still holds the worker. A suspendable route may freely
call `sua.ws.send` / `sua.ws.broadcast` — those reach the loop's connection table under the same
guarantee that only one thread runs Bantu code at a time.

#### Read this before turning it on

**A suspended handler is not atomic.** Today a handler runs start to finish with no other Bantu code
interleaved, and programs are written against that whether or not their authors realised it.
Suspension breaks it:

```bantu
$stock = 1;
sua.server.post("/buy", def($req, $res) {
    if ($stock > 0) {
        sua.http.post("https://payments/charge", $body);   // suspends HERE
        $stock = $stock - 1;                               // another buyer already passed the check
        $res.json({"ok": true});
    }
}, {"suspend": true});
```

Two buyers both pass `$stock > 0`, both are charged, stock goes to `-1`. This is a **logical** race,
not a memory one: no tool finds it and it appears only under concurrency. It is the ordinary cost of
cooperative multitasking — Node, asyncio and Go all live with it — but sua did not have it before,
which is exactly why this is **per route** and not a global switch. An unmarked handler is still
atomic, and always will be.

The rule: a marked handler must not read shared state, suspend, and then act on what it read. Re-read
it after the suspension, or keep the state in the database and let SQL do the check.

#### Cost and limits

A suspended handler is one OS thread; a connection whose handler is *not* suspended costs nothing
extra. So the thread count tracks concurrent outbound I/O — tens — not connections.

```bantu
sua.server.limits({"max_suspended_handlers": 256});   // default 256; set before listen()
$s = sua.server.stats();   // ... suspended, max_suspended, suspensions
```

Over the cap a suspendable handler simply **runs inline** — the behaviour of every earlier release,
so the request is still served, just without yielding the worker. It is never an error.
`suspended` sitting at `max_suspended` is the signal to raise it.

`sua.sqlite.*` does **not** suspend. sua has one process-wide connection, and offloading statements
onto it would make `lastInsertId` and `changes` report another handler's results — a silent
corruption worse than the stall it would fix. See `docs/sua-async-design.md` §9.2.

---

## UDP — `sua.udp`

Datagram sockets: DNS, STUN, NTP, game and telemetry protocols, anything where you want to send a
packet and not hold a connection open. Bytes are Bantu's usual representation — a list of integers
0–255, the same as the hash/crypto modules use.

```bantu
sua.udp.socket(opts?)                 // -> handle; opts: {"family": "ipv4"|"ipv6", "nonblocking": bool}
sua.udp.bind($sock, "host:port")      // port 0 = let the OS choose
sua.udp.send_to($sock, "host:port", $bytes)   // -> bytes sent
sua.udp.recvfrom($sock, opts?)        // -> {from, data, timeout}; opts: {"timeoutMs", "maxBytes"}
sua.udp.getsockname($sock)            // -> "host:port"
sua.udp.close($sock)                  // safe to call twice
sua.udp.send("host:port", $bytes, opts?)      // one-shot: socket, send, await reply, close
```

### A round trip

```bantu
$srv = sua.udp.socket({});
sua.udp.bind($srv, "127.0.0.1:39000");

$cli = sua.udp.socket({});
sua.udp.send_to($cli, "127.0.0.1:39000", [72, 73]);

$pkt = sua.udp.recvfrom($srv, {"timeoutMs": 2000});
if (!$pkt.timeout) {
    print("got " + str($pkt.data) + " from " + $pkt.from);
}
sua.udp.close($srv);
sua.udp.close($cli);
```

`recvfrom` returns `{"timeout": true, "from": "", "data": []}` when the timeout expires — it does not
raise. A hard socket error does raise.

The one-shot form covers the common query/response case:

```bantu
$r = sua.udp.send("8.8.8.8:53", $dnsQuery, {"timeoutMs": 2000});
```

IPv6 uses bracket form for the address, and the socket must be created for it:

```bantu
$s = sua.udp.socket({"family": "ipv6"});
sua.udp.bind($s, "[::1]:39001");
```

### Things worth knowing before you rely on it

**A datagram larger than `maxBytes` is silently truncated.** UDP has no way to ask for the rest, so
the tail is simply gone and nothing reports it. `maxBytes` defaults to **4096**; set it to 65536 if
you might receive full-size datagrams:

```bantu
$pkt = sua.udp.recvfrom($sock, {"timeoutMs": 1000, "maxBytes": 65536});
```

**`maxBytes` must be 0–65536 and ports must be 0–65535.** Both are validated and raise a catchable
Bantu error. They did not used to be: a negative `maxBytes` terminated the process outright, and
`"127.0.0.1:99999"` quietly bound to port 34463.

**A datagram is capped at 65,507 bytes** by the protocol, and sockets ask the kernel for a 64 KB send
and receive buffer so a full-size one actually goes out. Larger sends fail with "Message too long".

**Sockets are not closed for you.** There are no finalisers — a socket you do not `close()` stays
open until the process exits. In a long-running server, close every socket you create.

**Blocking, unless the route opts in.** `recvfrom` and the one-shot `send` wait on the network, and
inside an ordinary handler that holds the whole worker — a 3-second `recvfrom` made every other
connection on that worker wait 2.7 seconds, measured. On a route marked `{"suspend": true}` they
hand the worker back for the duration, exactly like `sleep` and `sua.http.*`:

```bantu
sua.server.get("/lookup", def($req, $res) {
    $r = sua.udp.send("8.8.8.8:53", $query, {"timeoutMs": 2000});   // suspends the handler,
    $res.json({"answered": !$r.timeout});                           // not the worker
}, {"suspend": true});
```

See [Suspending handlers](#suspending-handlers), including the atomicity note.

**UDP gives no delivery guarantee.** Packets can be lost, duplicated, or arrive out of order, and
`send_to` returning a byte count means the packet left this machine — nothing more. Anything that
needs reliability has to build it on top, or use TCP.

**`from` is unauthenticated.** A UDP source address is trivially spoofed. Never use `$pkt.from` as
proof of who sent something, and be careful about replying to it in a way that could amplify traffic
towards a victim.

## Databases

```bantu
sua.sqlite.open(path)
sua.sqlite.exec(sql, params?)      // parameterised with ?
sua.sqlite.query(sql, params?)
sua.sqlite.tables()
sua.sqlite.close()
```

`sua.postgres.*` needs a build with `-DBANTU_POSTGRES=ON`. For a higher-level interface see
[orm.md](orm.md); for analytics see [arctic.md](arctic.md).

> `sua.mysql.*` is a **simulation** — it returns canned rows regardless of build flags. Do not use it.

---

## Known limitations

These are real constraints of the current implementation, not oversights to work around silently.

- **An unmarked handler stalls its worker while it waits.** Each worker is one event loop on one
  thread, so a slow handler delays the other clients *on that worker*, and a handler that makes an
  HTTP request to its own worker deadlocks. Mark the route `{"suspend": true}` and neither happens —
  see [Suspending handlers](#suspending-handlers), and read the atomicity note there first.
  `sua.server.workers(n)` reduces the blast radius to 1/n either way.
- **`sua.sqlite.*` never suspends**, even on a marked route: sua has a single process-wide
  connection, and offloading statements onto it would make `lastInsertId` and `changes` report
  another handler's results. A slow query still stalls the worker.
  [sua-async-design.md](sua-async-design.md) §9.2 has the detail and what the real fix would be.
- **UDP sockets are never closed for you.** There are no finalisers in the language, so a socket you
  do not `sua.udp.close()` stays open until the process exits. See [UDP](#udp--suaudp).
- **`sua.server.use()` registers middleware that never runs.** The dispatch loop contains no
  middleware step. Put shared logic in a function your handlers call.
- **`sua.response.*` does not work.** It writes to globals the server never reads, and
  `sua.response.set`/`cookie` only print. Use the `$res` argument your handler receives.
- **No cookie or session helper.** Set them by hand with `$res.set("Set-Cookie", …)`, one per
  response.
- **Request headers are capped at 64 KB** (configurable via `sua.server.limits`). A larger header
  block gets `431 Request Header Fields Too Large` — it is no longer silently truncated.
- **No chunked transfer-encoding** on requests — only `Content-Length` is honoured.
- **Unrecognised status codes render as `OK`** in the status line, though the numeric code is
  correct.
- **No SSE.** WebSockets *are* supported (`sua.ws.*`, RFC 6455: masking enforced, continuation
  frames reassembled, UTF-8 validated, `Origin` checked on upgrade). The dead implementation in
  `server.hpp` is unrelated and still unused.
- Push notifications are **UTC only** and assume a single record per message.
