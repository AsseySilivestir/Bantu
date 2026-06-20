// ============================================================================
// dashboard_routes.b — Route registration for the PG Dashboard sample
// Exposes:
//   registerAll($sua)
// ============================================================================

registerAll = def($sua) {
    // GET /api/metrics — aggregate event counts by kind
    $sua.server.get("/api/metrics", def($req, $res) {
        $m = metrics();
        $res.json({ "ok": true, "metrics": $m });
    });

    // POST /api/track  { kind, value }
    $sua.server.post("/api/track", def($req, $res) {
        $kind  = $req.body["kind"];
        $value = $req.body["value"];
        if ($kind == "" || $kind == null) {
            $res.status(400);
            $res.json({ "ok": false, "error": "kind is required" });
            return;
        }
        if ($value == null) { $value = 1; }
        trackEvent($kind, $value);
        $res.status(201);
        $res.json({ "ok": true, "tracked": $kind });
    });

    // GET /api/health
    $sua.server.get("/api/health", def($req, $res) {
        $res.json({
            "ok": true,
            "service": "pg-dashboard",
            "version": "1.2.2",
            "database": "postgresql"
        });
    });

    print("[routes] registered 3 routes: /api/metrics, /api/track, /api/health");
};
