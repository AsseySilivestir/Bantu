// ============================================================================
// routes.b — Route registration for the Bantu Blogsite sample
//
// Exposes:
//   registerAll($sua) — wire all HTTP routes onto the sua.server instance.
//
// This module is included with `include "./routes.b" as routes;` so the
// caller accesses it via `routes.registerAll(sua)`.
// ============================================================================

registerAll = def($sua) {
    // GET /api/posts — list all posts
    $sua.server.get("/api/posts", def($req, $res) {
        postController.index($req, $res);
    });

    // GET /api/posts/:id — fetch one post
    $sua.server.get("/api/posts/:id", def($req, $res) {
        postController.show($req, $res);
    });

    // POST /api/posts — create a new post
    $sua.server.post("/api/posts", def($req, $res) {
        postController.create($req, $res);
    });

    // GET /api/health — simple health check
    $sua.server.get("/api/health", def($req, $res) {
        $res.json({
            "ok": true,
            "service": "bantu-blogsite",
            "version": "1.2.2"
        });
    });

    print("[routes] registered 4 routes under /api/*");
};
