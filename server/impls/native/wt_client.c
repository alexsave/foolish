// wt_client.c — a tiny WebTransport-over-HTTP/3 smoke-test client for the
// QUIC front-end (quic_wt.c), the WebTransport analogue of what foolish_hammer
// is for WebSocket. It performs a real QUIC handshake, opens a WebTransport
// session via HTTP/3 Extended CONNECT, receives the server's initial view
// DATAGRAM, sends a move DATAGRAM back, and receives the resulting view
// DATAGRAM — then prints PASS/FAIL. One connection, one poll() loop.
//
// Builds only in the QUIC toolchain (links libquiche.a, no OpenSSL) — see the
// `wt_client` target in the Makefile and quic_test.sh.
//   usage: wt_client <host> <port> <path>
//     path e.g. "/wt?token=<bearer>&game_id=<id>&seat=0"
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>

#include <quiche.h>

static uint64_t wtc_now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
static int wtc_cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

#define WTC_MAX_DGRAM 1350

// QUIC varint (RFC 9000 §16) — WebTransport DATAGRAMs are prefixed with the
// session's quarter-stream-id as a varint.
static int wtc_varint_encode(uint64_t v, uint8_t *o) {
    if (v <= 63)    { o[0] = (uint8_t)v; return 1; }
    if (v <= 16383) { o[0] = 0x40 | (uint8_t)(v >> 8); o[1] = (uint8_t)v; return 2; }
    o[0] = 0x80 | (uint8_t)(v >> 24); o[1] = (uint8_t)(v >> 16);
    o[2] = (uint8_t)(v >> 8); o[3] = (uint8_t)v; return 4;
}
static int wtc_varint_decode(const uint8_t *in, size_t len, uint64_t *out) {
    if (len < 1) return 0;
    int n = 1 << (in[0] >> 6);
    if ((size_t)n > len) return 0;
    uint64_t v = in[0] & 0x3F;
    for (int i = 1; i < n; i++) v = (v << 8) | in[i];
    *out = v;
    return n;
}

