// ════════════════════════════════════════════════════════════════════
//  ChatBantu — Social Network Backend
//  ────────────────────────────────────────────────────────────────────
//  Pure Bantu Language + Sua Framework + SQLite
//  Features:
//    • User registration & login (token-based, no external deps)
//    • Social feed: posts, likes, comments
//    • Friendships: follow / unfollow
//    • Real-time 1-to-1 chat (HTTP long-poll fallback)
//    • Presence: online/offline via heartbeat
//    • WebRTC signaling: offer / answer / ICE exchange
//    • Live notifications
//
//  Run:    bantu run server.b
//  HTTP:   http://0.0.0.0:$PORT
//  DB:     /data/chatbantu.db (Render volume) or ./chatbantu.db (local)
// ════════════════════════════════════════════════════════════════════

print "═══════════════════════════════════════════";
print "  ChatBantu — Social Network Backend";
print "  Pure Bantu + Sua + SQLite";
print "═══════════════════════════════════════════";

// ─── Configuration ───────────────────────────────────────────────────
string $envPort = env("PORT");
if (!$envPort) { $envPort = "8080"; }
string $dbPath = "/data/chatbantu.db";

// Probe persistent volume; fall back to local file
dict $probe = sua.sqlite.open($dbPath);
if (!$probe.connected) {
    $dbPath = "chatbantu.db";
    print "[INFO] /data not writable, using local: " + $dbPath;
} else {
    print "[INFO] Using persistent volume: " + $dbPath;
}

print "[INFO] Opening SQLite at " + $dbPath;
dict $conn = sua.sqlite.open($dbPath);
if (!$conn.connected) {
    print "[ERROR] Cannot open SQLite — aborting.";
    exit(1);
}
print "[OK] Connected to SQLite.";

// ─── Schema ──────────────────────────────────────────────────────────
sua.sqlite.exec("PRAGMA journal_mode=WAL;");
sua.sqlite.exec("PRAGMA foreign_keys=ON;");

sua.sqlite.exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT UNIQUE NOT NULL, password TEXT NOT NULL, display_name TEXT NOT NULL, bio TEXT DEFAULT '', avatar TEXT DEFAULT '', token TEXT, last_seen INTEGER DEFAULT 0, created_at TEXT DEFAULT (datetime('now')));");

sua.sqlite.exec("CREATE TABLE IF NOT EXISTS follows (id INTEGER PRIMARY KEY AUTOINCREMENT, follower_id INTEGER NOT NULL, followee_id INTEGER NOT NULL, created_at TEXT DEFAULT (datetime('now')), UNIQUE(follower_id, followee_id));");

sua.sqlite.exec("CREATE TABLE IF NOT EXISTS posts (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL, body TEXT NOT NULL, image TEXT DEFAULT '', created_at TEXT DEFAULT (datetime('now')), FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE);");
sua.sqlite.exec("CREATE INDEX IF NOT EXISTS idx_posts_user ON posts(user_id);");
sua.sqlite.exec("CREATE INDEX IF NOT EXISTS idx_posts_created ON posts(created_at DESC);");

sua.sqlite.exec("CREATE TABLE IF NOT EXISTS likes (id INTEGER PRIMARY KEY AUTOINCREMENT, post_id INTEGER NOT NULL, user_id INTEGER NOT NULL, created_at TEXT DEFAULT (datetime('now')), UNIQUE(post_id, user_id), FOREIGN KEY (post_id) REFERENCES posts(id) ON DELETE CASCADE, FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE);");
sua.sqlite.exec("CREATE INDEX IF NOT EXISTS idx_likes_post ON likes(post_id);");

sua.sqlite.exec("CREATE TABLE IF NOT EXISTS comments (id INTEGER PRIMARY KEY AUTOINCREMENT, post_id INTEGER NOT NULL, user_id INTEGER NOT NULL, body TEXT NOT NULL, created_at TEXT DEFAULT (datetime('now')), FOREIGN KEY (post_id) REFERENCES posts(id) ON DELETE CASCADE, FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE);");
sua.sqlite.exec("CREATE INDEX IF NOT EXISTS idx_comments_post ON comments(post_id);");

// Conversations are 1-to-1: a sorted pair (a,b) is the conversation key.
sua.sqlite.exec("CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY AUTOINCREMENT, from_id INTEGER NOT NULL, to_id INTEGER NOT NULL, body TEXT NOT NULL, created_at TEXT DEFAULT (datetime('now')), delivered INTEGER DEFAULT 0, FOREIGN KEY (from_id) REFERENCES users(id) ON DELETE CASCADE, FOREIGN KEY (to_id) REFERENCES users(id) ON DELETE CASCADE);");
sua.sqlite.exec("CREATE INDEX IF NOT EXISTS idx_messages_to ON messages(to_id, delivered, id);");
sua.sqlite.exec("CREATE INDEX IF NOT EXISTS idx_messages_pair ON messages(from_id, to_id, id);");

