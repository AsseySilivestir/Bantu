#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  pwa_native.hpp — Progressive Web App support for the sua web framework.
//
//  Modelled on Python's django-pwa (see docs/pwa-research.md), which contributes
//  three URLs and one template tag to a Django project. The same three URLs are
//  auto-registered here by `sua.pwa.configure()`:
//
//      /manifest.json  (+ /manifest.webmanifest)   the web app manifest
//      /serviceworker.js                           the generated service worker
//      /offline                                    the offline fallback page
//      /pwa.js                                     the client helper (our addition)
//
//  WHY THE SERVICE WORKER IS SERVED FROM THE ROOT
//  ----------------------------------------------
//  A service worker's default scope is the directory it is served from. At
//  /static/js/sw.js it could only control /static/js/*; at /serviceworker.js it
//  controls the whole origin. django-pwa mounts its URLs at the root for exactly
//  this reason, and getting it wrong is the most common PWA bug.
//
//  This file only RENDERS text. Route registration, config parsing and request
//  handling live in evaluator.hpp beside the rest of sua.
// ════════════════════════════════════════════════════════════════════════════

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>
#include <utility>

namespace bantu_pwa {

struct IconEntry {
    std::string src;
    std::string sizes;
    std::string type;
    std::string media;      // splash screens only
};

struct Config {
    bool configured = false;

    std::string name             = "Bantu App";
    std::string short_name;                        // defaults to `name`
    std::string description;
    std::string theme_color      = "#000000";
    std::string background_color = "#ffffff";
    std::string display          = "standalone";
    std::string scope            = "/";
    std::string start_url        = "/";
    std::string orientation      = "any";
    std::string lang             = "en-US";
    std::string dir              = "ltr";
    std::string status_bar_color = "default";
    std::string offline_url      = "/offline";

    std::string service_worker;                    // path to a custom SW file
    std::string cache_version    = "v1";
    bool debug       = false;
    bool auto_inject = true;                       // patch <head> of static HTML
    bool auto_register = true;                     // register the SW from /pwa.js

    std::vector<IconEntry> icons;
    std::vector<IconEntry> icons_apple;
    std::vector<IconEntry> splash_screen;

    // Pre-rendered JSON array fragments for the manifest's pass-through keys,
    // built in evaluator.hpp where Value is available. Passing the developer's
    // JSON straight through preserves keys we don't model (icon `purpose`,
    // shortcut `icons`, and anything the spec adds later).
    std::string icons_json       = "";
    std::string screenshots_json = "";
    std::string shortcuts_json   = "";
    std::string categories_json  = "";

    std::vector<std::string> precache;

    // Push (populated by sua.push.configure); empty disables the push helpers.
    std::string vapid_public_key;
    std::string subscribe_url = "/pwa/subscribe";
};

// ── escaping ────────────────────────────────────────────────────────────────

inline std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

inline std::string html_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&':  o += "&amp;";  break;
            case '<':  o += "&lt;";   break;
            case '>':  o += "&gt;";   break;
            case '"':  o += "&quot;"; break;
            case '\'': o += "&#39;";  break;
            default:   o += c;
        }
    }
    return o;
}

// A JS single-quoted string literal.
inline std::string js_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\'' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '<')  o += "\\x3c";      // never let a literal close a <script>
        else o += c;
    }
    return o;
}

