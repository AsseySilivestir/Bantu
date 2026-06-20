// ============================================================================
// pg-dashboard/server.b — Analytics dashboard backed by PostgreSQL (v1.2.2)
//
// Demonstrates:
//   - sua.postgres.connect / query
//   - sua.server static + JSON API
//   - Modular separation (db layer + dashboard routes)
// ============================================================================

print("=== Bantu PG Dashboard v1.2.2 ===");

include "./db.b";                       // brings $pg, initSchema, metrics() into scope
include "./dashboard_routes.b" as dash; // namespaced — exposes dash.registerAll(sua)

// 1. Connect to PostgreSQL (override connStr in production via env)
$connStr = "host=localhost dbname=dashboard user=postgres password=postgres port=5432";
$pg.connect($connStr);
initSchema();

// 2. Register routes
dash.registerAll(sua);

// 3. Serve the dashboard UI
sua.server.static("./public");
sua.server.listen(5000);
print("[dashboard] http://localhost:5000");