sua.sqlite.exec("CREATE TABLE IF NOT EXISTS notifications (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL, type TEXT NOT NULL, body TEXT NOT NULL, link TEXT DEFAULT '', is_read INTEGER DEFAULT 0, created_at TEXT DEFAULT (datetime('now')), FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE);");
sua.sqlite.exec("CREATE INDEX IF NOT EXISTS idx_notif_user ON notifications(user_id, is_read, id);");

// WebRTC signaling — one row per offer / answer / ICE candidate.
// 'type' is one of: offer, answer, ice.
// 'payload' holds the SDP or candidate JSON string.
// 'consumed' = 1 once the recipient has fetched it.
sua.sqlite.exec("CREATE TABLE IF NOT EXISTS signaling (id INTEGER PRIMARY KEY AUTOINCREMENT, from_id INTEGER NOT NULL, to_id INTEGER NOT NULL, type TEXT NOT NULL, payload TEXT NOT NULL, consumed INTEGER DEFAULT 0, created_at TEXT DEFAULT (datetime('now')), FOREIGN KEY (from_id) REFERENCES users(id) ON DELETE CASCADE, FOREIGN KEY (to_id) REFERENCES users(id) ON DELETE CASCADE);");
sua.sqlite.exec("CREATE INDEX IF NOT EXISTS idx_signal_to ON signaling(to_id, consumed, id);");

print "[OK] Schema ready (users, follows, posts, likes, comments, messages, notifications, signaling).";

// ─── Seed an admin user if empty ─────────────────────────────────────
list $u = sua.sqlite.query("SELECT COUNT(*) AS n FROM users;");
if (num($u[0].n) == 0) {
    sua.sqlite.exec("INSERT INTO users (username, password, display_name, bio) VALUES ('silivestir', 'bantu123', 'Silivestir', 'Creator of ChatBantu. Building African tech with Bantu + Sua.');");
    sua.sqlite.exec("INSERT INTO users (username, password, display_name, bio) VALUES ('alice', 'alice123', 'Alice Mwangi', 'Designer & photographer from Nairobi.');");
    sua.sqlite.exec("INSERT INTO users (username, password, display_name, bio) VALUES ('bob', 'bob123', 'Bob Otieno', 'Software engineer. Coffee enthusiast.');");
    sua.sqlite.exec("INSERT INTO follows (follower_id, followee_id) VALUES (1, 2);");
    sua.sqlite.exec("INSERT INTO follows (follower_id, followee_id) VALUES (1, 3);");
    sua.sqlite.exec("INSERT INTO follows (follower_id, followee_id) VALUES (2, 1);");
    sua.sqlite.exec("INSERT INTO posts (user_id, body) VALUES (1, 'Welcome to ChatBantu — a social network built entirely with the Bantu programming language and the Sua web framework! Real-time chat, video calls, and a feed, all powered by SQLite.');");
    sua.sqlite.exec("INSERT INTO posts (user_id, body) VALUES (2, 'Just shipped a new design portfolio. Loving how Bantu makes backend code feel familiar and clean.');");
    sua.sqlite.exec("INSERT INTO posts (user_id, body) VALUES (3, 'Coffee + code = happiness. Working on some new Bantu examples today.');");
    sua.sqlite.exec("INSERT INTO likes (post_id, user_id) VALUES (1, 2);");
    sua.sqlite.exec("INSERT INTO likes (post_id, user_id) VALUES (1, 3);");
    sua.sqlite.exec("INSERT INTO likes (post_id, user_id) VALUES (2, 1);");
    sua.sqlite.exec("INSERT INTO comments (post_id, user_id, body) VALUES (1, 2, 'This is amazing! Proud of you, Silivestir.');");
    sua.sqlite.exec("INSERT INTO comments (post_id, user_id, body) VALUES (1, 3, 'Bantu is going to change the game for African developers.');");
    print "[OK] Seeded 3 users + 3 posts + likes + comments.";
}

// ════════════════════════════════════════════════════════════════════
//  HELPERS
// ════════════════════════════════════════════════════════════════════

// Escape single quotes for SQL string literals.
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

// Generate a pseudo-random hex token (32 chars = 128 bits).
def newToken() {
    string $t = "";
    string $hex = "0123456789abcdef";
    number $i = 0;
    while ($i < 32) {
        // random(15) returns 0..15 inclusive — perfect index into $hex
        number $r = random(15);
        $t = $t + $hex[$r];
        $i = $i + 1;
    }
    return $t;
}

// Current epoch milliseconds (used as a monotonic message cursor).
def nowMs() {
    return clock();
}