// ── MIME types ──────────────────────────────────────────────────────────────
//
// Shared by the static file server. The original table covered nine extensions
// and lacked `.webmanifest` (required for a manifest served as a file) and the
// font/media types any real app needs.
inline std::string mime_for_extension(const std::string& ext) {
    if (ext == "html" || ext == "htm")   return "text/html; charset=utf-8";
    if (ext == "css")                    return "text/css; charset=utf-8";
    if (ext == "js" || ext == "mjs")     return "application/javascript; charset=utf-8";
    if (ext == "json" || ext == "map")   return "application/json; charset=utf-8";
    if (ext == "webmanifest")            return "application/manifest+json; charset=utf-8";
    if (ext == "svg")                    return "image/svg+xml";
    if (ext == "png")                    return "image/png";
    if (ext == "jpg" || ext == "jpeg")   return "image/jpeg";
    if (ext == "gif")                    return "image/gif";
    if (ext == "webp")                   return "image/webp";
    if (ext == "avif")                   return "image/avif";
    if (ext == "ico")                    return "image/x-icon";
    if (ext == "txt")                    return "text/plain; charset=utf-8";
    if (ext == "xml")                    return "application/xml; charset=utf-8";
    if (ext == "pdf")                    return "application/pdf";
    if (ext == "wasm")                   return "application/wasm";
    if (ext == "woff")                   return "font/woff";
    if (ext == "woff2")                  return "font/woff2";
    if (ext == "ttf")                    return "font/ttf";
    if (ext == "otf")                    return "font/otf";
    if (ext == "eot")                    return "application/vnd.ms-fontobject";
    if (ext == "mp4")                    return "video/mp4";
    if (ext == "webm")                   return "video/webm";
    if (ext == "mp3")                    return "audio/mpeg";
    if (ext == "ogg")                    return "audio/ogg";
    if (ext == "wav")                    return "audio/wav";
    if (ext == "csv")                    return "text/csv; charset=utf-8";
    return "application/octet-stream";
}

// ── the manifest ────────────────────────────────────────────────────────────

inline std::string render_icon_array(const std::vector<IconEntry>& icons, bool with_type) {
    if (icons.empty()) return "[]";
    std::string o = "[";
    for (size_t i = 0; i < icons.size(); i++) {
        if (i) o += ",";
        o += "{\"src\":\"" + json_escape(icons[i].src) + "\"";
        if (!icons[i].sizes.empty()) o += ",\"sizes\":\"" + json_escape(icons[i].sizes) + "\"";
        if (with_type && !icons[i].type.empty()) o += ",\"type\":\"" + json_escape(icons[i].type) + "\"";
        o += "}";
    }
    o += "]";
    return o;
}

inline std::string render_manifest(const Config& c) {
    std::string sn = c.short_name.empty() ? c.name : c.short_name;
    std::string o = "{\n";
    o += "  \"name\": \""             + json_escape(c.name) + "\",\n";
    o += "  \"short_name\": \""       + json_escape(sn) + "\",\n";
    if (!c.description.empty())
        o += "  \"description\": \""  + json_escape(c.description) + "\",\n";
    o += "  \"start_url\": \""        + json_escape(c.start_url) + "\",\n";
    o += "  \"scope\": \""            + json_escape(c.scope) + "\",\n";
    o += "  \"display\": \""          + json_escape(c.display) + "\",\n";
    o += "  \"orientation\": \""      + json_escape(c.orientation) + "\",\n";
    o += "  \"theme_color\": \""      + json_escape(c.theme_color) + "\",\n";
    o += "  \"background_color\": \"" + json_escape(c.background_color) + "\",\n";
    o += "  \"lang\": \""             + json_escape(c.lang) + "\",\n";
    o += "  \"dir\": \""              + json_escape(c.dir) + "\",\n";
    if (!c.categories_json.empty() && c.categories_json != "[]")
        o += "  \"categories\": "     + c.categories_json + ",\n";
    if (!c.screenshots_json.empty() && c.screenshots_json != "[]")
        o += "  \"screenshots\": "    + c.screenshots_json + ",\n";
    if (!c.shortcuts_json.empty() && c.shortcuts_json != "[]")
        o += "  \"shortcuts\": "      + c.shortcuts_json + ",\n";
    o += "  \"icons\": " + (c.icons_json.empty() ? render_icon_array(c.icons, true) : c.icons_json) + "\n";
    o += "}\n";
    return o;
}

// ── the service worker ──────────────────────────────────────────────────────

