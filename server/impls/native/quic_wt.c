// quic_wt.c — QUIC (HTTP/3) + WebTransport front-end. See quic_wt.h.
//
// Structure mirrors quiche's own examples/http3-server.c for the QUIC accept /
// stateless-retry / version-negotiation boilerplate, but with libev and uthash
// replaced by a plain poll() loop and a small intrusive connection list (no
// extra deps), and extended with WebTransport (Extended CONNECT + DATAGRAMs)
// and a bridge onto the live game (game_bridge.h).
//
// Sharded across cores: N QUIC worker threads, each with its OWN UDP socket
// bound to the same port with SO_REUSEPORT, its own poll loop, its own
// connection list, and its own quiche config. The kernel load-balances inbound
// UDP by 4-tuple hash, exactly like the TCP acceptors, so QUIC's per-packet
// crypto/congestion work parallelizes.
//
// Migration caveat (documented, not hidden): the kernel hashes the 4-tuple,
// not the QUIC Connection ID. A connection stays on one worker for its whole
// life UNLESS the client migrates to a new 4-tuple (mobile network switch, NAT
// rebind); post-migration packets may then land on a different worker that
// lacks that connection's state. Migration is left ENABLED (it still works for
// a connection that stays on its worker); robust cross-worker migration needs
// eBPF Connection-ID steering (SO_ATTACH_REUSEPORT_EBPF), a documented
// follow-up. With one worker (--quic-workers=1) there is no such limitation.
#define _GNU_SOURCE
#include "quic_wt.h"
#include "game_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <quiche.h>

#define QW_CID_LEN       16              // our locally-chosen connection-id length
#define QW_MAX_DGRAM     1350            // conservative UDP payload (fits common MTUs)
#define QW_RECV_BUF      65535
#define QW_MAX_TOKEN     (sizeof("foolishq") - 1 + sizeof(struct sockaddr_storage) + QUICHE_MAX_CONN_ID_LEN)
#define QW_MAX_WORKERS   16

// The view (state_put) worst case is a few hundred bytes — comfortably inside
// one QUIC DATAGRAM. We still size the scratch generously and fall back to the
// WebTransport stream if a view ever exceeds the datagram budget.
#define QW_VIEW_CAP      65536

// ---- one QUIC connection (and, for us, at most one WebTransport session) ----
typedef struct QConn {
    uint8_t cid[QW_CID_LEN];
    quiche_conn *conn;
    quiche_h3_conn *h3;
    struct sockaddr_storage peer;
    socklen_t peer_len;

    // WebTransport session state. wt_stream is the CONNECT request stream id
    // (>= 0 once established); wt_flow_id is its "quarter stream id" — the
    // varint every WebTransport DATAGRAM on this session is prefixed with.
    int64_t  wt_stream;
    uint64_t wt_flow_id;
    bool     wt_ready;
    char     game_id[48];
    int      seat;
    char     token[128];

    struct QConn *next;
} QConn;

// ---- one worker: its own socket, config, poll loop and connection list ----
typedef struct QWorker {
    int idx;
    int sock;
    struct sockaddr_storage local;
    socklen_t local_len;
    quiche_config *cfg;
    quiche_h3_config *h3cfg;
    QConn *head;                          // intrusive singly-linked list of this worker's conns
} QWorker;

// -------------------------- QUIC varint (RFC 9000 §16) --------------------------
// WebTransport DATAGRAMs carry a leading varint flow id; encode/decode it here.
static int qw_varint_encode(uint64_t v, uint8_t *out) {
    if (v <= 63)               { out[0] = (uint8_t)v; return 1; }
    if (v <= 16383)            { out[0] = 0x40 | (uint8_t)(v >> 8); out[1] = (uint8_t)v; return 2; }
    if (v <= 1073741823ULL)    { out[0] = 0x80 | (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16);
                                 out[2] = (uint8_t)(v >> 8); out[3] = (uint8_t)v; return 4; }
    out[0] = 0xC0 | (uint8_t)(v >> 56); out[1] = (uint8_t)(v >> 48);
    out[2] = (uint8_t)(v >> 40); out[3] = (uint8_t)(v >> 32); out[4] = (uint8_t)(v >> 24);
    out[5] = (uint8_t)(v >> 16); out[6] = (uint8_t)(v >> 8); out[7] = (uint8_t)v; return 8;
}
// Decodes a varint from in[0..len); returns bytes consumed (>0) or 0 if short.
static int qw_varint_decode(const uint8_t *in, size_t len, uint64_t *out) {
    if (len < 1) return 0;
    int n = 1 << (in[0] >> 6);            // 1,2,4,8 by the top two bits
    if ((size_t)n > len) return 0;
    uint64_t v = in[0] & 0x3F;
    for (int i = 1; i < n; i++) v = (v << 8) | in[i];
    *out = v;
    return n;
}