// Look up the user by the Bearer token in $req.headers.authorization.
// Returns a dict {id, username, displayName} or null.
def authUser($req) {
    dict $hdrs = $req.headers;
    string $auth = "";
    if ($hdrs.authorization) { $auth = $hdrs.authorization; }
    if (len($auth) < 8) { return null; }
    // Expect "Bearer <token>"
    string $tok = substr($auth, 7);
    if (len($tok) < 8) { return null; }

    list $rows = sua.sqlite.query(
        "SELECT id, username, display_name FROM users WHERE token = '" + esc($tok) + "';"
    );
    if (len($rows) == 0) { return null; }
    dict $u = {
        "id": num($rows[0].id),
        "username": $rows[0].username,
        "displayName": $rows[0].display_name
    };
    // Update last_seen
    sua.sqlite.exec("UPDATE users SET last_seen = " + str(nowMs()) + " WHERE id = " + str($u.id) + ";");
    return $u;
}

// Build a "safe" user dict (no password / token leak).
def publicUser($row) {
    dict $u = {
        "id": num($row.id),
        "username": $row.username,
        "displayName": $row.display_name,
        "bio": $row.bio,
        "avatar": $row.avatar
    };
    return $u;
}

// Build a post dict from a SQL row + optional counts.
def postFromRow($row, $likeCount, $commentCount) {
    dict $p = {
        "id": num($row.id),
        "userId": num($row.user_id),
        "authorName": $row.author_name,
        "authorUsername": $row.author_username,
        "body": $row.body,
        "image": $row.image,
        "createdAt": $row.created_at,
        "likes": num($likeCount),
        "comments": num($commentCount)
    };
    return $p;
}

// Send a notification to $toUserId. type is "like" | "comment" | "follow" | "message".
def notify($toUserId, $type, $body, $link) {
    sua.sqlite.exec(
        "INSERT INTO notifications (user_id, type, body, link) VALUES (" +
        str($toUserId) + ", '" + esc($type) + "', '" + esc($body) + "', '" + esc($link) + "');"
    );
}

// ════════════════════════════════════════════════════════════════════
//  AUTH HANDLERS
// ════════════════════════════════════════════════════════════════════

def handleRegister($req, $res) {
    if (!$req.body.username || !$req.body.password) {
        $res.status(400).json({"error": "username and password are required"});
        return null;
    }
    string $username = $req.body.username;
    string $password = $req.body.password;
    string $displayName = $username;
    if ($req.body.displayName) { $displayName = $req.body.displayName; }

    list $exists = sua.sqlite.query("SELECT id FROM users WHERE username = '" + esc($username) + "';");
    if (len($exists) > 0) {
        $res.status(409).json({"error": "Username already taken"});
        return null;
    }

    string $token = newToken();
    dict $ins = sua.sqlite.exec(
        "INSERT INTO users (username, password, display_name, token) VALUES ('" +
        esc($username) + "', '" + esc($password) + "', '" + esc($displayName) + "', '" + esc($token) + "');"
    );
    number $uid = num($ins.lastInsertId);
    $res.status(201).json({
        "user": {"id": $uid, "username": $username, "displayName": $displayName},
        "token": $token,
        "message": "Account created"
    });
}

def handleLogin($req, $res) {
    if (!$req.body.username || !$req.body.password) {
        $res.status(400).json({"error": "username and password are required"});
        return null;
    }
    list $rows = sua.sqlite.query(
        "SELECT id, username, display_name, password FROM users WHERE username = '" + esc($req.body.username) + "';"
    );
    if (len($rows) == 0) {
        $res.status(401).json({"error": "Invalid credentials"});
        return null;
    }
    if ($rows[0].password != $req.body.password) {
        $res.status(401).json({"error": "Invalid credentials"});
        return null;
    }
    string $token = newToken();
    sua.sqlite.exec("UPDATE users SET token = '" + esc($token) + "' WHERE id = " + str($rows[0].id) + ";");

    $res.json({
        "user": {
            "id": num($rows[0].id),
            "username": $rows[0].username,
            "displayName": $rows[0].display_name
        },
        "token": $token,
        "message": "Login successful"
    });
}

def handleMe($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    list $r = sua.sqlite.query("SELECT * FROM users WHERE id = " + str($me.id) + ";");
    $res.json({"user": publicUser($r[0])});
}

// ════════════════════════════════════════════════════════════════════
//  USERS & FOLLOWS
// ════════════════════════════════════════════════════════════════════

