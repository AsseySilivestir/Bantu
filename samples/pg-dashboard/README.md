# Bantu PG Dashboard Sample

A small analytics dashboard backed by PostgreSQL, demonstrating `sua.postgres.*` and modular `include`.

## Prerequisites

You need a running PostgreSQL. Quick start with Docker:

```bash
docker run -d --name bantu-pg -e POSTGRES_PASSWORD=postgres -e POSTGRES_DB=dashboard -p 5432:5432 postgres:16
```

## Run

```bash
bantu run server.b
# → http://localhost:5000
```

## What it shows

- `sua.postgres.connect(connStr)` — libpq-style connection string
- `sua.postgres.exec(sql)` — DDL + INSERT
- `sua.postgres.query(sql)` — SELECT returning rows
- Modular project: `server.b` → `db.b` + `dashboard_routes.b`
- Live-updating dashboard UI that polls `/api/metrics` every 5s

## Files

```
pg-dashboard/
├── server.b             # Entry point
├── db.b                 # PostgreSQL layer (initSchema, metrics, trackEvent)
├── dashboard_routes.b   # Route registration (registerAll)
├── public/
│   └── index.html       # Dashboard UI
└── bantu.json
```

## Build with real libpq

The default Bantu binary ships with a deterministic PG stub (so the sample runs offline). For real PostgreSQL:

```bash
sudo apt install libpq-dev
cd bantu-src/compiler
mkdir build && cd build
cmake .. -DBANTU_POSTGRES=ON
make -j$(nproc)
sudo cp bantu /usr/local/bin/
```
