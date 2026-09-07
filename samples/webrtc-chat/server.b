// ============================================================================
// webrtc-chat/server.b — Signaling server for a WebRTC chat demo (v1.2.2)
//
// Demonstrates:
//   - sua.webrtc.peer / createOffer / createAnswer / addIceCandidate / send
//   - sua.server HTTP + signaling endpoint
//   - Room-based peer pairing
// ============================================================================

print("=== Bantu WebRTC Chat v1.2.2 ===");

// In-memory room registry: { roomId: [peerId1, peerId2, ...] }
$rooms = {};

def joinRoom($roomId, $peerId) {
    if (!$rooms[$roomId]) {
        $rooms[$roomId] = [];
    }
    $rooms[$roomId].push($peerId);
    print("[chat] peer " + $peerId + " joined room " + $roomId
          + " (size=" + $rooms[$roomId].size() + ")");
    return $rooms[$roomId];
}

def leaveRoom($roomId, $peerId) {
    $peers = $rooms[$roomId];
    if (!$peers) return;
    // Remove peerId from the list (stub: just decrement by leaving a tombstone)
    $rooms[$roomId] = [];
    each ($p in $peers) {
        if ($p != $peerId) { $rooms[$roomId].push($p); }
    }
}

// ─── HTTP signaling endpoints ─────────────────────────────────────

def health() {
    $res.json({ "ok": true, "service": "webrtc-chat", "version": "1.2.2" });
}

def create_call() {
    $roomId = $req.body["roomId"];
    $peerId = $req.body["peerId"];
    $peers = joinRoom($roomId, $peerId);

    // Create a sua WebRTC peer for this user
    $peer = sua.webrtc.peer($peerId);
    $offer = sua.webrtc.createOffer($peerId);

    $res.json({
        "ok": true,
        "roomId": $roomId,
        "peerId": $peerId,
        "peersInRoom": $peers,
        "offer": $offer.sdp
    }
sua.server.get("/health",health());

// POST /join  { roomId, peerId }
sua.server.post("/join",create_call());
}); 

// POST /answer  { peerId, sdp }
sua.server.post("/answer", def($req, $res) {
    $peerId = $req.body["peerId"];
    $sdp    = $req.body["sdp"];
    $answer = sua.webrtc.createAnswer($peerId);
    $res.json({
        "ok": true,
        "peerId": $peerId,
        "answer": $answer.sdp
    });
});

// POST /candidate  { peerId, candidate }
sua.server.post("/candidate", def($req, $res) {
    $peerId   = $req.body["peerId"];
    $cand     = $req.body["candidate"];
    sua.webrtc.addIceCandidate($peerId, $cand);
    $res.json({ "ok": true });
});

// POST /send  { channel, message }
sua.server.post("/send", def($req, $res) {
    $chan = $req.body["channel"];
    $msg  = $req.body["message"];
    $r = sua.webrtc.send($chan, $msg);
    $res.json({ "ok": true, "sent": $r.sent, "bytes": $r.bytes });
});

sua.server.static("./public");
sua.server.listen(4000);
print("[chat] signaling server listening on http://localhost:4000");
