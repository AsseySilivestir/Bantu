# Progressive Web Apps in `sua` — research and reference design

This is the design note behind sua's PWA support. It records what Python's **django-pwa** actually
does, what it deliberately leaves out, where **django-webpush** picks up, and how each piece maps
onto Bantu. Read this before changing `pwa_native.hpp`, `webpush.hpp`, `p256.hpp` or `aes_gcm.hpp`.

---

## 1. What a PWA actually requires

A "progressive web app" is a normal web app that satisfies three browser-checkable conditions:

| Requirement | Why | Who serves it |
|---|---|---|
| A **web app manifest** linked from `<head>` | Names the app, supplies icons, and declares `display: standalone` so the browser drops its own chrome | server |
| A registered **service worker** with a `fetch` handler | A background script that can answer requests offline. Without a `fetch` handler the browser will not offer installation | server + browser |
| **HTTPS** (or `localhost`) | Service workers are a powerful primitive; browsers gate them on a secure context | deployment |

Meet all three and the browser offers "Add to Home Screen" / "Install". That is the entire bar.
Everything else — offline pages, push, background sync — is built on the service worker.

## 2. django-pwa: what it is

`django-pwa` is small. It is an app you add to `INSTALLED_APPS` that contributes three URLs and one
template tag. There is no model, no middleware, no JavaScript build step.

### 2.1 Configuration is flat settings

Everything is a module-level `PWA_APP_*` setting read at request time:

```python
PWA_APP_NAME = 'My App'
PWA_APP_DESCRIPTION = "My app description"
PWA_APP_THEME_COLOR = '#0A0302'
PWA_APP_BACKGROUND_COLOR = '#ffffff'
PWA_APP_DISPLAY = 'standalone'
PWA_APP_SCOPE = '/'
PWA_APP_ORIENTATION = 'any'
PWA_APP_START_URL = '/'
PWA_APP_STATUS_BAR_COLOR = 'default'
PWA_APP_ICONS = [{'src': '/static/images/icon-192.png', 'sizes': '192x192'}]
PWA_APP_ICONS_APPLE = [{'src': '/static/images/icon-192.png', 'sizes': '192x192'}]
PWA_APP_SPLASH_SCREEN = [{'src': '/static/images/splash-640x1136.png',
                          'media': '(device-width: 320px) and (device-height: 568px)'}]
PWA_APP_SCREENSHOTS = [{'src': '/static/images/splash.png', 'sizes': '750x1334', 'type': 'image/png'}]
PWA_APP_SHORTCUTS = [{'name': 'Shortcut', 'url': '/target', 'description': '...'}]
PWA_APP_DIR = 'ltr'
PWA_APP_LANG = 'en-US'
PWA_APP_DEBUG_MODE = True
PWA_SERVICE_WORKER_PATH = os.path.join(BASE_DIR, 'my_app', 'serviceworker.js')
```

Every one is optional and has a default. This flatness is the package's best idea: a developer
configures a PWA without learning a new object model.

### 2.2 Three URLs

`url('', include('pwa.urls'))` — mounted at the **root**, deliberately:

| URL | Serves |
|---|---|
| `/manifest.json` | the manifest, rendered from the settings above |
| `/serviceworker.js` | the service worker |
| `/offline` | the fallback page shown when the network is gone |

The root mount is not cosmetic. **A service worker's default scope is the directory it is served
from.** A worker at `/static/js/sw.js` can only control `/static/js/*`; served from `/serviceworker.js`
it controls the whole origin. Getting this wrong is the single most common PWA bug.

### 2.3 The template tag

```django
{% load pwa %}
<head>{% progressive_web_app_meta %}</head>
```

It emits `<link rel="manifest">`, the theme-color and Apple-specific meta tags, the apple-touch-icon
links, the splash-screen `<link>`s, and an inline script that calls
`navigator.serviceWorker.register('/serviceworker.js')`. Injection is **explicit** — the package
never rewrites responses behind your back.

### 2.4 The default service worker

Roughly: on `install`, open a named cache and `addAll()` a small asset list; on `fetch`, try the
cache, fall back to the network, and if that throws, return the cached `/offline` page. Override it
wholesale with `PWA_SERVICE_WORKER_PATH`.

### 2.5 What django-pwa does NOT do

**It has no push notifications.** No VAPID keys, no subscription storage, no `push` event handler.
The README's feature list stops at manifest + service worker + offline. This is worth stating plainly
because it is widely assumed otherwise.

## 3. django-webpush: the other half

Push in Django comes from a separate package, `django-webpush`, wrapping `pywebpush`.