def handleListUsers($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    list $rows = sua.sqlite.query(
        "SELECT u.id, u.username, u.display_name, u.bio, u.avatar, u.last_seen, " +
        "(SELECT COUNT(*) FROM follows WHERE follower_id = u.id) AS following_count, " +
        "(SELECT COUNT(*) FROM follows WHERE followee_id = u.id) AS followers_count, " +
        "EXISTS(SELECT 1 FROM follows WHERE follower_id = " + str($me.id) + " AND followee_id = u.id) AS is_following " +
        "FROM users u WHERE u.id != " + str($me.id) + " ORDER BY u.display_name ASC LIMIT 100;"
    );
    list $out = [];
    number $i = 0;
    each ($r in $rows) {
        dict $u = {
            "id": num($r.id),
            "username": $r.username,
            "displayName": $r.display_name,
            "bio": $r.bio,
            "avatar": $r.avatar,
            "online": (nowMs() - num($r.last_seen)) < 60000,
            "following": num($r.following_count),
            "followers": num($r.followers_count),
            "isFollowing": $r.is_following == 1
        };
        $out[$i] = $u;
        $i = $i + 1;
    }
    $res.json({"users": $out, "count": len($out)});
}

def handleFollow($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $target = num($req.params.id);
    if ($target == $me.id) {
        $res.status(400).json({"error": "Cannot follow yourself"});
        return null;
    }
    list $exists = sua.sqlite.query(
        "SELECT id FROM follows WHERE follower_id = " + str($me.id) + " AND followee_id = " + str($target) + ";"
    );
    if (len($exists) > 0) {
        $res.json({"following": true, "message": "Already following"});
        return null;
    }
    sua.sqlite.exec(
        "INSERT INTO follows (follower_id, followee_id) VALUES (" + str($me.id) + ", " + str($target) + ");"
    );
    list $tu = sua.sqlite.query("SELECT username, display_name FROM users WHERE id = " + str($target) + ";");
    if (len($tu) > 0) {
        notify($target, "follow", $me.displayName + " started following you.", "/u/" + $me.username);
    }
    $res.json({"following": true, "message": "Followed"});
}

def handleUnfollow($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $target = num($req.params.id);
    sua.sqlite.exec(
        "DELETE FROM follows WHERE follower_id = " + str($me.id) + " AND followee_id = " + str($target) + ";"
    );
    $res.json({"following": false, "message": "Unfollowed"});
}

// ════════════════════════════════════════════════════════════════════
//  POSTS (Social Feed)
// ════════════════════════════════════════════════════════════════════

def handleListPosts($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    list $rows = sua.sqlite.query(
        "SELECT p.id, p.user_id, p.body, p.image, p.created_at, " +
        "u.display_name AS author_name, u.username AS author_username, " +
        "(SELECT COUNT(*) FROM likes WHERE post_id = p.id) AS like_count, " +
        "(SELECT COUNT(*) FROM comments WHERE post_id = p.id) AS comment_count, " +
        "EXISTS(SELECT 1 FROM likes WHERE post_id = p.id AND user_id = " + str($me.id) + ") AS liked " +
        "FROM posts p JOIN users u ON u.id = p.user_id " +
        "ORDER BY p.created_at DESC LIMIT 100;"
    );
    list $out = [];
    number $i = 0;
    each ($r in $rows) {
        dict $p = postFromRow($r, $r.like_count, $r.comment_count);
        $p.liked = $r.liked == 1;
        $out[$i] = $p;
        $i = $i + 1;
    }
    $res.json({"posts": $out, "count": len($out)});
}

def handleCreatePost($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    if (!$req.body.body) {
        $res.status(400).json({"error": "body is required"});
        return null;
    }
    string $image = "";
    if ($req.body.image) { $image = $req.body.image; }
    dict $ins = sua.sqlite.exec(
        "INSERT INTO posts (user_id, body, image) VALUES (" + str($me.id) + ", '" +
        esc($req.body.body) + "', '" + esc($image) + "');"
    );
    $res.status(201).json({
        "post": {
            "id": num($ins.lastInsertId),
            "userId": $me.id,
            "authorName": $me.displayName,
            "authorUsername": $me.username,
            "body": $req.body.body,
            "image": $image,
            "createdAt": "",
            "likes": 0,
            "comments": 0,
            "liked": false
        },
        "message": "Posted"
    });
}

def handleLikePost($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $pid = num($req.params.id);
    list $exists = sua.sqlite.query(
        "SELECT id FROM likes WHERE post_id = " + str($pid) + " AND user_id = " + str($me.id) + ";"
    );
    if (len($exists) > 0) {
        sua.sqlite.exec("DELETE FROM likes WHERE post_id = " + str($pid) + " AND user_id = " + str($me.id) + ";");
        list $cnt = sua.sqlite.query("SELECT COUNT(*) AS n FROM likes WHERE post_id = " + str($pid) + ";");
        $res.json({"liked": false, "likes": num($cnt[0].n)});
        return null;
    }
    sua.sqlite.exec(
        "INSERT INTO likes (post_id, user_id) VALUES (" + str($pid) + ", " + str($me.id) + ");"
    );
    list $owner = sua.sqlite.query("SELECT user_id FROM posts WHERE id = " + str($pid) + ";");
    if (len($owner) > 0 && num($owner[0].user_id) != $me.id) {
        notify(num($owner[0].user_id), "like", $me.displayName + " liked your post.", "/post/" + str($pid));
    }
    list $cnt = sua.sqlite.query("SELECT COUNT(*) AS n FROM likes WHERE post_id = " + str($pid) + ";");
    $res.json({"liked": true, "likes": num($cnt[0].n)});
}

