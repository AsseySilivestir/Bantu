// ════════════════════════════════════════════════════════════════════
//  init_templates.hpp — File templates for `bantu init --web`
//
//  Used by cmdInitWeb() in main.cpp. Each template returns the file
//  content with the project name substituted in.
//
//  Implementation note: we use a single raw-string literal per file
//  with a placeholder token (%%NAME%%), then string-replace it with
//  the actual project name. This avoids the "raw string splice"
//  pitfall where a literal )" in the content can prematurely close
//  a raw string.
// ════════════════════════════════════════════════════════════════════
#pragma once
#include <string>

namespace bantu_templates {

// Replace all occurrences of `from` with `to` in `s`, in place.
inline void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// ─── main.b — Sua web server starter ─────────────────────────────────
inline std::string main_b(const std::string& name) {
    std::string s = R"BANTU(// ════════════════════════════════════════════════════════════════════
//  %%NAME%% — Bantu + Sua web app
//  Scaffolded by `bantu init --web`
//
//  Run:    bantu run main.b
//  HTTP:   http://localhost:8080
//  DB:     ./app.db (or /data/app.db on Render)
// ════════════════════════════════════════════════════════════════════

print "═══════════════════════════════════════════";
print "  %%NAME%% — Bantu + Sua";
print "═══════════════════════════════════════════";

// ─── Config ──────────────────────────────────────────────────────────
string $envPort = env("PORT");
if (!$envPort) { $envPort = "8080"; }

// On Render, mount a persistent volume at /data. Locally, use ./app.db.
string $dbPath = "/data/app.db";
dict $probe = sua.sqlite.open($dbPath);
if (!$probe.connected) {
    $dbPath = "app.db";
    print "[INFO] /data not writable, using local: " + $dbPath;
} else {
    print "[INFO] Using persistent volume: " + $dbPath;
}

dict $conn = sua.sqlite.open($dbPath);
if (!$conn.connected) {
    print "[ERROR] Cannot open SQLite — aborting.";
    exit(1);
}
print "[OK] Connected to SQLite.";

// ─── Schema ──────────────────────────────────────────────────────────
sua.sqlite.exec("PRAGMA journal_mode=WAL;");
sua.sqlite.exec("PRAGMA foreign_keys=ON;");
sua.sqlite.exec("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, created_at TEXT DEFAULT (datetime('now')));");

// ─── Helpers ─────────────────────────────────────────────────────────

// Escape single quotes for safe SQL string literals.
def esc($s) {
    if (!$s) { return ""; }
    string $out = "";
    string $c = "";
    number $i = 0;
    while ($i < len($s)) {
        $c = $s[$i];
        if ($c == "'") {
            $out = $out + "''";
        } else {
            $out = $out + $c;
        }
        $i = $i + 1;
    }
    return $out;
}

// ─── Routes ──────────────────────────────────────────────────────────

// Health check — used by Render and the start scripts.
def handleHealth($req, $res) {
    $res.json({
        "status": "ok",
        "app":    "%%NAME%%",
        "version": "1.0.0",
        "time":   clock()
    });

}

// List all items.
def handleList($req, $res) {
    list $rows = sua.sqlite.query("SELECT * FROM items ORDER BY id DESC LIMIT 100;");
    $res.json({ "items": $rows });

}

// Create a new item. Body: {"name": "..."}.
def handleCreate($req, $res) {
    string $name = $req.body.name;
    if (!$name) {
        $res.status(400).json({ "error": "name is required" });
        return null;
    }
    sua.sqlite.exec("INSERT INTO items (name) VALUES ('" + esc($name) + "');");
    // No lastId() helper in sua.sqlite — just acknowledge.
    // The frontend refreshes the list after create, so the new id shows up there.
    $res.status(201).json({ "ok": true });

}

// Delete an item by id.
def handleDelete($req, $res) {
    number $id = num($req.params.id);
    sua.sqlite.exec("DELETE FROM items WHERE id = " + str($id) + ";");
    $res.json({ "ok": true });

}

sua.server.get("/api/health",          handleHealth);
sua.server.get("/api/items",           handleList);
sua.server.post("/api/items",          handleCreate);
sua.server.delete("/api/items/:id",    handleDelete);

// Static frontend
sua.server.static("./public");

// ─── Start ───────────────────────────────────────────────────────────
print "";
print "  Routes:";
print "    GET    /api/health";
print "    GET    /api/items";
print "    POST   /api/items        {\"name\":\"...\"}";
print "    DELETE /api/items/:id";
print "    GET    /                 (static frontend)";
print "";
print "  Listening on http://0.0.0.0:" + $envPort;
print "═══════════════════════════════════════════";

sua.server.listen(num($envPort));
)BANTU";
    replaceAll(s, "%%NAME%%", name);
    return s;
}

