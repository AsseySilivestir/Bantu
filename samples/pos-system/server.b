// ============================================================================
// server.b — Entry point for the Bantu POS System
// ----------------------------------------------------------------------------
// Tech stack:
//   • Backend  — Bantu v1.2.2 (Sua HTTP framework + PostgreSQL driver)
//   • Database — PostgreSQL 12+
//   • Frontend — HTML5 + CSS3 + vanilla JavaScript (no framework, no build step)
//
// Serves a Point-of-Sale web app on http://localhost:4000 with:
//   • Product grid (search + filter by category + barcode scanner support)
//   • Cart with tax-per-line calculation
//   • Checkout with cash/card/mpesa payment + change calculation
//   • Receipt (printable)
//   • Products management (full CRUD)
//   • Sales history with receipt re-print
//   • Dashboard (today / week / month stats, top items, last-7-day chart)
// ============================================================================

print("============================================================");
print("  Bantu POS System v1.0.0");
print("  Backend  : Bantu v1.2.2 + Sua + PostgreSQL");
print("  Frontend : HTML / CSS / vanilla JS");
print("============================================================");

// 1. Load DB layer and routes
include "./db.b";
include "./routes.b" as routes;

// 2. Connect to PostgreSQL
//
// Configure via the $DATABASE_URL env var, OR edit the connection string below.
// Default assumes a local dev PG running on :5432 with db=pos, user=postgres.
//
//   export DATABASE_URL="host=localhost dbname=pos user=postgres password=postgres port=5432"
//
$connStr = "host=localhost dbname=pos user=postgres password=postgres port=5432";
$envUrl = sua.env("DATABASE_URL");
if ($envUrl != null && $envUrl != "") {
    $connStr = $envUrl;
}
print("[db] connecting to PostgreSQL...");
$pg.connect($connStr);

// 3. Initialize schema (idempotent — safe to run on every startup)
initSchema();
print("[db] schema ready");

// 4. Register HTTP API routes
routes.registerAll(sua);

// 5. Serve the POS UI (HTML/CSS/JS) from ./public
sua.server.static("./public");

// 6. Listen on port 4000 (override with $PORT env var)
$port = 4000;
$envPort = sua.env("PORT");
if ($envPort != null && $envPort != "") {
    $port = floor(num($envPort));
}
print("[server] POS UI:  http://localhost:" + $port);
print("[server] API:     http://localhost:" + $port + "/api/health");
print("[server] Press Ctrl+C to stop.");
sua.server.listen($port);