```python
WEBPUSH_SETTINGS = {
    "VAPID_PUBLIC_KEY": "...", "VAPID_PRIVATE_KEY": "...",
    "VAPID_ADMIN_EMAIL": "admin@example.com",
}
```

- `python manage.py webpush_generate_vapid_keypair` — generates a P-256 keypair.
- A `PushInformation`/`SubscriptionInfo` model pair stores each browser subscription
  (`endpoint`, `p256dh`, `auth`) against a user or a named group.
- `{% webpush_header %}` and `{% webpush_button %}` emit the client JS that calls
  `PushManager.subscribe()` and POSTs the resulting subscription to `/webpush/save_information`.
- `send_user_notification(user=..., payload={"head": ..., "body": ...}, ttl=...)` encrypts and POSTs
  to each stored endpoint.

**The important structural lesson:** the browser gives you a subscription object; the server stores
it; sending is an encrypted, signed HTTP POST to the endpoint *in that object*. Your server never
talks to Google or Mozilla directly by name — the endpoint URL tells you where to go.

## 4. Web Push on the wire — what a send actually is

This is the part that has to be built from scratch in Bantu, so it is specified here precisely.

A browser subscription is three values:

```json
{ "endpoint": "https://fcm.googleapis.com/fcm/send/xyz...",
  "keys": { "p256dh": "<base64url 65-byte P-256 public key>",
            "auth":   "<base64url 16-byte secret>" } }
```

Sending one message is:

```
POST <endpoint>
Authorization: vapid t=<JWT>, k=<base64url(VAPID public key)>
Content-Encoding: aes128gcm
Content-Type: application/octet-stream
TTL: 86400

<binary body>
```

### 4.1 The body — RFC 8291 over RFC 8188

Content coding header (RFC 8188 §2.1), 86 octets for Web Push:

```
salt (16, fresh random) || rs (4, big-endian; 4096) || idlen (1; = 65) || keyid (65 = as_public)
```

Key derivation (RFC 8291 §3.4). Note the argument order — the auth secret is the **HMAC key**:

```
ecdh_secret = ECDH(as_private, ua_public)                       # 32 bytes, X coordinate only
key_info    = "WebPush: info" || 0x00 || ua_public || as_public # 13 + 1 + 65 + 65 = 144 bytes
PRK_key     = HMAC-SHA256(auth_secret, ecdh_secret)
IKM         = HMAC-SHA256(PRK_key, key_info || 0x01)

PRK   = HMAC-SHA256(salt, IKM)
CEK   = HMAC-SHA256(PRK, "Content-Encoding: aes128gcm" || 0x00 || 0x01)[0..15]
NONCE = HMAC-SHA256(PRK, "Content-Encoding: nonce"     || 0x00 || 0x01)[0..11]
```

Record and body:

```
record_plaintext = payload || 0x02              # 0x02 = last record (0x01 = a non-final one)
body             = header || AES-128-GCM(CEK, NONCE, aad = <empty>, record_plaintext)
```

Size budget — push services reject bodies over 4096 octets:

```
max payload = 4096 - 86 (header) - 1 (delimiter) - 16 (tag) = 3993 octets
```

**A fresh ephemeral keypair AND a fresh salt are mandatory per message** (RFC 8291 §2). Reusing
either reuses `(CEK, NONCE)`, which is a total break of the AEAD. Our API therefore accepts neither
as a parameter — they are generated internally and cannot be supplied from Bantu.

### 4.2 The VAPID header — RFC 8292

```
header  = {"typ":"JWT","alg":"ES256"}
claims  = {"aud": <ORIGIN of the endpoint>, "exp": <unix seconds, <= now + 24h>, "sub": "mailto:..."}
signing_input = base64url(header) || "." || base64url(claims)
signature     = r || s                       # each left-padded to exactly 32 bytes; RAW, not DER
jwt           = signing_input || "." || base64url(signature)
```

Three traps, all of which produce a bare `401` with no useful diagnostic:

- **`aud` is the origin, not the endpoint URL.** `https://fcm.googleapis.com`, not
  `https://fcm.googleapis.com/fcm/send/xyz`. Mozilla's autopush tolerates the full URL; FCM does
  not — so this bug ships green from Firefox testing.
- **ES256 is raw `r || s`, 64 bytes** (RFC 7518 §3.4), *not* the DER `SEQUENCE` that OpenSSL emits.
  Each half must be zero-padded to 32 bytes; a minimal big-integer encoding drops a leading zero
  byte about 0.8% of the time, so this fails intermittently in production.
- **`exp` is in seconds**, and no more than 24 hours out.

All base64url in the JWT is URL-alphabet and **unpadded**.

### 4.3 Response codes worth handling