def handleListComments($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $pid = num($req.params.id);
    list $rows = sua.sqlite.query(
        "SELECT c.id, c.body, c.created_at, u.id AS user_id, u.username, u.display_name " +
        "FROM comments c JOIN users u ON u.id = c.user_id " +
        "WHERE c.post_id = " + str($pid) + " ORDER BY c.created_at ASC;"
    );
    list $out = [];
    number $i = 0;
    each ($r in $rows) {
        $out[$i] = {
            "id": num($r.id),
            "body": $r.body,
            "createdAt": $r.created_at,
            "user": {
                "id": num($r.user_id),
                "username": $r.username,
                "displayName": $r.display_name
            }
        };
        $i = $i + 1;
    }
    $res.json({"comments": $out, "count": len($out)});
}

def handleCreateComment($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    if (!$req.body.body) {
        $res.status(400).json({"error": "body is required"});
        return null;
    }
    number $pid = num($req.params.id);
    dict $ins = sua.sqlite.exec(
        "INSERT INTO comments (post_id, user_id, body) VALUES (" + str($pid) + ", " + str($me.id) + ", '" + esc($req.body.body) + "');"
    );
    list $owner = sua.sqlite.query("SELECT user_id FROM posts WHERE id = " + str($pid) + ";");
    if (len($owner) > 0 && num($owner[0].user_id) != $me.id) {
        notify(num($owner[0].user_id), "comment", $me.displayName + " commented on your post.", "/post/" + str($pid));
    }
    $res.status(201).json({
        "comment": {
            "id": num($ins.lastInsertId),
            "body": $req.body.body,
            "createdAt": "",
            "user": {"id": $me.id, "username": $me.username, "displayName": $me.displayName}
        },
        "message": "Comment added"
    });
}

// ════════════════════════════════════════════════════════════════════
//  REAL-TIME CHAT (polling-based)
//  Client polls /api/messages/:userId?since=<id> every 1–2 seconds.
//  New messages with id > since are returned immediately.
// ════════════════════════════════════════════════════════════════════

def handleListMessages($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $peerId = num($req.params.id);
    number $since = 0;
    if ($req.query.since) { $since = num($req.query.since); }

    list $rows = sua.sqlite.query(
        "SELECT id, from_id, to_id, body, created_at, delivered FROM messages " +
        "WHERE ((from_id = " + str($me.id) + " AND to_id = " + str($peerId) + ") " +
        "OR (from_id = " + str($peerId) + " AND to_id = " + str($me.id) + ")) " +
        "AND id > " + str($since) + " ORDER BY id ASC LIMIT 200;"
    );

    // Mark inbound messages as delivered
    sua.sqlite.exec(
        "UPDATE messages SET delivered = 1 WHERE to_id = " + str($me.id) + " AND delivered = 0;"
    );

    list $out = [];
    number $i = 0;
    each ($r in $rows) {
        $out[$i] = {
            "id": num($r.id),
            "fromId": num($r.from_id),
            "toId": num($r.to_id),
            "body": $r.body,
            "createdAt": $r.created_at,
            "delivered": $r.delivered == 1
        };
        $i = $i + 1;
    }
    $res.json({"messages": $out, "count": len($out), "serverTime": nowMs()});
}

def handleSendMessage($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $peerId = num($req.params.id);
    if (!$req.body.body) {
        $res.status(400).json({"error": "body is required"});
        return null;
    }
    dict $ins = sua.sqlite.exec(
        "INSERT INTO messages (from_id, to_id, body) VALUES (" + str($me.id) + ", " + str($peerId) + ", '" + esc($req.body.body) + "');"
    );
    notify($peerId, "message", $me.displayName + " sent you a message.", "/chat/" + str($me.id));
    $res.status(201).json({
        "message": {
            "id": num($ins.lastInsertId),
            "fromId": $me.id,
            "toId": $peerId,
            "body": $req.body.body,
            "createdAt": "",
            "delivered": false
        }
    });
}

