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
sua.server.get(path, handler)
sua.server.post(path, handler)
sua.server.put(path, handler)
sua.server.delete(path, handler)
sua.server.patch(path, handler)
sua.server.head(path, handler)
sua.server.options(path, handler)     // path defaults to "*"
sua.server.all(path, handler)         // GET, POST, PUT, DELETE, PATCH
sua.server.routes()                   // -> [{method, path}, ...]
sua.server.listen(port)               // blocks forever
```

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

**TLS certificates are verified.** Pass `"insecure": true` for a self-signed development endpoint —
and only then.

Bodies are length-explicit, so a body containing NUL bytes is transmitted intact.

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

- **The server is single-threaded.** Requests are handled one at a time on the accept loop. A slow
  handler blocks every other client, and a handler that makes an HTTP request *to its own server*
  deadlocks. `sua.push.send_all()` blocks for the duration of the fan-out; for large subscriber
  lists, send from a separate process.
- **`sua.server.use()` registers middleware that never runs.** The dispatch loop contains no
  middleware step. Put shared logic in a function your handlers call.
- **`sua.response.*` does not work.** It writes to globals the server never reads, and
  `sua.response.set`/`cookie` only print. Use the `$res` argument your handler receives.
- **No cookie or session helper.** Set them by hand with `$res.set("Set-Cookie", …)`, one per
  response.
- **Request headers are capped at 16 KB** (a single `recv`); a larger header block is truncated.
- **No chunked transfer-encoding** on requests — only `Content-Length` is honoured.
- **Unrecognised status codes render as `OK`** in the status line, though the numeric code is
  correct.
- **No WebSocket or SSE.** `server.hpp` contains a WebSocket implementation that is never
  instantiated and whose handshake is not spec-compliant.
- Push notifications are **UTC only** and assume a single record per message.