// ─── public/index.html ───────────────────────────────────────────────
inline std::string index_html(const std::string& name) {
    std::string s = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>%%NAME%%</title>
  <link rel="stylesheet" href="/css/style.css">
</head>
<body>
  <main>
    <h1>%%NAME%%</h1>
    <p class="subtitle">Scaffolded by <code>bantu init --web</code> &middot; Bantu + Sua + SQLite</p>

    <section class="card">
      <h2>Health</h2>
      <pre id="health">loading...</pre>
    </section>

    <section class="card">
      <h2>Create item</h2>
      <form id="createForm">
        <input type="text" name="name" placeholder="Item name" required autocomplete="off">
        <button type="submit">Add</button>
      </form>
    </section>

    <section class="card">
      <h2>Items <button id="refresh" small>Refresh</button></h2>
      <ul id="items"><li>loading...</li></ul>
    </section>
  </main>

  <script src="/js/app.js"></script>
</body>
</html>
)HTML";
    replaceAll(s, "%%NAME%%", name);
    return s;
}

// ─── public/css/style.css ────────────────────────────────────────────
inline std::string style_css() {
    return R"CSS(* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  background: #f4f4f9; color: #1a1a2e; line-height: 1.5; padding: 2rem;
}
main { max-width: 720px; margin: 0 auto; }
h1 { font-size: 2rem; margin-bottom: 0.25rem; }
.subtitle { color: #6b7280; margin-bottom: 2rem; font-size: 0.95rem; }
.subtitle code { background: #e5e7eb; padding: 0.1rem 0.35rem; border-radius: 3px; }
.card {
  background: #fff; border: 1px solid #e5e7eb; border-radius: 8px;
  padding: 1.25rem 1.5rem; margin-bottom: 1rem;
  box-shadow: 0 1px 2px rgba(0,0,0,0.04);
}
h2 { font-size: 1.1rem; margin-bottom: 0.75rem; display: flex; justify-content: space-between; align-items: center; }
pre { background: #1a1a2e; color: #a5d6ff; padding: 0.75rem; border-radius: 6px; overflow-x: auto; font-size: 0.85rem; }
form { display: flex; gap: 0.5rem; }
input {
  flex: 1; padding: 0.5rem 0.75rem; border: 1px solid #d1d5db; border-radius: 6px;
  font-size: 1rem;
}
input:focus { outline: none; border-color: #3b82f6; box-shadow: 0 0 0 3px rgba(59,130,246,0.15); }
button {
  padding: 0.5rem 1rem; background: #3b82f6; color: #fff; border: none; border-radius: 6px;
  cursor: pointer; font-size: 0.95rem; font-weight: 500;
}
button:hover { background: #2563eb; }
button[small] { padding: 0.25rem 0.6rem; font-size: 0.8rem; background: #6b7280; }
button[small]:hover { background: #4b5563; }
ul { list-style: none; }
li {
  padding: 0.6rem 0.75rem; border-bottom: 1px solid #f3f4f6; display: flex;
  justify-content: space-between; align-items: center;
}
li:last-child { border-bottom: none; }
li .del {
  background: transparent; color: #dc2626; padding: 0.15rem 0.5rem; font-size: 0.8rem;
}
li .del:hover { background: #fee2e2; }
.empty { color: #9ca3af; font-style: italic; padding: 0.6rem 0; }
)CSS";
}

// ─── public/js/app.js ────────────────────────────────────────────────
inline std::string app_js() {
    return R"JS(// Tiny vanilla JS client — no framework, no build step.

async function api(path, opts = {}) {
  const res = await fetch(path, {
    headers: { 'Content-Type': 'application/json' },
    ...opts,
  });
  if (!res.ok) {
    const err = await res.json().catch(() => ({ error: res.statusText }));
    throw new Error(err.error || res.statusText);
  }
  return res.json();
}

async function refreshHealth() {
  try {
    const h = await api('/api/health');
    document.getElementById('health').textContent = JSON.stringify(h, null, 2);
  } catch (e) {
    document.getElementById('health').textContent = 'ERROR: ' + e.message;
  }
}

async function refreshItems() {
  const ul = document.getElementById('items');
  try {
    const { items } = await api('/api/items');
    if (items.length === 0) {
      ul.innerHTML = '<li class="empty">No items yet. Create one above.</li>';
      return;
    }
    ul.innerHTML = items.map(i => `
      <li>
        <span>#${i.id} · ${escapeHtml(i.name)}</span>
        <button class="del" data-id="${i.id}">delete</button>
      </li>
    `).join('');
    ul.querySelectorAll('.del').forEach(btn => {
      btn.onclick = async () => {
        await api('/api/items/' + btn.dataset.id, { method: 'DELETE' });
        refreshItems();
      };
    });
  } catch (e) {
    ul.innerHTML = '<li class="empty">ERROR: ' + escapeHtml(e.message) + '</li>';
  }
}

function escapeHtml(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

document.getElementById('createForm').onsubmit = async (e) => {
  e.preventDefault();
  const input = e.target.name;
  try {
    await api('/api/items', {
      method: 'POST',
      body: JSON.stringify({ name: input.value }),
    });
    input.value = '';
    refreshItems();
  } catch (err) {
    alert(err.message);
  }
};

document.getElementById('refresh').onclick = refreshItems;

refreshHealth();
refreshItems();
setInterval(refreshHealth, 30000);
)JS";
}

// ─── start.sh (Linux/Mac) ────────────────────────────────────────────
inline std::string start_sh() {
    return std::string(R"SH(#!/usr/bin/env bash
# Launch the Bantu web app on http://localhost:${PORT:-8080}.
set -eu
cd "$(dirname "$0")"

# Find the bantu binary: prefer ./bantu (project-local), else PATH.
BANTU="./bantu"
if [[ ! -x "$BANTU" ]]; then
    if command -v bantu >/dev/null 2>&1; then
        BANTU="bantu"
    else
        echo "bantu binary not found."
        echo "  Either place a 'bantu' binary in this folder, or install it system-wide."
        exit 1
    fi
fi

export PORT="${PORT:-8080}"
echo "Starting on http://localhost:${PORT}"
exec "$BANTU" run main.b
)SH");
}

// ─── start.bat (Windows) ─────────────────────────────────────────────
inline std::string start_bat() {
    return R"BAT(@echo off
REM  Launch the Bantu web app on http://localhost:8080
cd /d "%~dp0"

if "%PORT%"=="" set PORT=8080

REM  Prefer bantu.exe in this folder (e.g. from the chatbantu-windows-x64 zip),
REM  else fall back to bantu.exe on PATH.
if exist "bantu.exe" (
    set BANTU=bantu.exe
) else (
    where bantu.exe >nul 2>&1
    if errorlevel 1 (
        echo [ERROR] bantu.exe not found.
        echo          Place bantu.exe + its DLLs in this folder, or install bantu system-wide.
        pause
        exit /b 1
    )
    set BANTU=bantu.exe
)

echo Starting on http://localhost:%PORT% ...
"%BANTU%" run main.b
pause
)BAT";
}

// ─── Dockerfile (Render-ready) ───────────────────────────────────────
// Uses the multi-stage pattern from ChatBantu: build the bantu binary
// inside Ubuntu 22.04 (ABI-compatible runtime), then run a thin image.
// NOTE: this Dockerfile assumes you have bantu-src/compiler/ available
// in your repo (copy it from chatbantu-dev/bantu-src/). If you don't
// want to rebuild from source, replace this with a single-stage image
// that just COPYs a prebuilt ./bantu binary.
inline std::string dockerfile() {
    return R"DOCKER(# ─── Stage 1: Build the Bantu binary ──────────────────────────
FROM ubuntu:22.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++ gcc make binutils file \
        libsqlite3-dev libcurl4-openssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY bantu-src/compiler/ /build/compiler/
RUN cd /build/compiler && chmod +x build.sh && ./build.sh \
    && cp build/bantu /build/bantu

# ─── Stage 2: Runtime ──────────────────────────────────────────────
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 libcurl4 ca-certificates sqlite3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=builder /build/bantu /usr/local/bin/bantu
COPY main.b /app/main.b
COPY public/ /app/public/
RUN mkdir -p /data && chmod 777 /data
ENV PORT=8080
EXPOSE 8080
CMD ["bantu", "run", "main.b"]
)DOCKER";
}

// ─── render.yaml ─────────────────────────────────────────────────────
inline std::string render_yaml(const std::string& name) {
    std::string s = R"YAML(services:
  - type: web
    name: %%NAME%%
    runtime: docker
    plan: free
    region: ohio
    dockerfilePath: ./Dockerfile
    dockerContext: .
    healthCheckPath: /api/health
    autoDeploy: true
    envVars:
      - key: PORT
        value: 8080
    disk:
      name: %%NAME%%-data
      mountPath: /data
      sizeGB: 1
)YAML";
    replaceAll(s, "%%NAME%%", name);
    return s;
}

// ─── .gitignore ──────────────────────────────────────────────────────
inline std::string gitignore() {
    return R"GIT(# Bantu / Sua local artifacts
*.db
*.db-wal
*.db-shm
.env
.env.local
.DS_Store
*.log
node_modules/

# Build artifacts
bantu-src/compiler/build/

# Editor / OS junk
*.swp
*.swo
*~
.idea/
.vscode/
)GIT";
}

// ─── README.md ───────────────────────────────────────────────────────
inline std::string readme_md(const std::string& name) {
    std::string s = R"MD(# %%NAME%%

A web app built with **Bantu** + **Sua** + **SQLite**. Scaffolded by `bantu init --web`.

## Run locally

### Option A — Linux / macOS

```bash
./start.sh
```

Open http://localhost:8080.

### Option B — Windows

Double-click `start.bat`. (Requires `bantu.exe` + its DLLs in this folder — grab them from the [bantusua-local release](https://github.com/AsseySilivestir/bantusua-local/releases).)

### Option C — Anywhere with `bantu` on PATH

```bash
bantu run main.b
```

## API

| Method | Path              | Purpose              |
|--------|-------------------|----------------------|
| GET    | `/api/health`     | Health check (JSON)  |
| GET    | `/api/items`      | List items           |
| POST   | `/api/items`      | Create item `{name}` |
| DELETE | `/api/items/:id`  | Delete item          |
| GET    | `/`               | Static frontend      |

## Database

SQLite file at `./app.db` locally, or `/data/app.db` on Render (persistent volume).

## Deploy to Render

1. Push this repo to GitHub.
2. Render → New → Blueprint → connect the repo.
3. Render detects `render.yaml` and creates the service.
4. The Dockerfile builds `bantu` from source inside Ubuntu 22.04 (ABI-safe), then runs `bantu run main.b`.

## Edit & iterate

- Edit `main.b` → restart the server (`Ctrl-C` then re-run).
- Edit `public/*` → just refresh the browser (no build step).
- Add new routes: copy the pattern of `handleList` / `handleCreate`.

## Project structure

```
%%NAME%%/
├── main.b              ← backend (single Bantu file)
├── public/             ← frontend (vanilla HTML/CSS/JS)
│   ├── index.html
│   ├── css/style.css
│   └── js/app.js
├── start.sh            ← Linux/Mac launcher
├── start.bat           ← Windows launcher
├── Dockerfile          ← Render-ready multi-stage build
├── render.yaml         ← Render blueprint
├── bantu.json          ← project config
├── .gitignore
└── README.md
```

## Learn more

- Bantu language: https://github.com/AsseySilivestir/bantu-lang
- Example app: https://github.com/AsseySilivestir/bantusua-local (ChatBantu — full social network)
)MD";
    replaceAll(s, "%%NAME%%", name);
    return s;
}

// ─── bantu.json ──────────────────────────────────────────────────────
inline std::string bantu_json(const std::string& name, const std::string& version) {
    std::string s = R"JSON({
  "name": "%%NAME%%",
  "version": "1.0.0",
  "entry": "main.b",
  "template": "web",
  "language": "bantu",
  "bantuVersion": "%%VER%%"
}
)JSON";
    replaceAll(s, "%%NAME%%", name);
    replaceAll(s, "%%VER%%", version);
    return s;
}

} // namespace bantu_templates