def handleConversations($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    // Last message per peer — use UNION to gather both directions,
    // then pick the max id per peer_id.
    list $rows = sua.sqlite.query(
        "SELECT m.id, m.from_id, m.to_id, m.body, m.created_at, m.delivered, " +
        "  CASE WHEN m.from_id = " + str($me.id) + " THEN m.to_id ELSE m.from_id END AS peer_id, " +
        "  u.username AS peer_username, u.display_name AS peer_display_name, u.last_seen AS peer_last_seen " +
        "FROM messages m " +
        "JOIN users u ON u.id = (CASE WHEN m.from_id = " + str($me.id) + " THEN m.to_id ELSE m.from_id END) " +
        "WHERE m.id IN (" +
        "  SELECT MAX(id) FROM (" +
        "    SELECT id, from_id AS peer_id FROM messages WHERE to_id = " + str($me.id) +
        "    UNION ALL" +
        "    SELECT id, to_id AS peer_id FROM messages WHERE from_id = " + str($me.id) +
        "  ) GROUP BY peer_id" +
        ") ORDER BY m.id DESC;"
    );
    list $out = [];
    number $i = 0;
    each ($r in $rows) {
        $out[$i] = {
            "peer": {
                "id": num($r.peer_id),
                "username": $r.peer_username,
                "displayName": $r.peer_display_name,
                "online": (nowMs() - num($r.peer_last_seen)) < 60000
            },
            "lastMessage": {
                "id": num($r.id),
                "fromId": num($r.from_id),
                "toId": num($r.to_id),
                "body": $r.body,
                "createdAt": $r.created_at,
                "delivered": $r.delivered == 1
            }
        };
        $i = $i + 1;
    }
    $res.json({"conversations": $out, "count": len($out)});
}

def handleUnreadCount($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    list $rows = sua.sqlite.query(
        "SELECT COUNT(*) AS n FROM messages WHERE to_id = " + str($me.id) + " AND delivered = 0;"
    );
    number $messages = num($rows[0].n);
    list $nrows = sua.sqlite.query(
        "SELECT COUNT(*) AS n FROM notifications WHERE user_id = " + str($me.id) + " AND is_read = 0;"
    );
    number $notifs = num($nrows[0].n);
    $res.json({"unreadMessages": $messages, "unreadNotifications": $notifs});
}

// ════════════════════════════════════════════════════════════════════
//  PRESENCE — clients POST /api/presence every 30s; we update last_seen.
//  Online = last_seen within last 60s.
// ════════════════════════════════════════════════════════════════════

def handlePresenceHeartbeat($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    sua.sqlite.exec("UPDATE users SET last_seen = " + str(nowMs()) + " WHERE id = " + str($me.id) + ";");
    $res.json({"ok": true, "online": true, "timestamp": nowMs()});
}

def handlePresenceList($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    list $rows = sua.sqlite.query(
        "SELECT id, username, display_name FROM users WHERE last_seen > " + str(nowMs() - 60000) +
        " AND id != " + str($me.id) + " ORDER BY display_name ASC LIMIT 200;"
    );
    list $out = [];
    number $i = 0;
    each ($r in $rows) {
        $out[$i] = {
            "id": num($r.id),
            "username": $r.username,
            "displayName": $r.display_name,
            "online": true
        };
        $i = $i + 1;
    }
    $res.json({"online": $out, "count": len($out)});
}

// ════════════════════════════════════════════════════════════════════
//  NOTIFICATIONS
// ════════════════════════════════════════════════════════════════════

def handleListNotifications($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    list $rows = sua.sqlite.query(
        "SELECT id, type, body, link, is_read, created_at FROM notifications WHERE user_id = " + str($me.id) +
        " ORDER BY id DESC LIMIT 50;"
    );
    list $out = [];
    number $i = 0;
    each ($r in $rows) {
        $out[$i] = {
            "id": num($r.id),
            "type": $r.type,
            "body": $r.body,
            "link": $r.link,
            "isRead": $r.is_read == 1,
            "createdAt": $r.created_at
        };
        $i = $i + 1;
    }
    // Mark as read
    sua.sqlite.exec("UPDATE notifications SET is_read = 1 WHERE user_id = " + str($me.id) + " AND is_read = 0;");
    $res.json({"notifications": $out, "count": len($out)});
}

// ════════════════════════════════════════════════════════════════════
//  WEBRTC SIGNALING — exchange offer/answer/ICE between two users.
//  Flow:
//    A: POST /api/call/offer/:to   {sdp}      →  store offer
//    B: GET  /api/call/offer/:fromA          →  fetch offer (consumed=1)
//    B: POST /api/call/answer/:toA  {sdp}     →  store answer
//    A: GET  /api/call/answer/:fromB         →  fetch answer (consumed=1)
//    A,B: POST /api/call/ice/:to  {candidate}  →  store ICE
//    A,B: GET  /api/call/ice/:from            →  fetch unconsumed ICE
//    A,B: POST /api/call/hangup/:to           →  store hangup signal
// ════════════════════════════════════════════════════════════════════