// -------------------------- QUIC address-validation token --------------------------
// Same shape as quiche's example: "foolishq" | client addr | original dcid.
static void qw_mint_token(const uint8_t *dcid, size_t dcid_len,
                          struct sockaddr_storage *addr, socklen_t addr_len,
                          uint8_t *token, size_t *token_len) {
    memcpy(token, "foolishq", sizeof("foolishq") - 1);
    memcpy(token + sizeof("foolishq") - 1, addr, addr_len);
    memcpy(token + sizeof("foolishq") - 1 + addr_len, dcid, dcid_len);
    *token_len = sizeof("foolishq") - 1 + addr_len + dcid_len;
}
static bool qw_validate_token(const uint8_t *token, size_t token_len,
                              struct sockaddr_storage *addr, socklen_t addr_len,
                              uint8_t *odcid, size_t *odcid_len) {
    if (token_len < sizeof("foolishq") - 1 || memcmp(token, "foolishq", sizeof("foolishq") - 1)) return false;
    token += sizeof("foolishq") - 1; token_len -= sizeof("foolishq") - 1;
    if (token_len < addr_len || memcmp(token, addr, addr_len)) return false;
    token += addr_len; token_len -= addr_len;
    if (*odcid_len < token_len) return false;
    memcpy(odcid, token, token_len); *odcid_len = token_len;
    return true;
}

static bool qw_gen_cid(uint8_t *cid, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    ssize_t r = read(fd, cid, len);
    close(fd);
    return r == (ssize_t)len;
}

// -------------------------- connection map (per-worker intrusive list) --------------------------
static QConn *qw_find(QWorker *w, const uint8_t *dcid, size_t dcid_len) {
    if (dcid_len != QW_CID_LEN) return NULL;   // we always issue QW_CID_LEN cids
    for (QConn *c = w->head; c; c = c->next)
        if (memcmp(c->cid, dcid, QW_CID_LEN) == 0) return c;
    return NULL;
}

static QConn *qw_create(QWorker *w, const uint8_t *dcid, size_t dcid_len,
                        const uint8_t *odcid, size_t odcid_len,
                        struct sockaddr_storage *peer, socklen_t peer_len) {
    if (dcid_len != QW_CID_LEN) return NULL;
    QConn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    memcpy(c->cid, dcid, QW_CID_LEN);
    c->wt_stream = -1;
    quiche_conn *conn = quiche_accept(c->cid, QW_CID_LEN, odcid, odcid_len,
                                      (struct sockaddr *)&w->local, w->local_len,
                                      (struct sockaddr *)peer, peer_len, w->cfg);
    if (!conn) { free(c); return NULL; }
    c->conn = conn;
    memcpy(&c->peer, peer, peer_len);
    c->peer_len = peer_len;
    c->next = w->head;
    w->head = c;
    return c;
}

static void qw_free(QConn *c) {
    if (c->h3)   quiche_h3_conn_free(c->h3);
    if (c->conn) quiche_conn_free(c->conn);
    free(c);
}

// -------------------------- egress --------------------------
static void qw_flush(QWorker *w, QConn *c) {
    uint8_t out[QW_MAX_DGRAM];
    quiche_send_info si;
    for (;;) {
        ssize_t written = quiche_conn_send(c->conn, out, sizeof out, &si);
        if (written == QUICHE_ERR_DONE) break;
        if (written < 0) return;
        sendto(w->sock, out, written, 0, (struct sockaddr *)&si.to, si.to_len);
    }
}