inline std::string render_service_worker(const Config& c) {
    std::string cache = "bantu-pwa-" + c.cache_version;
    std::string icon  = c.icons.empty() ? "" : c.icons.front().src;

    std::string precache = "[";
    precache += "'" + js_escape(c.offline_url) + "'";
    for (const auto& p : c.precache) precache += ", '" + js_escape(p) + "'";
    precache += "]";

    std::string o;
    o += "// Generated by sua.pwa — do not edit by hand.\n";
    o += "// Override with:  sua.pwa.configure({ \"service_worker\": \"./public/sw.js\" })\n";
    o += "'use strict';\n\n";
    o += "const CACHE_NAME  = '" + js_escape(cache) + "';\n";
    o += "const OFFLINE_URL = '" + js_escape(c.offline_url) + "';\n";
    o += "const APP_NAME    = '" + js_escape(c.name) + "';\n";
    o += "const APP_ICON    = '" + js_escape(icon) + "';\n";
    o += "const PRECACHE    = " + precache + ";\n";
    o += "const DEBUG       = " + std::string(c.debug ? "true" : "false") + ";\n";
    o += "const log = (...a) => { if (DEBUG) console.log('[sua.pwa]', ...a); };\n\n";

    o += "// Install: warm the cache, then take over immediately.\n";
    o += "self.addEventListener('install', (event) => {\n";
    o += "  event.waitUntil((async () => {\n";
    o += "    const cache = await caches.open(CACHE_NAME);\n";
    o += "    // Add individually so one 404 cannot fail the whole install.\n";
    o += "    await Promise.all(PRECACHE.map(async (url) => {\n";
    o += "      try { await cache.add(new Request(url, { cache: 'reload' })); }\n";
    o += "      catch (e) { log('precache skipped', url, e); }\n";
    o += "    }));\n";
    o += "    await self.skipWaiting();\n";
    o += "  })());\n";
    o += "});\n\n";

    o += "// Activate: drop caches from older versions.\n";
    o += "self.addEventListener('activate', (event) => {\n";
    o += "  event.waitUntil((async () => {\n";
    o += "    const keys = await caches.keys();\n";
    o += "    await Promise.all(keys.map((k) => k === CACHE_NAME ? null : caches.delete(k)));\n";
    o += "    await self.clients.claim();\n";
    o += "    log('activated', CACHE_NAME);\n";
    o += "  })());\n";
    o += "});\n\n";

    o += "self.addEventListener('fetch', (event) => {\n";
    o += "  const req = event.request;\n";
    o += "  if (req.method !== 'GET') return;\n";
    o += "  let url;\n";
    o += "  try { url = new URL(req.url); } catch (e) { return; }\n";
    o += "  if (url.origin !== self.location.origin) return;   // never cache cross-origin\n\n";

    o += "  // Navigations: network first (so content stays fresh), then cache,\n";
    o += "  // then the offline page. This is what makes the app usable offline.\n";
    o += "  if (req.mode === 'navigate') {\n";
    o += "    event.respondWith((async () => {\n";
    o += "      try {\n";
    o += "        const fresh = await fetch(req);\n";
    o += "        const cache = await caches.open(CACHE_NAME);\n";
    o += "        cache.put(req, fresh.clone());\n";
    o += "        return fresh;\n";
    o += "      } catch (e) {\n";
    o += "        return (await caches.match(req))\n";
    o += "            || (await caches.match(OFFLINE_URL))\n";
    o += "            || new Response('Offline', { status: 503, headers: { 'Content-Type': 'text/plain' } });\n";
    o += "      }\n";
    o += "    })());\n";
    o += "    return;\n";
    o += "  }\n\n";

    o += "  // Assets: cache first, fall back to the network.\n";
    o += "  event.respondWith((async () => {\n";
    o += "    const cached = await caches.match(req);\n";
    o += "    if (cached) return cached;\n";
    o += "    try {\n";
    o += "      const fresh = await fetch(req);\n";
    o += "      if (fresh && fresh.status === 200 && fresh.type === 'basic') {\n";
    o += "        const cache = await caches.open(CACHE_NAME);\n";
    o += "        cache.put(req, fresh.clone());\n";
    o += "      }\n";
    o += "      return fresh;\n";
    o += "    } catch (e) {\n";
    o += "      return (await caches.match(OFFLINE_URL))\n";
    o += "          || new Response('Offline', { status: 503, headers: { 'Content-Type': 'text/plain' } });\n";
    o += "    }\n";
    o += "  })());\n";
    o += "});\n\n";

    o += "// ── Push ──\n";
    o += "// Payload shape (matches django-webpush): { head, body, icon, badge, tag, url }\n";
    o += "self.addEventListener('push', (event) => {\n";
    o += "  let data = {};\n";
    o += "  if (event.data) {\n";
    o += "    try { data = event.data.json(); }\n";
    o += "    catch (e) { data = { body: event.data.text() }; }\n";
    o += "  }\n";
    o += "  const title = data.head || data.title || APP_NAME;\n";
    o += "  const options = {\n";
    o += "    body: data.body || '',\n";
    o += "    icon: data.icon || APP_ICON || undefined,\n";
    o += "    badge: data.badge || undefined,\n";
    o += "    tag: data.tag || undefined,\n";
    o += "    renotify: !!data.renotify,\n";
    o += "    requireInteraction: !!data.requireInteraction,\n";
    o += "    data: { url: data.url || '/' }\n";
    o += "  };\n";
    o += "  event.waitUntil(self.registration.showNotification(title, options));\n";
    o += "});\n\n";

    o += "self.addEventListener('notificationclick', (event) => {\n";
    o += "  event.notification.close();\n";
    o += "  const target = (event.notification.data && event.notification.data.url) || '/';\n";
    o += "  event.waitUntil((async () => {\n";
    o += "    const all = await clients.matchAll({ type: 'window', includeUncontrolled: true });\n";
    o += "    for (const c of all) {\n";
    o += "      if (c.url === target && 'focus' in c) return c.focus();\n";
    o += "    }\n";
    o += "    if (clients.openWindow) return clients.openWindow(target);\n";
    o += "  })());\n";
    o += "});\n";
    return o;
}

