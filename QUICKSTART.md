# 🚀 ChatBantu — Quick Start (Local)

Get ChatBantu running on your machine in **under 60 seconds**, three different ways.

---

## Option A — Native (fastest, recommended on Linux)

> Requires: Linux x86_64 with `libsqlite3` and `libcurl4` installed.

```bash
git clone https://github.com/AsseySilivestir/bantusua-local.git
cd bantusua-local
chmod +x dev.sh
./dev.sh
```

That's it. The script will:
1. Verify the Bantu binary works on your system
2. Check shared libraries (`libsqlite3`, `libcurl`)
3. Start the server on `http://localhost:8080`
4. Open the browser for you

**Login with the demo account:**
- Username: `silivestir`
- Password: `bantu123`

---

## Option B — Docker (no system deps, cross-platform)

> Requires: Docker (or Docker Desktop).

```bash
git clone https://github.com/AsseySilivestir/bantusua-local.git
cd bantusua-local
./dev.sh --docker
```

Or directly with docker compose:

```bash
docker compose -f docker-compose.dev.yml up --build
```

Then open `http://localhost:8080`.

The SQLite DB is persisted in a Docker volume (`chatbantu-data`) so your data survives restarts.

---

## Option C — Rebuild Bantu from source (for hackers)

> Requires: `g++`, `cmake`, `make`, `libsqlite3-dev`, `libcurl4-openssl-dev`.

```bash
git clone https://github.com/AsseySilivestir/bantusua-local.git
cd bantusua-local
./dev.sh --build        # rebuilds bantu-src/compiler/ → ./bantu
```

Or via Makefile:

```bash
make build              # rebuild binary
make run                # start server
```

---

## Common dev tasks

| Task                       | Command                     |
|----------------------------|-----------------------------|
| Start on a custom port     | `./dev.sh --port 9000`      |
| Wipe DB & reseed           | `./dev.sh --reset-db`       |
| Start without opening browser | `./dev.sh --no-browser` |
| Rebuild binary + start     | `./dev.sh --build`          |
| Stop background server     | `make stop`                 |
| Run smoke tests            | `make test`                 |
| Tail logs                  | `make logs`                 |

---

## Verifying it works

In another terminal:

```bash
# Health check
curl http://localhost:8080/api/health
# → {"status":"ok","app":"ChatBantu", ...}

# Login
curl -X POST http://localhost:8080/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"silivestir","password":"bantu123"}'
# → {"token":"...","user":{...}}

# Get the feed (replace $TOKEN)
curl http://localhost:8080/api/posts -H "Authorization: Bearer $TOKEN"
```

---

## Where data lives

| Mode    | DB path                          |
|---------|----------------------------------|
| Native  | `./chatbantu.db` (next to server.b) |
| Docker  | Docker volume `chatbantu-data` → `/data/chatbantu.db` |
| Render  | Render disk → `/data/chatbantu.db` |

To wipe everything: `./dev.sh --reset-db` (or `rm chatbantu.db*`).

---

## Troubleshooting

**`./bantu: error while loading shared libraries: libsqlite3.so.0`**
→ Install: `sudo apt-get install -y libsqlite3-0 libcurl4 ca-certificates`

**Port 8080 already in use**
→ Use a different port: `./dev.sh --port 3000`

**`./bantu: /lib/x86_64-linux-gnu/libm.so.6: version GLIBC_2.38 not found`**
→ You're on an older distro. Either:
  - Rebuild from source: `./dev.sh --build`
  - Use Docker: `./dev.sh --docker`

**Browser doesn't auto-open**
→ Open `http://localhost:8080` manually, or set `BROWSER=1`.

---

## Next steps

- Read `README.md` for the full API reference and architecture.
- Edit `server.b` — restart the server to pick up changes.
- Edit `public/*.html` — refresh the browser (no build step).
- Deploy to Render with the included `render.yaml` blueprint.
