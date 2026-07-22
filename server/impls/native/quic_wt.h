// quic_wt.h — QUIC (HTTP/3) + WebTransport front-end for foolish_server.
//
// A UDP/QUIC listener that speaks HTTP/3 and WebTransport, bridging onto the
// SAME in-memory game the TCP (WebSocket) front-end serves (via game_bridge.h).
// Built only in the QUIC target (foolish_server_quic, -DFOOLISH_QUIC), which
// links Cloudflare quiche (libquiche.a, bundling its own BoringSSL) and does
// NOT link OpenSSL — so this is a separate build from the default server.
//
// Why QUIC/WebTransport (see SERVER_SCALING.md "recent network tech"): QUIC
// carries TLS 1.3 in the transport, survives IP/network changes via connection
// migration (a phone moving cell->wifi keeps its game), and its unreliable
// DATAGRAMs are a natural fit for the push-only game view (drop a stale frame
// rather than head-of-line-block behind it, which a WebSocket over TCP can't
// do). WebTransport is the browser-reachable API over HTTP/3.
#ifndef FOOLISH_QUIC_WT_H
#define FOOLISH_QUIC_WT_H

// Run the QUIC/HTTP3/WebTransport server on UDP `port`, loading the TLS 1.3
// cert/key from the given PEM paths (QUIC has no plaintext mode). Blocking —
// call on its own thread. Returns non-zero only on fatal setup failure
// (bad socket / config / cert); otherwise runs until the process exits.
int quic_wt_run(int port, const char *cert_path, const char *key_path);

#endif