// ── the client helper (/pwa.js) ─────────────────────────────────────────────

inline std::string render_client_js(const Config& c) {
    std::string o;
    o += "// Generated by sua.pwa — service worker registration, install prompt, push.\n";
    o += "(function () {\n";
    o += "  'use strict';\n";
    o += "  const SW_URL   = '/serviceworker.js';\n";
    o += "  const SCOPE    = '" + js_escape(c.scope) + "';\n";
    o += "  const VAPID    = '" + js_escape(c.vapid_public_key) + "';\n";
    o += "  const SUB_URL  = '" + js_escape(c.subscribe_url) + "';\n";
    o += "  const DEBUG    = " + std::string(c.debug ? "true" : "false") + ";\n";
    o += "  const log = (...a) => { if (DEBUG) console.log('[sua.pwa]', ...a); };\n\n";

    o += "  let deferredPrompt = null;\n";
    o += "  const installCbs = [];\n\n";

    o += "  function b64ToUint8Array(base64) {\n";
    o += "    const pad = '='.repeat((4 - (base64.length % 4)) % 4);\n";
    o += "    const s = (base64 + pad).replace(/-/g, '+').replace(/_/g, '/');\n";
    o += "    const raw = atob(s);\n";
    o += "    const arr = new Uint8Array(raw.length);\n";
    o += "    for (let i = 0; i < raw.length; i++) arr[i] = raw.charCodeAt(i);\n";
    o += "    return arr;\n";
    o += "  }\n\n";

    o += "  const BantuPWA = {\n";
    o += "    registration: null,\n\n";

    o += "    supported() { return 'serviceWorker' in navigator; },\n\n";

    o += "    async register() {\n";
    o += "      if (!this.supported()) { log('service workers unsupported'); return null; }\n";
    o += "      try {\n";
    o += "        this.registration = await navigator.serviceWorker.register(SW_URL, { scope: SCOPE });\n";
    o += "        log('registered', this.registration.scope);\n";
    o += "        return this.registration;\n";
    o += "      } catch (e) { console.error('[sua.pwa] registration failed', e); return null; }\n";
    o += "    },\n\n";

    o += "    async ready() {\n";
    o += "      if (!this.supported()) return null;\n";
    o += "      this.registration = this.registration || await navigator.serviceWorker.ready;\n";
    o += "      return this.registration;\n";
    o += "    },\n\n";

    o += "    isInstalled() {\n";
    o += "      return window.matchMedia('(display-mode: standalone)').matches\n";
    o += "          || window.navigator.standalone === true;\n";
    o += "    },\n\n";

    o += "    canInstall() { return deferredPrompt !== null; },\n\n";

    o += "    onInstallPrompt(cb) {\n";
    o += "      installCbs.push(cb);\n";
    o += "      if (deferredPrompt) { try { cb(); } catch (e) {} }\n";
    o += "    },\n\n";

    o += "    async promptInstall() {\n";
    o += "      if (!deferredPrompt) return null;\n";
    o += "      deferredPrompt.prompt();\n";
    o += "      const choice = await deferredPrompt.userChoice;\n";
    o += "      deferredPrompt = null;\n";
    o += "      return choice.outcome;   // 'accepted' | 'dismissed'\n";
    o += "    },\n\n";

    o += "    pushSupported() {\n";
    o += "      return this.supported() && 'PushManager' in window && 'Notification' in window;\n";
    o += "    },\n\n";

    o += "    permission() { return ('Notification' in window) ? Notification.permission : 'unsupported'; },\n\n";

    o += "    async isSubscribed() {\n";
    o += "      if (!this.pushSupported()) return false;\n";
    o += "      const reg = await this.ready();\n";
    o += "      if (!reg) return false;\n";
    o += "      return !!(await reg.pushManager.getSubscription());\n";
    o += "    },\n\n";

    o += "    // Asks permission, subscribes with the server's VAPID key, and POSTs\n";
    o += "    // the subscription so the server can push to it later.\n";
    o += "    async subscribePush(extra) {\n";
    o += "      if (!this.pushSupported()) throw new Error('Push is not supported in this browser');\n";
    o += "      if (!VAPID) throw new Error('No VAPID public key configured — call sua.push.configure() on the server');\n";
    o += "      const reg = (await this.ready()) || (await this.register());\n";
    o += "      if (!reg) throw new Error('No service worker registration');\n";
    o += "      const perm = await Notification.requestPermission();\n";
    o += "      if (perm !== 'granted') throw new Error('Notification permission: ' + perm);\n";
    o += "      let sub = await reg.pushManager.getSubscription();\n";
    o += "      if (!sub) {\n";
    o += "        sub = await reg.pushManager.subscribe({\n";
    o += "          userVisibleOnly: true,\n";
    o += "          applicationServerKey: b64ToUint8Array(VAPID)\n";
    o += "        });\n";
    o += "      }\n";
    o += "      const payload = Object.assign({ subscription: sub.toJSON() }, extra || {});\n";
    o += "      const res = await fetch(SUB_URL, {\n";
    o += "        method: 'POST',\n";
    o += "        headers: { 'Content-Type': 'application/json' },\n";
    o += "        body: JSON.stringify(payload)\n";
    o += "      });\n";
    o += "      if (!res.ok) throw new Error('Subscribe failed: HTTP ' + res.status);\n";
    o += "      log('subscribed', sub.endpoint);\n";
    o += "      return sub.toJSON();\n";
    o += "    },\n\n";

    o += "    async unsubscribePush() {\n";
    o += "      const reg = await this.ready();\n";
    o += "      if (!reg) return false;\n";
    o += "      const sub = await reg.pushManager.getSubscription();\n";
    o += "      if (!sub) return false;\n";
    o += "      try {\n";
    o += "        await fetch(SUB_URL, {\n";
    o += "          method: 'DELETE',\n";
    o += "          headers: { 'Content-Type': 'application/json' },\n";
    o += "          body: JSON.stringify({ endpoint: sub.endpoint })\n";
    o += "        });\n";
    o += "      } catch (e) { log('server unsubscribe failed', e); }\n";
    o += "      return await sub.unsubscribe();\n";
    o += "    }\n";
    o += "  };\n\n";

    o += "  window.addEventListener('beforeinstallprompt', (e) => {\n";
    o += "    e.preventDefault();\n";
    o += "    deferredPrompt = e;\n";
    o += "    log('install prompt available');\n";
    o += "    installCbs.forEach((cb) => { try { cb(); } catch (err) {} });\n";
    o += "  });\n";
    o += "  window.addEventListener('appinstalled', () => { deferredPrompt = null; log('installed'); });\n\n";

    o += "  window.BantuPWA = BantuPWA;\n";
    if (c.auto_register) {
        o += "  if (document.readyState === 'complete') BantuPWA.register();\n";
        o += "  else window.addEventListener('load', () => BantuPWA.register());\n";
    }
    o += "})();\n";
    return o;
}