| Code | Meaning | Action |
|---|---|---|
| 201 | accepted | done |
| 400 | malformed request | bug — check headers and body framing |
| 401 / 403 | bad VAPID | check `aud`, `exp`, `sub`, and the signature encoding |
| 404 / 410 | subscription is dead | **delete it from storage** |
| 413 | body too large | payload exceeded 3993 octets |
| 429 | rate limited | back off, honour `Retry-After` |

## 5. Mapping onto sua

`sua` is not a `.b` package — it is a native namespace built in C++ (`evaluator.hpp`), so PWA support
belongs there too, as `sua.pwa.*` and `sua.push.*`.

| django | sua |
|---|---|
| `PWA_APP_*` settings | `sua.pwa.configure({...})` — one dict, same key names minus the prefix |
| `url('', include('pwa.urls'))` | implicit: `configure()` registers the routes itself |
| `/manifest.json` | `/manifest.json` **and** `/manifest.webmanifest` |
| `/serviceworker.js` | same path, so scope stays `/`; served `no-cache` |
| `/offline` | `public/offline.html` if present, else a built-in page |
| `{% progressive_web_app_meta %}` | `sua.pwa.meta()` → HTML string; plus opt-in `auto_inject` |
| `PWA_SERVICE_WORKER_PATH` | `service_worker: "./public/sw.js"` |
| `{% webpush_button %}` + save-subscription view | `/pwa.js` client helper + `sua.push.endpoint(path)` |
| `SubscriptionInfo` model | a `push_subscriptions` sqlite table |
| `send_user_notification()` | `sua.push.send()` / `sua.push.send_all()` |
| `webpush_generate_vapid_keypair` | `sua.push.vapid_keys()` |

### 5.1 Why the crypto is written from scratch

`crypto-suite/DECISIONS.md` D6 and D11 say encryption primitives come from libsodium and are never
hand-rolled. This work is a deliberate, documented exception (see D14), for one reason:

**libsodium has no P-256.** It offers Curve25519 and Ed25519 only. Web Push mandates P-256
(RFC 8291 §3.1). So a from-scratch P-256 is unavoidable unless we link OpenSSL — which would make
libcrypto a hard runtime dependency of every Bantu binary, the exact thing D11 exists to prevent.
Once P-256 must be written by hand (the hard part), AES-128-GCM is a small marginal addition and is
completely pinned by published vectors.

Bantu's float64-only numbers make a pure-Bantu fallback impossible here — the same reason SHA-512 is
native-only. So this code is compiled unconditionally rather than behind a build flag: there would be
nothing to fall back to.

The mitigations that come with that exception:

- It is reachable **only** through `webpush_*`. It is not re-exported as general-purpose crypto.
- Complete (exception-free) point formulas, so there are no `P == Q` or point-at-infinity special
  cases to get wrong on a sparse, input-dependent set of scalars.
- No secret-dependent branches and no secret-dependent memory indices anywhere: constant-time
  `cmov` in the ladder, Fermat inversion over a public exponent, and a table-free GHASH.
- Deterministic nonces (RFC 6979), so a signature is byte-comparable against published vectors and
  an RNG failure cannot repeat a nonce and leak the VAPID key.
- Subscriber public keys are validated on import (on-curve and in-range) — the invalid-curve attack
  is live here because `p256dh` is attacker-supplied.
- A known-answer selftest runs once, memoised, on first use of any `webpush_*` builtin and **fails
  closed**, so a miscompiled binary can never silently send a broken or insecure push.

The threat model is a remote attacker against a long-lived server-side VAPID key — not a co-resident
attacker with cache observation. Scalar blinding and bitsliced AES are deliberately out of scope, and
that trade-off is stated in the headers.

### 5.2 Transport gaps that had to be closed first

`bantuHttpRequest` could not send a push at all:

1. `CURLOPT_POSTFIELDS` was set with no `CURLOPT_POSTFIELDSIZE`, so libcurl called `strlen()` on the
   body. An `aes128gcm` body starts with 16 random bytes — roughly two in five would have been
   silently truncated at the first `0x00`.
2. No way to set arbitrary request headers, so `Authorization`, `TTL` and `Content-Encoding` were
   unreachable.
3. `CURLOPT_SSL_VERIFYPEER 0` — certificates were not verified.
4. Every call printed `[HTTP] <method> <url>` to stdout, which would have logged subscriber endpoints.

All four are fixed, and `sua.http.request(opts)` exposes headers and binary bodies generally — not
just for push. Specifically:

- `CURLOPT_POSTFIELDSIZE_LARGE` is set **before** `CURLOPT_COPYPOSTFIELDS`. That order is
  load-bearing: libcurl documents that without a prior size "the data is assumed to be a
  null-terminated string", which is exactly the truncation bug.