// Send bytes as a WebTransport DATAGRAM on this connection's session: prefix
// the quarter-stream-id varint, then the payload, then hand to QUIC. Silently
// drops if it wouldn't fit the datagram budget (unreliable by design).
static void qw_wt_datagram_send(QConn *c, const uint8_t *payload, size_t len) {
    if (!c->wt_ready) return;
    uint8_t buf[QW_MAX_DGRAM];
    int hn = qw_varint_encode(c->wt_flow_id, buf);
    if ((size_t)hn + len > sizeof buf) return;
    memcpy(buf + hn, payload, len);
    ssize_t cap = quiche_conn_dgram_max_writable_len(c->conn);
    if (cap < 0 || (size_t)cap < (size_t)hn + len) return;   // peer's datagram window too small right now
    quiche_conn_dgram_send(c->conn, buf, (size_t)hn + len);
}

// -------------------------- HTTP/3 request headers --------------------------
typedef struct {
    char method[16];
    char path[512];
    char protocol[32];
} QwHdrs;

static int qw_hdr_cb(uint8_t *name, size_t nlen, uint8_t *val, size_t vlen, void *argp) {
    QwHdrs *h = argp;
    #define QW_EQ(s) (nlen == sizeof(s) - 1 && memcmp(name, s, nlen) == 0)
    if (QW_EQ(":method"))   { size_t n = vlen < sizeof h->method   - 1 ? vlen : sizeof h->method   - 1; memcpy(h->method, val, n);   h->method[n] = 0; }
    else if (QW_EQ(":path")) { size_t n = vlen < sizeof h->path     - 1 ? vlen : sizeof h->path     - 1; memcpy(h->path, val, n);     h->path[n] = 0; }
    else if (QW_EQ(":protocol")) { size_t n = vlen < sizeof h->protocol - 1 ? vlen : sizeof h->protocol - 1; memcpy(h->protocol, val, n); h->protocol[n] = 0; }
    #undef QW_EQ
    return 0;
}

// Pull a decimal ?key=value (or &key=value) out of a query string. Returns
// true and fills *out if present.
static bool qw_query_int(const char *path, const char *key, int *out) {
    char pat[24]; snprintf(pat, sizeof pat, "%s=", key);
    const char *p = strstr(path, pat);
    if (!p) return false;
    *out = (int)strtol(p + strlen(pat), NULL, 10);
    return true;
}
static void qw_query_str(const char *path, const char *key, char *out, size_t cap) {
    out[0] = 0;
    char pat[24]; snprintf(pat, sizeof pat, "%s=", key);
    const char *p = strstr(path, pat);
    if (!p) return;
    p += strlen(pat);
    size_t i = 0;
    while (p[i] && p[i] != '&' && i < cap - 1) { out[i] = p[i]; i++; }
    out[i] = 0;
}

static void qw_send_simple(QConn *c, int64_t stream, const char *status,
                           const char *body, size_t body_len) {
    quiche_h3_header hs[] = {
        { .name = (const uint8_t *)":status", .name_len = 7,
          .value = (const uint8_t *)status, .value_len = strlen(status) },
    };
    quiche_h3_send_response(c->h3, c->conn, stream, hs, 1, body_len == 0);
    if (body_len) quiche_h3_send_body(c->h3, c->conn, stream, (uint8_t *)body, body_len, true);
}

// A WebTransport CONNECT: validate the seat/token against the live game, accept
// the session (200, stream stays open), and push the seat's current view as the
// first DATAGRAM so the client renders immediately.
static void qw_handle_wt_connect(QConn *c, int64_t stream, const QwHdrs *h) {
    char game_id[48]; qw_query_str(h->path, "game_id", game_id, sizeof game_id);
    char token[128];  qw_query_str(h->path, "token",   token,   sizeof token);
    int seat = -1;    qw_query_int(h->path, "seat", &seat);

    unsigned char view[QW_VIEW_CAP];
    int vn = gb_apply_move(game_id, token, seat, NULL, 0, view, sizeof view);
    if (vn < 0) { qw_send_simple(c, stream, "403", NULL, 0); return; }   // bad auth / game / seat

    // 200 with no fin — the CONNECT stream stays open for the session lifetime.
    quiche_h3_header ok[] = {
        { .name = (const uint8_t *)":status", .name_len = 7, .value = (const uint8_t *)"200", .value_len = 3 },
        { .name = (const uint8_t *)"sec-webtransport-http3-draft", .name_len = 28, .value = (const uint8_t *)"draft02", .value_len = 7 },
    };
    quiche_h3_send_response(c->h3, c->conn, stream, ok, 2, false);

    c->wt_stream  = stream;
    c->wt_flow_id = (uint64_t)stream / 4;   // quarter stream id
    c->wt_ready   = true;
    c->seat       = seat;
    snprintf(c->game_id, sizeof c->game_id, "%s", game_id);
    snprintf(c->token,   sizeof c->token,   "%s", token);

    qw_wt_datagram_send(c, view, (size_t)vn);   // initial state push
    fprintf(stderr, "quic/wt: session up stream=%lld game=%s seat=%d (pushed %d-byte view)\n",
            (long long)stream, game_id, seat, vn);
}