def handleCallOfferPost($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $toId = num($req.params.id);
    if (!$req.body.sdp) {
        $res.status(400).json({"error": "sdp is required"});
        return null;
    }
    // Remove any previous unconsumed offers from me to this peer
    sua.sqlite.exec(
        "DELETE FROM signaling WHERE from_id = " + str($me.id) + " AND to_id = " + str($toId) + " AND type = 'offer' AND consumed = 0;"
    );
    sua.sqlite.exec(
        "INSERT INTO signaling (from_id, to_id, type, payload) VALUES (" + str($me.id) + ", " + str($toId) + ", 'offer', '" + esc($req.body.sdp) + "');"
    );
    notify($toId, "call", $me.displayName + " is calling you.", "/call/" + str($me.id));
    $res.status(201).json({"ok": true, "message": "Offer sent"});
}

def handleCallOfferGet($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $fromId = num($req.params.id);
    list $rows = sua.sqlite.query(
        "SELECT id, payload, created_at FROM signaling WHERE from_id = " + str($fromId) +
        " AND to_id = " + str($me.id) + " AND type = 'offer' AND consumed = 0 ORDER BY id DESC LIMIT 1;"
    );
    if (len($rows) == 0) {
        $res.json({"hasOffer": false});
        return null;
    }
    sua.sqlite.exec("UPDATE signaling SET consumed = 1 WHERE id = " + str($rows[0].id) + ";");
    $res.json({
        "hasOffer": true,
        "fromId": $fromId,
        "sdp": $rows[0].payload,
        "createdAt": $rows[0].created_at
    });
}

def handleCallAnswerPost($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $toId = num($req.params.id);
    if (!$req.body.sdp) {
        $res.status(400).json({"error": "sdp is required"});
        return null;
    }
    sua.sqlite.exec(
        "INSERT INTO signaling (from_id, to_id, type, payload) VALUES (" + str($me.id) + ", " + str($toId) + ", 'answer', '" + esc($req.body.sdp) + "');"
    );
    $res.status(201).json({"ok": true, "message": "Answer sent"});
}

def handleCallAnswerGet($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $fromId = num($req.params.id);
    list $rows = sua.sqlite.query(
        "SELECT id, payload, created_at FROM signaling WHERE from_id = " + str($fromId) +
        " AND to_id = " + str($me.id) + " AND type = 'answer' AND consumed = 0 ORDER BY id DESC LIMIT 1;"
    );
    if (len($rows) == 0) {
        $res.json({"hasAnswer": false});
        return null;
    }
    sua.sqlite.exec("UPDATE signaling SET consumed = 1 WHERE id = " + str($rows[0].id) + ";");
    $res.json({
        "hasAnswer": true,
        "fromId": $fromId,
        "sdp": $rows[0].payload,
        "createdAt": $rows[0].created_at
    });
}

def handleCallIcePost($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $toId = num($req.params.id);
    if (!$req.body.candidate) {
        $res.status(400).json({"error": "candidate is required"});
        return null;
    }
    sua.sqlite.exec(
        "INSERT INTO signaling (from_id, to_id, type, payload) VALUES (" + str($me.id) + ", " + str($toId) + ", 'ice', '" + esc($req.body.candidate) + "');"
    );
    $res.status(201).json({"ok": true});
}

def handleCallIceGet($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $fromId = num($req.params.id);
    list $rows = sua.sqlite.query(
        "SELECT id, payload FROM signaling WHERE from_id = " + str($fromId) +
        " AND to_id = " + str($me.id) + " AND type = 'ice' AND consumed = 0 ORDER BY id ASC LIMIT 50;"
    );
    list $cands = [];
    number $i = 0;
    each ($r in $rows) {
        $cands[$i] = $r.payload;
        sua.sqlite.exec("UPDATE signaling SET consumed = 1 WHERE id = " + str($r.id) + ";");
        $i = $i + 1;
    }
    $res.json({"candidates": $cands, "count": len($cands)});
}

def handleCallHangup($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    number $toId = num($req.params.id);
    // Clear all signaling rows between the two
    sua.sqlite.exec(
        "DELETE FROM signaling WHERE (from_id = " + str($me.id) + " AND to_id = " + str($toId) + ") OR (from_id = " + str($toId) + " AND to_id = " + str($me.id) + ");"
    );
    notify($toId, "call", $me.displayName + " ended the call.", "");
    $res.json({"ok": true, "message": "Call ended"});
}

// Peek for an incoming call addressed to me WITHOUT consuming the offer.
// The global poller in api.js calls this to ring the Accept banner; the
// callee's call.html later performs the real (consuming) GET /api/call/offer.
// Returns the newest unconsumed offer's caller id, name and video flag.
def handleCallIncoming($req, $res) {
    dict $me = authUser($req);
    if (!$me) {
        $res.status(401).json({"error": "Not authenticated"});
        return null;
    }
    list $rows = sua.sqlite.query(
        "SELECT s.from_id AS from_id, u.display_name AS from_name, s.payload AS payload, s.created_at AS created_at " +
        "FROM signaling s JOIN users u ON u.id = s.from_id " +
        "WHERE s.to_id = " + str($me.id) + " AND s.type = 'offer' AND s.consumed = 0 " +
        "ORDER BY s.id DESC LIMIT 1;"
    );
    if (len($rows) == 0) {
        $res.json({"incoming": false});
        return null;
    }
    string $payload = $rows[0].payload;
    bool $hasVideo = contains($payload, "m=video");
    $res.json({
        "incoming": true,
        "fromId": num($rows[0].from_id),
        "fromName": $rows[0].from_name,
        "hasVideo": $hasVideo,
        "createdAt": $rows[0].created_at
    });
}

