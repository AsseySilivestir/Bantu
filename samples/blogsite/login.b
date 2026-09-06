// ============================================================================
// login.b — Simple token-based login module for the Bantu Blogsite sample
//
// Exposes:
//   authenticate($req) — returns { ok, userId, token } or { ok: false }
//
// In production, hash the password (Bantu will expose sua.crypto.hash
// in v1.3); for the sample we compare plain text to the seeded user.
// ============================================================================

$USERS = [
    { "id": 1, "username": "admin", "password": "bantu123", "role": "admin" },
    { "id": 2, "username": "alice", "password": "wonderland", "role": "author" }
];

authenticate = def($req) {
    $username = $req.body["username"];
    $password = $req.body["password"];

    each ($u in $USERS) {
        if ($u["username"] == $username && $u["password"] == $password) {
            $token = "bantu-" + $u["id"] + "-" + ($u["username"] + ":1700000000");
            return {
                "ok": true,
                "userId": $u["id"],
                "username": $u["username"],
                "role": $u["role"],
                "token": $token
            };
        }
    }
    return { "ok": false, "error": "invalid credentials" };
};

print("[login] seeded 2 users: admin, alice");