// Drain quiche's egress queue to the socket.
static void wtc_flush(int sock, quiche_conn *conn, uint8_t *out, size_t out_cap) {
    quiche_send_info si;
    for (;;) {
        ssize_t w = quiche_conn_send(conn, out, out_cap, &si);
        if (w == QUICHE_ERR_DONE || w < 0) break;
        send(sock, out, w, 0);
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s host port path\n", argv[0]); return 2; }
    const char *host = argv[1], *port = argv[2], *path = argv[3];
    int hold_ticks = (argc > 4) ? atoi(argv[4]) * 10 : 0;   // seconds to hold the session open after the round trip

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *peer;
    if (getaddrinfo(host, port, &hints, &peer) != 0) { perror("getaddrinfo"); return 1; }
    int sock = socket(peer->ai_family, SOCK_DGRAM, 0);
    fcntl(sock, F_SETFL, O_NONBLOCK);
    if (connect(sock, peer->ai_addr, peer->ai_addrlen) < 0) { perror("connect"); return 1; }
    struct sockaddr_storage local; socklen_t local_len = sizeof local;
    getsockname(sock, (struct sockaddr *)&local, &local_len);

    quiche_config *cfg = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    quiche_config_set_application_protos(cfg, (uint8_t *)QUICHE_H3_APPLICATION_PROTOCOL,
                                         sizeof(QUICHE_H3_APPLICATION_PROTOCOL) - 1);
    quiche_config_verify_peer(cfg, false);   // self-signed test cert, same posture as foolish_hammer --tls
    quiche_config_set_max_idle_timeout(cfg, 5000);
    quiche_config_set_max_recv_udp_payload_size(cfg, WTC_MAX_DGRAM);
    quiche_config_set_max_send_udp_payload_size(cfg, WTC_MAX_DGRAM);
    quiche_config_set_initial_max_data(cfg, 10000000);
    quiche_config_set_initial_max_stream_data_bidi_local(cfg, 1000000);
    quiche_config_set_initial_max_stream_data_bidi_remote(cfg, 1000000);
    quiche_config_set_initial_max_stream_data_uni(cfg, 1000000);
    quiche_config_set_initial_max_streams_bidi(cfg, 100);
    quiche_config_set_initial_max_streams_uni(cfg, 100);
    quiche_config_enable_dgram(cfg, true, 1024, 1024);

    uint8_t scid[16];
    int rng = open("/dev/urandom", O_RDONLY);
    if (rng < 0 || read(rng, scid, sizeof scid) != (ssize_t)sizeof scid) { perror("urandom"); return 1; }
    close(rng);

    quiche_conn *conn = quiche_connect(host, scid, sizeof scid,
                                       (struct sockaddr *)&local, local_len,
                                       peer->ai_addr, peer->ai_addrlen, cfg);
    if (!conn) { fprintf(stderr, "quiche_connect failed\n"); return 1; }

    quiche_h3_config *h3cfg = quiche_h3_config_new();
    quiche_h3_config_enable_extended_connect(h3cfg, true);
    quiche_h3_conn *h3 = NULL;

    uint8_t buf[65535], out[WTC_MAX_DGRAM];
    quiche_recv_info ri;
    bool req_sent = false, got_headers = false, move_sent = false;
    int datagrams_recv = 0;
    int64_t wt_stream = -1; uint64_t wt_flow = 0;

    // Ping mode (5th arg N>0): after the session is up, ping-pong N move/view
    // DATAGRAMs measuring each round-trip, then print RTT percentiles. Used to
    // measure QUIC/WebTransport latency (e.g. --quic-workers sharding effect).
    int npings = (argc > 5) ? atoi(argv[5]) : 0;
    int pings_done = 0; bool awaiting = false; uint64_t ping_t0 = 0;
    static uint64_t rtt[20000]; int nrtt = 0;

    wtc_flush(sock, conn, out, sizeof out);

    struct pollfd pfd = { .fd = sock, .events = POLLIN };
    for (int iter = 0; iter < 2000; iter++) {
        int t = (int)quiche_conn_timeout_as_millis(conn);
        if (t < 0 || t > 100) t = 100;
        int pr = poll(&pfd, 1, t);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            for (;;) {
                ssize_t r = recv(sock, buf, sizeof buf, 0);
                if (r < 0) break;   // EAGAIN — drained
                ri.from = (struct sockaddr *)peer->ai_addr; ri.from_len = peer->ai_addrlen;
                ri.to = (struct sockaddr *)&local; ri.to_len = local_len;
                quiche_conn_recv(conn, buf, r, &ri);
            }
        } else {
            quiche_conn_on_timeout(conn);
        }

        if (quiche_conn_is_established(conn)) {
            if (!h3) h3 = quiche_h3_conn_new_with_transport(conn, h3cfg);
            if (h3 && !req_sent) {
                quiche_h3_header hs[] = {
                    { (uint8_t *)":method", 7, (uint8_t *)"CONNECT", 7 },
                    { (uint8_t *)":scheme", 7, (uint8_t *)"https", 5 },
                    { (uint8_t *)":authority", 10, (uint8_t *)host, strlen(host) },
                    { (uint8_t *)":path", 5, (uint8_t *)path, strlen(path) },
                    { (uint8_t *)":protocol", 9, (uint8_t *)"webtransport", 12 },
                };
                wt_stream = quiche_h3_send_request(h3, conn, hs, 5, false);
                if (wt_stream >= 0) { wt_flow = (uint64_t)wt_stream / 4; req_sent = true; }
            }
            if (h3) {
                quiche_h3_event *ev;
                for (;;) {
                    int64_t s = quiche_h3_conn_poll(h3, conn, &ev);
                    if (s < 0) break;
                    if (quiche_h3_event_type(ev) == QUICHE_H3_EVENT_HEADERS) {
                        got_headers = true;
                        fprintf(stderr, "[wt_client] response headers on stream %lld\n", (long long)s);
                    }
                    quiche_h3_event_free(ev);
                }
            }
            for (;;) {
                ssize_t n = quiche_conn_dgram_recv(conn, buf, sizeof buf);
                if (n < 0) break;
                uint64_t flow = 0; int hn = wtc_varint_decode(buf, (size_t)n, &flow);
                datagrams_recv++;
                if (npings > 0) {
                    if (awaiting && nrtt < (int)(sizeof rtt / sizeof rtt[0])) {
                        rtt[nrtt++] = wtc_now_us() - ping_t0;   // a view DATAGRAM in reply to our move = one RTT
                        awaiting = false; pings_done++;
                    }
                    continue;
                }
                fprintf(stderr, "[wt_client] recv WT datagram: flow=%llu payload=%d bytes\n",
                        (unsigned long long)flow, (int)n - hn);
                if (!move_sent) {   // send one move datagram back to exercise the inbound path
                    uint8_t d[64]; int p = wtc_varint_encode(wt_flow, d);
                    d[p++] = 0x00; d[p++] = 0x01;   // bytes rejected as illegal in a WAITING game — echoes the view
                    quiche_conn_dgram_send(conn, d, p);
                    move_sent = true;
                    fprintf(stderr, "[wt_client] sent move datagram (flow=%llu)\n", (unsigned long long)wt_flow);
                }
            }
            // Ping driver: once the session is up and no ping is outstanding,
            // fire the next move DATAGRAM and start its RTT timer.
            if (npings > 0 && got_headers && datagrams_recv >= 1 && !awaiting && pings_done < npings) {
                uint8_t d[64]; int p = wtc_varint_encode(wt_flow, d); d[p++] = 0x00; d[p++] = 0x01;
                quiche_conn_dgram_send(conn, d, p);
                ping_t0 = wtc_now_us(); awaiting = true;
            }
        }

        wtc_flush(sock, conn, out, sizeof out);

        if (npings > 0) {
            if (pings_done >= npings) break;
            continue;   // ping mode drives its own loop; skip the smoke-mode hold/break below
        }
        // Optional 4th arg: hold the session open ~N seconds after the round
        // trip (≈10 poll ticks/sec) so a test can observe that an OPEN WT
        // session keeps its game pinned against reclamation.
        if (got_headers && datagrams_recv >= 2) {
            if (hold_ticks <= 0) break;
            hold_ticks--;
        }
    }

    if (npings > 0) {
        if (nrtt == 0) { printf("FAIL: no RTT samples (session=%s)\n", got_headers ? "up" : "down"); return 1; }
        qsort(rtt, nrtt, sizeof rtt[0], wtc_cmp_u64);
        double sum = 0; for (int i = 0; i < nrtt; i++) sum += rtt[i];
        printf("RTT us: n=%d mean=%.1f p50=%llu p90=%llu p99=%llu max=%llu\n",
               nrtt, sum / nrtt,
               (unsigned long long)rtt[nrtt / 2],
               (unsigned long long)rtt[(nrtt * 9) / 10],
               (unsigned long long)rtt[(nrtt * 99) / 100],
               (unsigned long long)rtt[nrtt - 1]);
        return 0;
    }

    if (got_headers && datagrams_recv >= 2) {
        printf("PASS: WebTransport session established, %d view datagrams round-tripped\n", datagrams_recv);
        return 0;
    }
    if (got_headers && datagrams_recv >= 1) {
        printf("PARTIAL: session up, %d view datagram(s) received\n", datagrams_recv);
        return 0;
    }
    printf("FAIL: session=%s datagrams=%d\n", got_headers ? "up" : "down", datagrams_recv);
    return 1;
}