- Verification is on by default. `sua.http.request()` takes a per-request `"insecure": true`; the
  six convenience helpers, which have no options object, share the global `sua.http.insecure(true)`
  toggle so an app on a self-signed internal endpoint still has a way forward. A verification
  failure now returns an error that names both options instead of curl's bare string.
- The trace moved to stderr, honours `bantu -q`, and names only the **origin** — the path and query
  carry API tokens, and a push endpoint's path *is* the subscription id. `BANTU_HTTP_DEBUG=1` opts
  back in to the full URL.

### 5.3 Where the MIME table lives

Extension → Content-Type is `bantu-src/compiler/src/mime_types.hpp` (`bantu_mime::for_path`), not the
PWA header — only the static file server consumes it, and it is not a PWA concern. The original
inline table covered nine extensions; anything else fell through to `application/octet-stream`, which
browsers refuse to execute or render. It now covers `.webmanifest`, every common font, WebP/AVIF,
WASM, source maps and media.

Caching is deliberately conservative. A manifest is always revalidated, because a stale one pins an
old `start_url` or icon set. HTML only switches to `no-cache` **once `sua.pwa.configure()` has been
called** — there a stale shell pins old asset URLs and the service worker makes it sticky. An app
that never configures a PWA keeps the previous `max-age=300` exactly, so this cannot change
behaviour it relies on.

## 6. Test-vector gates

Each layer is pinned independently so a failure localises to one primitive.

| Layer | Vector source |
|---|---|
| HKDF-SHA256 | RFC 5869 §A.1–A.3 |
| Field arithmetic | `a·a⁻¹ == 1`; operands near p; `__int128` path vs portable path agree |
| Point arithmetic | `[n]G == O`, `[n−1]G == −G`, `[2]G == G+G`; projective ladder vs an independent affine reference |
| ECDH | RFC 5903 §8.1; plus `ECDH(a, [b]G) == ECDH(b, [a]G)` |
| ECDSA sign | **RFC 6979 §A.2.5** — exact `k`, `r`, `s`. Its `"test"` case has `s = 019F41…`, the leading-zero-byte case that pins 32-byte padding |
| ECDSA verify | round-trip, plus rejection of out-of-range `r`/`s` and tampered messages |
| AES-128 | FIPS 197 Appendix B |
| AES-128-GCM | McGrew–Viega Appendix B cases 1–4; cases 5–6 (non-96-bit IV) as rejection tests |
| RFC 8188 framing | §3.1 single record (byte-exact body), §3.2 multi-record (decrypt) |
| RFC 8291 | §5 + Appendix A — byte-exact body, every intermediate asserted individually |
| VAPID | self-verify, `aud` extraction unit tests, and a pinned JWT cross-verified once against an independent implementation |

## 7. Deliberate limitations

- **UTC only.** No timezone database; `exp` is computed from epoch seconds.
- **Single record.** We only ever emit one `aes128gcm` record, so payloads cap at 3993 octets.
  Multi-record is implemented on the *decrypt* side only, to run the RFC 8188 §3.2 vector.

  A transcription note for anyone re-checking the vectors: RFC 8188 §3.1 says "54-octet content
  body" and RFC 8291 §5 sends `Content-Length: 145`, but the published base64url values decode to
  **53** and **144** octets respectively. The encoded values are authoritative and are what the
  selftest asserts; the octet counts in the prose are editorial slips.
- **The server is single-threaded** (`bantuStartHttpServer`, evaluator.hpp). `sua.push.send_all()`
  blocks the accept loop for the duration of the fan-out. For large subscriber lists, send from a
  separate process.
- **No background sync, no periodic sync, no Badging API.** The service worker handles `install`,
  `activate`, `fetch`, `push` and `notificationclick` only.

## 8. Sources

- django-pwa — <https://github.com/silviolleite/django-pwa>
- django-webpush — <https://github.com/safwanrahman/django-webpush>
- RFC 8030 — Generic Event Delivery Using HTTP Push
- RFC 8188 — Encrypted Content-Encoding for HTTP (`aes128gcm`)
- RFC 8291 — Message Encryption for Web Push
- RFC 8292 — VAPID for Web Push
- RFC 6979 — Deterministic ECDSA
- RFC 7515 / RFC 7518 — JWS and JWA (ES256)
- FIPS 186-4 (P-256), FIPS 197 (AES), NIST SP 800-38D (GCM)
- Renes, Costello, Batina — *Complete addition formulas for prime order elliptic curves*, EUROCRYPT 2015