// ── the <head> block (django-pwa's {% progressive_web_app_meta %}) ──────────

inline std::string render_meta(const Config& c) {
    std::string sn = c.short_name.empty() ? c.name : c.short_name;
    std::string o;
    o += "<link rel=\"manifest\" href=\"/manifest.json\">\n";
    o += "<meta name=\"theme-color\" content=\"" + html_escape(c.theme_color) + "\">\n";
    o += "<meta name=\"application-name\" content=\"" + html_escape(sn) + "\">\n";
    if (!c.description.empty())
        o += "<meta name=\"description\" content=\"" + html_escape(c.description) + "\">\n";
    o += "<meta name=\"mobile-web-app-capable\" content=\"yes\">\n";
    o += "<meta name=\"apple-mobile-web-app-capable\" content=\"yes\">\n";
    o += "<meta name=\"apple-mobile-web-app-title\" content=\"" + html_escape(sn) + "\">\n";
    o += "<meta name=\"apple-mobile-web-app-status-bar-style\" content=\"" + html_escape(c.status_bar_color) + "\">\n";
    o += "<meta name=\"msapplication-TileColor\" content=\"" + html_escape(c.theme_color) + "\">\n";
    if (!c.icons.empty())
        o += "<meta name=\"msapplication-TileImage\" content=\"" + html_escape(c.icons.front().src) + "\">\n";

    const std::vector<IconEntry>& apple = c.icons_apple.empty() ? c.icons : c.icons_apple;
    for (const auto& i : apple) {
        o += "<link rel=\"apple-touch-icon\" href=\"" + html_escape(i.src) + "\"";
        if (!i.sizes.empty()) o += " sizes=\"" + html_escape(i.sizes) + "\"";
        o += ">\n";
    }
    for (const auto& s : c.splash_screen) {
        o += "<link rel=\"apple-touch-startup-image\" href=\"" + html_escape(s.src) + "\"";
        if (!s.media.empty()) o += " media=\"" + html_escape(s.media) + "\"";
        o += ">\n";
    }
    if (!c.icons.empty())
        o += "<link rel=\"icon\" href=\"" + html_escape(c.icons.front().src) + "\">\n";
    o += "<script src=\"/pwa.js\" defer></script>\n";
    return o;
}

