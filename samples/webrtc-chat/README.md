# Bantu WebRTC Chat Sample

A small WebRTC signaling server + chat UI built entirely in Bantu v1.2.2.

## Run

```bash
bantu run server.b
# → http://localhost:4000
```

## What it does

- `POST /join` — joins a room, creates a `sua.webrtc.peer`, returns an SDP offer
- `POST /answer` — generates an SDP answer for a peer
- `POST /candidate` — relays an ICE candidate to a peer
- `POST /send` — sends a message over a `sua.webrtc` data channel
- `GET /health` — health check

## Files

```
webrtc-chat/
├── server.b          # Signaling server (uses sua.webrtc.* API)
├── public/
│   └── index.html    # Browser UI — joins room, sends messages
└── bantu.json
```

## Notes

- The default build uses the in-process WebRTC stub. To run against a real `rtc::PeerConnection`, build Bantu with `cmake .. -DBANTU_WEBRTC=ON` (requires libdatachannel).
- The signaling protocol is intentionally minimal — production deployments should add auth, persistence, and proper SDP exchange ordering.