// A plain HTTP/3 request (not WebTransport). We serve the read-only endpoints
// the game exposes over HTTP: /health and /state.
static void qw_handle_h3_request(QConn *c, int64_t stream, const QwHdrs *h) {
    if (strcmp(h->method, "GET") != 0) { qw_send_simple(c, stream, "405", NULL, 0); return; }

    if (strncmp(h->path, "/health", 7) == 0) {
        qw_send_simple(c, stream, "200", "{\"ok\":true}", 11);
        return;
    }
    if (strncmp(h->path, "/state", 6) == 0) {
        char game_id[48]; qw_query_str(h->path, "game_id", game_id, sizeof game_id);
        int seat = -1;    qw_query_int(h->path, "seat", &seat);
        unsigned char view[QW_VIEW_CAP];
        int vn = gb_state_for(game_id, seat, view, sizeof view);
        if (vn < 0) { qw_send_simple(c, stream, "404", NULL, 0); return; }
        quiche_h3_header hs[] = {
            { .name = (const uint8_t *)":status", .name_len = 7, .value = (const uint8_t *)"200", .value_len = 3 },
            { .name = (const uint8_t *)"content-type", .name_len = 12,
              .value = (const uint8_t *)"application/octet-stream", .value_len = 24 },
        };
        quiche_h3_send_response(c->h3, c->conn, stream, hs, 2, false);
        quiche_h3_send_body(c->h3, c->conn, stream, view, (size_t)vn, true);
        return;
    }
    qw_send_simple(c, stream, "404", NULL, 0);
}

// Drain any WebTransport DATAGRAMs the client sent: strip the flow-id varint,
// treat the payload as a move, apply it, and push the fresh view back.
static void qw_drain_datagrams(QConn *c) {
    if (!c->wt_ready) return;
    uint8_t buf[QW_MAX_DGRAM];
    for (;;) {
        ssize_t n = quiche_conn_dgram_recv(c->conn, buf, sizeof buf);
        if (n < 0) break;   // QUICHE_ERR_DONE
        uint64_t flow = 0;
        int hn = qw_varint_decode(buf, (size_t)n, &flow);
        if (hn <= 0 || flow != c->wt_flow_id) continue;   // not for this session
        const uint8_t *move = buf + hn;
        int mlen = (int)n - hn;
        unsigned char view[QW_VIEW_CAP];
        int vn = gb_apply_move(c->game_id, c->token, c->seat, move, mlen, view, sizeof view);
        if (vn >= 0) qw_wt_datagram_send(c, view, (size_t)vn);
    }
}

// -------------------------- per-connection HTTP/3 processing --------------------------
static void qw_process_h3(QWorker *w, QConn *c) {
    if (!quiche_conn_is_established(c->conn)) return;
    if (!c->h3) {
        c->h3 = quiche_h3_conn_new_with_transport(c->conn, w->h3cfg);
        if (!c->h3) return;
    }
    quiche_h3_event *ev;
    for (;;) {
        int64_t stream = quiche_h3_conn_poll(c->h3, c->conn, &ev);
        if (stream < 0) break;
        switch (quiche_h3_event_type(ev)) {
            case QUICHE_H3_EVENT_HEADERS: {
                QwHdrs h; memset(&h, 0, sizeof h);
                quiche_h3_event_for_each_header(ev, qw_hdr_cb, &h);
                if (strcmp(h.method, "CONNECT") == 0 && strcmp(h.protocol, "webtransport") == 0)
                    qw_handle_wt_connect(c, stream, &h);
                else
                    qw_handle_h3_request(c, stream, &h);
                break;
            }
            case QUICHE_H3_EVENT_DATA:
            case QUICHE_H3_EVENT_FINISHED:
            case QUICHE_H3_EVENT_RESET:
            case QUICHE_H3_EVENT_PRIORITY_UPDATE:
            case QUICHE_H3_EVENT_GOAWAY:
            default:
                break;
        }
        quiche_h3_event_free(ev);
    }
    qw_drain_datagrams(c);
}