// Does `needle` appear in `html` outside of any <!-- HTML comment -->?
//
// The idempotency check below must not be fooled by a page that merely MENTIONS
// the marker — a commented-out template, or documentation showing the tags. A
// plain substring search there silently strips the PWA tags from a page that
// looked fine in the source.
inline bool contains_outside_comments(const std::string& html, const std::string& needle) {
    size_t i = 0;
    while (i <= html.size()) {
        size_t open  = html.find("<!--", i);
        size_t limit = (open == std::string::npos) ? html.size() : open;
        size_t hit   = html.find(needle, i);
        if (hit != std::string::npos && hit < limit) return true;   // found before any comment
        if (open == std::string::npos) return false;
        size_t close = html.find("-->", open);
        if (close == std::string::npos) return false;               // unterminated comment
        i = close + 3;
    }
    return false;
}

// Insert the meta block into an HTML document's <head>.
//
// Used only when `auto_inject` is on. Idempotent: a document that already links
// the manifest (because it called sua.pwa.meta() itself) is left alone.
inline std::string inject_meta(const std::string& html, const std::string& meta) {
    if (contains_outside_comments(html, "rel=\"manifest\"")) return html;
    if (contains_outside_comments(html, "src=\"/pwa.js\"")) return html;

    // Prefer just before </head>; otherwise just after <head...>; otherwise skip.
    static const char* kClose = "</head>";
    size_t pos = html.find(kClose);
    if (pos == std::string::npos) {
        std::string upper = html;
        for (auto& ch : upper) ch = (char)toupper((unsigned char)ch);
        pos = upper.find("</HEAD>");
    }
    if (pos != std::string::npos) return html.substr(0, pos) + meta + html.substr(pos);

    size_t open = html.find("<head");
    if (open != std::string::npos) {
        size_t gt = html.find('>', open);
        if (gt != std::string::npos)
            return html.substr(0, gt + 1) + "\n" + meta + html.substr(gt + 1);
    }
    return html;   // not an HTML document we can safely patch
}