// ════════════════════════════════════════════════════════════════════
//  HEALTH & OPTIONS
// ════════════════════════════════════════════════════════════════════

def handleHealth($req, $res) {
    $res.json({
        "status": "ok",
        "language": "Bantu",
        "framework": "Sua",
        "database": "SQLite",
        "app": "ChatBantu",
        "version": "1.0.0",
        "serverTime": nowMs()
    });
}

def handleOptions($req, $res) {
    $res.set("Access-Control-Allow-Origin", "*");
    $res.set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    $res.set("Access-Control-Allow-Headers", "Content-Type, Authorization");
    $res.status(200).send("");
}

// ════════════════════════════════════════════════════════════════════
//  ROUTE REGISTRATION
// ════════════════════════════════════════════════════════════════════

sua.server.get("/api/health",                         handleHealth);

// Auth
sua.server.post("/api/auth/register",                 handleRegister);
sua.server.post("/api/auth/login",                    handleLogin);
sua.server.get("/api/auth/me",                        handleMe);

// Users & follows
sua.server.get("/api/users",                          handleListUsers);
sua.server.post("/api/users/:id/follow",              handleFollow);
sua.server.delete("/api/users/:id/follow",            handleUnfollow);

// Posts / feed
sua.server.get("/api/posts",                          handleListPosts);
sua.server.post("/api/posts",                         handleCreatePost);
sua.server.post("/api/posts/:id/like",                handleLikePost);
sua.server.get("/api/posts/:id/comments",             handleListComments);
sua.server.post("/api/posts/:id/comments",            handleCreateComment);

// Real-time chat (polling)
sua.server.get("/api/messages/:id",                   handleListMessages);
sua.server.post("/api/messages/:id",                  handleSendMessage);
sua.server.get("/api/conversations",                  handleConversations);
sua.server.get("/api/unread",                         handleUnreadCount);

// Presence
sua.server.post("/api/presence",                      handlePresenceHeartbeat);
sua.server.get("/api/presence",                       handlePresenceList);

// Notifications
sua.server.get("/api/notifications",                  handleListNotifications);

// WebRTC signaling
sua.server.post("/api/call/offer/:id",                handleCallOfferPost);
sua.server.get("/api/call/offer/:id",                 handleCallOfferGet);
sua.server.post("/api/call/answer/:id",               handleCallAnswerPost);
sua.server.get("/api/call/answer/:id",                handleCallAnswerGet);
sua.server.post("/api/call/ice/:id",                  handleCallIcePost);
sua.server.get("/api/call/ice/:id",                   handleCallIceGet);
sua.server.post("/api/call/hangup/:id",               handleCallHangup);
sua.server.get("/api/call/incoming",                  handleCallIncoming);

// CORS preflight
sua.server.options("/*",                              handleOptions);

// Static frontend
sua.server.static("./public");

// ════════════════════════════════════════════════════════════════════
//  START SERVER
// ════════════════════════════════════════════════════════════════════
print "";
print "═══════════════════════════════════════════";
print "  ChatBantu API ready on http://0.0.0.0:" + $envPort;
print "  Database: " + $dbPath + " (SQLite)";
print "  Real-time: HTTP polling (1-2s)";
print "  Video:    WebRTC + Bantu signaling";
print "  Endpoints:";
print "    POST   /api/auth/register";
print "    POST   /api/auth/login";
print "    GET    /api/auth/me";
print "    GET    /api/users";
print "    POST   /api/users/:id/follow";
print "    GET    /api/posts";
print "    POST   /api/posts";
print "    POST   /api/posts/:id/like";
print "    GET    /api/posts/:id/comments";
print "    POST   /api/posts/:id/comments";
print "    GET    /api/messages/:id?since=N";
print "    POST   /api/messages/:id";
print "    GET    /api/conversations";
print "    GET    /api/unread";
print "    POST   /api/presence";
print "    GET    /api/presence";
print "    GET    /api/notifications";
print "    POST   /api/call/offer/:id";
print "    GET    /api/call/offer/:id";
print "    POST   /api/call/answer/:id";
print "    GET    /api/call/answer/:id";
print "    POST   /api/call/ice/:id";
print "    GET    /api/call/ice/:id";
print "    POST   /api/call/hangup/:id";
print "═══════════════════════════════════════════";
print "";

sua.server.listen(num($envPort));