// -------------------------- ingress --------------------------
static void qw_recv_all(QWorker *w) {
    uint8_t buf[QW_RECV_BUF];
    uint8_t out[QW_MAX_DGRAM];
    for (;;) {
        struct sockaddr_storage peer; socklen_t peer_len = sizeof peer;
        memset(&peer, 0, sizeof peer);
        ssize_t r = recvfrom(w->sock, buf, sizeof buf, 0, (struct sockaddr *)&peer, &peer_len);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }

        uint8_t type;
        uint32_t version;
        uint8_t scid[QUICHE_MAX_CONN_ID_LEN]; size_t scid_len = sizeof scid;
        uint8_t dcid[QUICHE_MAX_CONN_ID_LEN]; size_t dcid_len = sizeof dcid;
        uint8_t odcid[QUICHE_MAX_CONN_ID_LEN]; size_t odcid_len = sizeof odcid;
        uint8_t token[QW_MAX_TOKEN]; size_t token_len = sizeof token;

        int rc = quiche_header_info(buf, (size_t)r, QW_CID_LEN, &version, &type,
                                    scid, &scid_len, dcid, &dcid_len, token, &token_len);
        if (rc < 0) continue;

        QConn *c = qw_find(w, dcid, dcid_len);
        if (!c) {
            // A packet for a connection we don't know yet: negotiate version,
            // do a stateless retry to validate the client's address, then
            // accept once the retry token comes back.
            if (!quiche_version_is_supported(version)) {
                ssize_t wr = quiche_negotiate_version(scid, scid_len, dcid, dcid_len, out, sizeof out);
                if (wr > 0) sendto(w->sock, out, wr, 0, (struct sockaddr *)&peer, peer_len);
                continue;
            }
            if (token_len == 0) {
                uint8_t new_cid[QW_CID_LEN];
                if (!qw_gen_cid(new_cid, QW_CID_LEN)) continue;
                qw_mint_token(dcid, dcid_len, &peer, peer_len, token, &token_len);
                ssize_t wr = quiche_retry(scid, scid_len, dcid, dcid_len,
                                          new_cid, QW_CID_LEN, token, token_len,
                                          version, out, sizeof out);
                if (wr > 0) sendto(w->sock, out, wr, 0, (struct sockaddr *)&peer, peer_len);
                continue;
            }
            odcid_len = sizeof odcid;
            if (!qw_validate_token(token, token_len, &peer, peer_len, odcid, &odcid_len)) continue;
            c = qw_create(w, dcid, dcid_len, odcid, odcid_len, &peer, peer_len);
            if (!c) continue;
        }

        quiche_recv_info ri = {
            (struct sockaddr *)&peer, peer_len,
            (struct sockaddr *)&w->local, w->local_len,
        };
        if (quiche_conn_recv(c->conn, buf, (size_t)r, &ri) < 0) continue;
    }
}

// -------------------------- config / event loop --------------------------
static quiche_config *qw_config_new(const char *cert_path, const char *key_path) {
    quiche_config *cfg = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (!cfg) return NULL;
    if (quiche_config_load_cert_chain_from_pem_file(cfg, cert_path) < 0 ||
        quiche_config_load_priv_key_from_pem_file(cfg, key_path) < 0) {
        fprintf(stderr, "quic/wt: failed to load cert (%s) / key (%s)\n", cert_path, key_path);
        quiche_config_free(cfg);
        return NULL;
    }
    quiche_config_set_application_protos(cfg, (uint8_t *)QUICHE_H3_APPLICATION_PROTOCOL,
                                         sizeof(QUICHE_H3_APPLICATION_PROTOCOL) - 1);
    quiche_config_set_max_idle_timeout(cfg, 30000);
    quiche_config_set_max_recv_udp_payload_size(cfg, QW_MAX_DGRAM);
    quiche_config_set_max_send_udp_payload_size(cfg, QW_MAX_DGRAM);
    quiche_config_set_initial_max_data(cfg, 10000000);
    quiche_config_set_initial_max_stream_data_bidi_local(cfg, 1000000);
    quiche_config_set_initial_max_stream_data_bidi_remote(cfg, 1000000);
    quiche_config_set_initial_max_stream_data_uni(cfg, 1000000);
    quiche_config_set_initial_max_streams_bidi(cfg, 100);
    quiche_config_set_initial_max_streams_uni(cfg, 100);
    quiche_config_set_disable_active_migration(cfg, false);   // migration ON — QUIC's headline feature for mobile
    quiche_config_set_cc_algorithm(cfg, QUICHE_CC_CUBIC);
    quiche_config_enable_dgram(cfg, true, 1024, 1024);        // WebTransport DATAGRAMs
    return cfg;
}

