# Bantu POS — Point-of-Sale System

A complete Point-of-Sale (POS) system built with **Bantu v1.2.2**, **vanilla
HTML/CSS/JavaScript**, and **PostgreSQL**. No frameworks, no build step,
no npm install — just `bantu run server.b` and you're open for business.

![Stack](https://img.shields.io/badge/stack-Bantu%20%2B%20PostgreSQL%20%2B%20vanilla%20JS-blue)

## Features

### Checkout (`/`)
- **Product grid** with live search (name / SKU / barcode) and category filter
- **Barcode scanner** — uses the browser's native `BarcodeDetector` API (Chrome on Android/desktop). Falls back to manual entry on unsupported browsers
- **Cart** with per-line qty controls, tax-per-line (VAT) calculation, and stock enforcement (can't oversell)
- **Discount** field for promotions
- **Multiple payment methods** — cash, card, M-Pesa, mobile money
- **Change calculation** for cash payments
- **Printable receipt** with store branding, itemized lines, totals, and payment breakdown
- **Keyboard shortcuts** — `F2` focus search, `F9` checkout, `Esc` close modal

### Products Management (`/products.html`)
- Full CRUD — create, list, search, edit, soft-delete (delete preserves historical sales integrity)
- Fields: name, SKU, barcode, category, price, cost, stock, tax rate, active flag
- Category autocomplete (Beverages, Bakery, Dairy, Groceries, Household, Stationery, Electronics)

### Sales History (`/sales.html`)
- List of last 200 sales with receipt number, customer, items, totals, payment method
- Click any sale to re-print the receipt

### Dashboard (`/dashboard.html`)
- **KPI cards** — Today's revenue/sales, This week, This month, Inventory value, Low-stock count
- **7-day revenue bar chart** (CSS-only, no chart library)
- **Top 5 products** by revenue (with units sold)

## Architecture

```
pos-system/
├── server.b              ← Bantu entry point — boots Sua HTTP server on :4000
├── db.b                  ← PostgreSQL data layer (schema, CRUD, helpers)
├── routes.b              ← HTTP route handlers (/api/products, /api/checkout, etc.)
├── schema.sql            ← Standalone SQL schema (run manually if preferred)
├── bantu.json            ← Bantu project manifest
├── public/               ← Static frontend served by Sua
│   ├── index.html        ← Checkout page (default route)
│   ├── products.html     ← Products management
│   ├── sales.html        ← Sales history
│   ├── dashboard.html    ← Dashboard
│   ├── css/style.css     ← All styling (single file, no framework)
│   └── js/
│       ├── app.js        ← Shared utilities (API helpers, money formatter, toast)
│       ├── pos.js        ← Checkout page logic
│       ├── products.js   ← Products CRUD
│       ├── sales.js      ← Sales history + receipt re-print
│       └── dashboard.js  ← KPI cards + bar chart
└── README.md             ← This file
```

**Data flow:**
```
Browser ──HTTP──▶ Sua server (server.b) ──▶ routes.b ──▶ db.b ──▶ PostgreSQL
                       │
                       └──▶ public/ (static HTML/CSS/JS)
```

All business logic — totals, tax, stock decrement, receipt number generation
— happens **server-side** in Bantu. The frontend is purely presentational
and never computes money values (it can't be tricked by client-side edits).

## Prerequisites

1. **Bantu v1.2.2** — install from <https://github.com/AsseySilivestir/Bantu/releases/tag/v1.2.2>
   - Linux: `curl -L -o bantu.zip https://github.com/AsseySilivestir/Bantu/releases/download/v1.2.2/Bantu-v1.2.2-linux-x64.zip && unzip bantu.zip && cd bantu-v1.2.2-linux-x64 && chmod +x bantu && ./bantu setup --seed`
   - Windows: download the zip, unzip, double-click `install.bat`
   - macOS: build from source — see "macOS install" section below
2. **PostgreSQL 12+** running locally
   - Ubuntu/Debian: `sudo apt install postgresql postgresql-contrib`
   - macOS: `brew install postgresql@16 && brew services start postgresql@16`
   - Windows: <https://www.postgresql.org/download/windows/>

> **Note on the Bantu binary:** the v1.2.2 release ships a *deterministic
> stub* for `sua.postgres` — meaning the API surface is correct but
> queries return placeholder data. To run the POS against a real PostgreSQL
> database you need to rebuild `bantu` with `-DBANTU_POSTGRES=ON`. See
> "Rebuilding Bantu with real PostgreSQL support" below.

## Setup

### 1. Create the PostgreSQL database

```bash
# As the postgres user, create a `pos` database
sudo -u postgres createuser --superuser $USER 2>/dev/null
sudo -u postgres psql -c "ALTER USER $USER PASSWORD 'postgres';"
sudo -u postgres createdb -O $USER pos

# Apply the schema (creates tables + seeds 20 sample products)
psql -d pos -f schema.sql
```

### 2. Start the Bantu POS server

```bash
cd samples/pos-system

# (Optional) Override the default connection string
export DATABASE_URL="host=localhost dbname=pos user=$USER password=postgres port=5432"

# Launch
bantu run server.b
```

You should see:
```
============================================================
  Bantu POS System v1.0.0
  Backend  : Bantu v1.2.2 + Sua + PostgreSQL
  Frontend : HTML / CSS / vanilla JS
============================================================
[db] connecting to PostgreSQL...
[db] schema ready
[db] products table has 20 rows
[routes] registered 11 routes under /api/*
[server] POS UI:  http://localhost:4000
[server] API:     http://localhost:4000/api/health
[server] Press Ctrl+C to stop.
```

### 3. Open the POS in your browser

Navigate to <http://localhost:4000> — you'll see the checkout page with 20
sample products across 7 categories.

Try it:
1. Click a product (or use the search bar) — it appears in the cart on the right
2. Adjust quantities with the +/− buttons
3. Enter the amount tendered (cash example: total = 5,500 → enter 10,000 → change = 4,500)
4. Click **Checkout** — the receipt modal pops up
5. Click **Print** to send to a receipt printer (or save as PDF)

## Rebuilding Bantu with real PostgreSQL support

The default release binary uses a stub for `sua.postgres`. To make actual
DB calls, rebuild `bantu` against `libpq`:

```bash
# Install libpq (PostgreSQL client library)
sudo apt install libpq-dev          # Debian/Ubuntu
# brew install libpq                 # macOS
# (Windows: install PostgreSQL incl. dev headers)

# Rebuild bantu with BANTU_POSTGRES=ON
cd bantu-src/compiler
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBANTU_POSTGRES=ON
cmake --build . --config Release -j

# Replace the bundled bantu with the freshly-built one
cp bantu ../../../bantu
cd ../../..
./bantu --version    # → Bantu v1.2.2
```

Now `sua.postgres.connect/query/exec/close` will hit your real PostgreSQL
database and the POS will fully function.

## API Reference

All endpoints are prefixed with `/api` and return JSON.

| Method | Path                       | Body / Query                         | Returns                          |
|--------|----------------------------|--------------------------------------|----------------------------------|
| GET    | `/api/health`              | —                                    | Service status                   |
| GET    | `/api/products`            | `?q=<search>`                        | List of active products          |
| GET    | `/api/products/:id`        | —                                    | Single product                   |
| GET    | `/api/products/lookup/:c`  | —                                    | Product by barcode or SKU        |
| POST   | `/api/products`            | `{name, sku, price, ...}`            | Created product                  |
| PUT    | `/api/products/:id`        | `{name, sku, price, ...}`            | Updated product                  |
| DELETE | `/api/products/:id`        | —                                    | `{ok:true}` (soft delete)        |
| GET    | `/api/sales`               | `?limit=<n>` (default 100)           | List of recent sales             |
| GET    | `/api/sales/:id`           | —                                    | Sale + line items                |
| POST   | `/api/checkout`            | `{items, payment_method, payment_amount, discount, customer_id}` | Created sale with receipt |
| GET    | `/api/dashboard`           | —                                    | Today/week/month stats, top items, 7-day chart |

### Checkout payload example

```json
{
    "items": [
        { "id": 1, "qty": 2 },
        { "id": 5, "qty": 1 }
    ],
    "payment_method": "cash",
    "payment_amount": 10000,
    "discount": 0,
    "customer_id": 1
}
```

## Configuration

| Env var          | Default                                                                | Purpose                                  |
|------------------|------------------------------------------------------------------------|------------------------------------------|
| `DATABASE_URL`   | `host=localhost dbname=pos user=postgres password=postgres port=5432`  | PostgreSQL connection string             |
| `PORT`           | `4000`                                                                 | HTTP port the POS UI listens on          |

## Currency

The frontend defaults to **TZS** (Tanzanian Shilling, no decimals).
Override in the browser console:
```js
localStorage.setItem('posCurrency', 'USD'); location.reload();
```
Supported: `TZS`, `KES`, `UGX` (whole-shilling), `USD`, `EUR`, `GBP` (2-decimal).

## Producing a desktop installer for the POS

Once you're happy with the app, package it for distribution:

```bash
# Linux .deb + .desktop
bantu installer server.b --platform linux --name "BantuPOS" --version "1.0.0"

# Windows NSIS installer
bantu installer server.b --platform windows --name "BantuPOS" --version "1.0.0"

# macOS .app bundle
bantu installer server.b --platform macos --name "BantuPOS" --version "1.0.0"

# Android APK (for tablet POS)
bantu installer server.b --platform android --name "BantuPOS" --version "1.0.0"
```

Each installer bundles the `bantu` interpreter + your `server.b` + the
`public/` folder — end users don't need Bantu installed.

## License

MIT — see `LICENSE` in the repo root.