// ── the built-in offline page ───────────────────────────────────────────────
//
// Used when the app has no public/offline.html of its own. django-pwa ships an
// overridable offline.html template; this is the equivalent default.
inline std::string render_offline_page(const Config& c) {
    std::string sn = c.short_name.empty() ? c.name : c.short_name;
    std::string o;
    o += "<!doctype html>\n<html lang=\"" + html_escape(c.lang) + "\" dir=\"" + html_escape(c.dir) + "\">\n<head>\n";
    o += "<meta charset=\"utf-8\">\n";
    o += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    o += "<title>" + html_escape(sn) + " — offline</title>\n";
    o += render_meta(c);
    o += "<style>\n";
    o += "  :root { color-scheme: light dark; }\n";
    o += "  body { margin:0; min-height:100vh; display:grid; place-items:center;\n";
    o += "         font: 16px/1.5 system-ui, -apple-system, Segoe UI, Roboto, sans-serif;\n";
    o += "         background: " + html_escape(c.background_color) + "; color: #1a1a1a; }\n";
    o += "  @media (prefers-color-scheme: dark) { body { background:#111; color:#eee; } }\n";
    o += "  main { text-align:center; padding:2rem; max-width:32rem; }\n";
    o += "  h1 { font-size:1.5rem; margin:0 0 .5rem; }\n";
    o += "  p { opacity:.75; margin:0 0 1.5rem; }\n";
    o += "  button { font:inherit; padding:.6rem 1.4rem; border:0; border-radius:.5rem;\n";
    o += "           background: " + html_escape(c.theme_color) + "; color:#fff; cursor:pointer; }\n";
    o += "  .dot { width:.6rem; height:.6rem; border-radius:50%; background:#c0392b;\n";
    o += "         display:inline-block; margin-right:.4rem; }\n";
    o += "</style>\n</head>\n<body>\n<main>\n";
    o += "  <p><span class=\"dot\"></span>No connection</p>\n";
    o += "  <h1>You're offline</h1>\n";
    o += "  <p>" + html_escape(sn) + " can't reach the network right now. Pages you've already visited are still available.</p>\n";
    o += "  <button onclick=\"location.reload()\">Try again</button>\n";
    o += "</main>\n";
    o += "<script>addEventListener('online', () => location.reload());</script>\n";
    o += "</body>\n</html>\n";
    return o;
}

} // namespace bantu_pwa