// Build one worker's UDP socket (its own SO_REUSEPORT listener) + config.
// Returns true on success.
static bool qw_worker_init(QWorker *w, int idx, int port, const char *cert, const char *key) {
    w->idx = idx;
    w->head = NULL;
    w->cfg = qw_config_new(cert, key);
    if (!w->cfg) return false;
    w->h3cfg = quiche_h3_config_new();
    if (!w->h3cfg) { quiche_config_free(w->cfg); return false; }
    quiche_h3_config_enable_extended_connect(w->h3cfg, true);   // WebTransport needs Extended CONNECT

    w->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (w->sock < 0) { perror("quic/wt: socket"); return false; }
    int one = 1;
    setsockopt(w->sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(w->sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);   // kernel spreads UDP across workers
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons((uint16_t)port);
    if (bind(w->sock, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("quic/wt: bind"); return false; }
    fcntl(w->sock, F_SETFL, fcntl(w->sock, F_GETFL, 0) | O_NONBLOCK);
    memcpy(&w->local, &addr, sizeof addr);
    w->local_len = sizeof addr;
    return true;
}

// One worker's event loop: poll its socket, service its connections, reap.
static void qw_worker_loop(QWorker *w) {
    struct pollfd pfd = { .fd = w->sock, .events = POLLIN };
    for (;;) {
        // Wake at the soonest connection timeout so quiche's loss/idle timers fire.
        int timeout = 1000;
        for (QConn *c = w->head; c; c = c->next) {
            uint64_t t = quiche_conn_timeout_as_millis(c->conn);
            if (t < (uint64_t)timeout) timeout = (int)t;
        }
        int pr = poll(&pfd, 1, timeout);
        if (pr < 0 && errno != EINTR) break;

        if (pr > 0 && (pfd.revents & POLLIN)) qw_recv_all(w);

        // Fire elapsed timeouts, run H3, flush egress.
        for (QConn *c = w->head; c; c = c->next) {
            if (quiche_conn_timeout_as_millis(c->conn) == 0) quiche_conn_on_timeout(c->conn);
            qw_process_h3(w, c);
            qw_flush(w, c);
        }

        // Reap closed connections.
        QConn **pp = &w->head;
        while (*pp) {
            QConn *c = *pp;
            if (quiche_conn_is_closed(c->conn)) { *pp = c->next; qw_free(c); }
            else pp = &c->next;
        }
    }
}

static void *qw_worker_thread(void *arg) {
    qw_worker_loop((QWorker *)arg);
    return NULL;
}

int quic_wt_run(int port, int workers, const char *cert_path, const char *key_path) {
    if (workers < 1) workers = 1;
    if (workers > QW_MAX_WORKERS) workers = QW_MAX_WORKERS;

    static QWorker w[QW_MAX_WORKERS];
    int n = 0;
    for (int i = 0; i < workers; i++) {
        if (!qw_worker_init(&w[i], i, port, cert_path, key_path)) {
            if (n == 0) return 1;                 // couldn't stand up even one worker — fatal
            fprintf(stderr, "quic/wt: warning: only %d/%d QUIC workers started\n", n, workers);
            break;
        }
        n++;
    }

    fprintf(stderr, "foolish QUIC/HTTP3/WebTransport listener on udp :%d "
            "(h3, extended-connect, datagrams, workers=%d)\n", port, n);

    // Spawn workers [1..n-1] as threads; run worker[0] on this (QUIC) thread so
    // the QUIC thread blocks here forever, exactly like the TCP acceptors.
    for (int i = 1; i < n; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, qw_worker_thread, &w[i]) == 0) pthread_detach(t);
        else fprintf(stderr, "quic/wt: warning: QUIC worker %d thread failed to start\n", i);
    }
    qw_worker_loop(&w[0]);   // never returns
    return 0;
}
