// chessbench.c - c_chess's load generator, and its house bot: N bots, each with its own account and
// socket, seek, get paired with each other, and play real games over the real protocol through the
// same client a browser runs (c/src/client.c): every move legal, every clock real.
//
//   chessbench [--port 8900] [--bots 100] [--tc 180+2] [--think 200] [--spread 200] [--watchers 0]
//              [--duration 60] [--warmup 10] [--pid <server pid>] [--accounts <file>] [--connect-rate 200]
//              [--rated 1] [--forever] [--arena <id>] [--arena-make <seconds> [--arena-name <name>]] [--chat]
//
// THE NUMBERS (after the warm-up, for `duration` seconds):
//   move round trip   a bot's move sent to the server's state push of that ply back on its own socket
//   move relay        a bot's move sent to its opponent's socket receiving it (the other side of the board)
//   games and moves per second; the server's RSS and CPU (from --pid), hence CPU per move
// Every sample is kept and sorted: the percentiles are exact (nearest rank).
//
// THE ACCOUNTS. Signing up is bcrypt on the server (about 60 ms each), so the bots' sessions are kept
// in --accounts (default build/bench_accounts.bin) and reused while their tokens live; run the server
// with a long CBASE_JWT_TTL for long runs.
//
// --forever with --bots 1 is the house bot a person can play: it seeks the time control, plays its
// moves after `think` ms, and seeks again when a game ends.
//
// --arena <id>: the bots join that arena (on every connect: a withdrawn bot comes back) instead of
// seeking, and play the games the arena pairs them in; a game's end seeks nothing.
// --arena-make <seconds>: the first bot makes an arena of the time control (--tc, --rated) that
// starts 5 s later and lasts that long, named --arena-name (default "Bench Arena"), and every bot
// joins it as above; its id is printed.
// --chat: a bot says "gl hf" in its game's room when a game starts and "gg" when it ends (a live chat
// to look at, and a load test of the chat: lines said and refused, and lines heard, are counted).
#include "client.h"

#include "net/loop.h"
#include "net/ws.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/str.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach_time.h>
#endif

static struct {
    int port, bots, base, inc, think, spread, watchers, duration, warmup, connect_rate, rated;
    pid_t pid;
    bool forever, quiet, tls, resume, chat;
    int handshakes, threads, ai, arena_make;
    uint32_t arena;
    const char *arena_name;
    const char *accounts, *prefix, *groups;
} O = { .port = 8900, .bots = 100, .base = 180, .inc = 2, .think = 200, .spread = 200, .duration = 60, .warmup = 10,
        .connect_rate = 200, .rated = 1, .resume = true, .threads = 4, .accounts = "build/bench_accounts.bin", .prefix = "bot" };

static cb_loop *L;

/* ---- samples ---- */
typedef struct { uint32_t *v; size_t n, cap; } samples;
static void sample(samples *s, int64_t us) {
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 4096; s->v = realloc(s->v, s->cap * sizeof *s->v); }
    s->v[s->n++] = us < 0 ? 0 : (uint32_t)us;
}
static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static uint32_t pct(const samples *s, double p) {
    if (!s->n) return 0;
    size_t i = (size_t)(p * (double)s->n);
    return s->v[i >= s->n ? s->n - 1 : i];
}

/* ---- the bots ---- */
typedef struct bot {
    cc_client c;
    int       idx;
    char      name[CW_NAME + 1], password[24];
    uint8_t   session[sizeof(cw_session) + CC_TOKEN_MAX];
    size_t    session_len;
    int       fd, state;       /* 0 none, 1 connecting, 2 upgrading, 3 open */
    cb_buf    in, out;
    uint64_t  move_timer;
    int64_t   sent_us;         /* our move in flight: when it left */
    uint16_t  sent_ply;
    uint32_t  sent_game;
    int64_t   last_sent_us;    /* the same, kept after our own confirmation (the relay's start) */
    uint32_t  playing;         /* the game we are in */
    bool      watcher;
    bool      joined;          /* --arena: our join was sent on this connection */
    SSL      *ssl;             /* --tls: the socket's TLS */
} bot;

/* --tls: one client context; the first session a bot gets is offered by every later connect, as a
 * browser's reconnects resume (--resume 0: never). */
static SSL_CTX *TLS_CTX;
static SSL_SESSION *TLS_SESS;
static long tls_full, tls_resumed;

static bot *B;
static bool measuring, stopping;
static samples rt_us, relay_us;
static long moves_measured, games_measured, games_ended, errors;
static long bytes_in, bytes_out, frames_in, frames_out;
static long chat_said, chat_refused, chat_heard;   /* --chat, while measuring */
static int n_open;

/* game id -> the two bots, for the relay timing (open addressing, ids are dense) */
#define GAME_SLOTS (1 << 20)
static int32_t *GAME_BOT;   /* [2 * slot] */

static int64_t now_us(void) { return cb_mono_us(); }
static int64_t now_ms_d(void) { return cb_mono_ms(); }

static void bot_close(bot *b) {
    if (b->ssl) { SSL_free(b->ssl); b->ssl = NULL; ERR_clear_error(); }
    if (b->fd >= 0) { cb_loop_remove(L, b->fd); close(b->fd); b->fd = -1; if (b->state == 3) n_open--; }
    b->state = 0;
}

/* send or SSL_write: the bytes moved, 0 for "not now", -1 failed */
static ssize_t bot_send(bot *b, const void *p, size_t n) {
    if (!b->ssl) {
        ssize_t k = send(b->fd, p, n, MSG_DONTWAIT);
        if (k < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
        return k;
    }
    int k = SSL_write(b->ssl, p, (int)n);
    if (k > 0) return k;
    int e = SSL_get_error(b->ssl, k);
    return e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ ? 0 : -1;
}

/* recv or SSL_read: the bytes, 0 closed, -2 "nothing now", -1 failed */
static ssize_t bot_recv(bot *b, void *p, size_t n) {
    if (!b->ssl) {
        ssize_t k = recv(b->fd, p, n, MSG_DONTWAIT);
        if (k < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? -2 : -1;
        return k;
    }
    int k = SSL_read(b->ssl, p, (int)n);
    if (k > 0) return k;
    int e = SSL_get_error(b->ssl, k);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return -2;
    return e == SSL_ERROR_ZERO_RETURN ? 0 : -1;
}

static void bot_flush(bot *b) {
    const uint8_t *f;
    size_t n;
    while ((n = cc_client_next_out(&b->c, &f)) > 0) {
        uint8_t hdr[14], mask[4];
        uint32_t r = (uint32_t)rand();
        memcpy(mask, &r, 4);
        size_t hl = cb_ws_frame_header(hdr, true, CB_WS_OP_BIN, n, mask);
        cb_buf_append(&b->out, hdr, hl);
        size_t at = b->out.len;
        cb_buf_append(&b->out, f, n);
        cb_ws_unmask(b->out.p + at, n, mask);
        frames_out++;
    }
    while (b->out.len && b->fd >= 0 && b->state >= 2 && b->state != 4) {
        ssize_t k = bot_send(b, b->out.p, b->out.len);
        if (k < 0) { bot_close(b); return; }
        if (k == 0) break;
        bytes_out += k;
        cb_buf_consume(&b->out, (size_t)k);
    }
    if (b->fd >= 0) cb_loop_set(L, b->fd, CB_EV_READ | (b->out.len ? CB_EV_WRITE : 0));
}

static void seek(bot *b) {
    if (stopping) return;
    if (O.arena_make && !O.arena) {   /* the arena is not made yet: the first bot makes it, the rest wait */
        static bool asked;
        if (b->idx || asked) return;
        asked = true;
        cw_arena_req q;
        memset(&q, 0, sizeof q);
        q.base_s = (uint16_t)O.base;
        q.inc_s = (uint8_t)O.inc;
        q.rated = (uint8_t)O.rated;
        q.start_in_s = 5;
        q.duration_s = (uint32_t)O.arena_make;
        const char *nm = O.arena_name ? O.arena_name : "Bench Arena";
        memcpy(q.name, nm, strlen(nm) < CW_ARENA_NAME ? strlen(nm) : CW_ARENA_NAME);
        cc_client_post(&b->c, "/c/arena", &q, sizeof q, 2);
        bot_flush(b);
        return;
    }
    if (O.arena) {   /* --arena: the arena pairs us (a join of a joined bot is refused, harmlessly) */
        cw_id q = { O.arena };
        cc_client_post(&b->c, "/c/arena/join", &q, sizeof q, 1);
        b->joined = true;
        bot_flush(b);
        return;
    }
    if (O.ai) { cc_client_ai(&b->c, O.base, O.inc, O.ai, 2); bot_flush(b); return; }   /* --ai L: against the computer */
    cc_client_seek(&b->c, O.base, O.inc, O.rated);
    bot_flush(b);
}

static void play_move(cb_loop *l, void *ud) {
    bot *b = ud;
    b->move_timer = 0;
    if (b->state != 3 || !cc_client_my_turn(&b->c)) return;
    ch_moves ms;
    ch_legal(&b->c.g.pos, &ms);
    if (!ms.n) return;
    /* a little better than random: a capture or a check half the time when there is one */
    ch_move m = ms.m[rand() % ms.n];
    if (rand() & 1)
        for (int i = 0; i < ms.n; i++)
            if (b->c.g.pos.sq[CH_TO(ms.m[i])]) { m = ms.m[i]; break; }
    int64_t t = now_us();
    if (cc_client_move(&b->c, CH_FROM(m), CH_TO(m), CH_PROMO(m), now_ms_d()) == CC_OK) {
        b->sent_us = t;
        b->last_sent_us = t;
        b->sent_ply = b->c.g.ply;
        b->sent_game = b->c.game;
        bot_flush(b);
    }
}

static void schedule_move(bot *b) {
    if (b->move_timer || !cc_client_my_turn(&b->c)) return;
    int d = O.think + (O.spread ? rand() % O.spread : 0);
    b->move_timer = cb_loop_timer(L, d, play_move, b);
}

/* --chat: a line in the game's room; the answer is a read's, tagged 3. */
static void say(bot *b, uint32_t game, const char *text) {
    char room[CW_ROOM];
    snprintf(room, sizeof room, "g%u", game);
    cc_client_chat_say(&b->c, room, text, strlen(text), 3);
    bot_flush(b);
}

static void on_events(bot *b, uint32_t ev) {
    if ((ev & CC_EV_READ) && b->c.read_tag == 3 && measuring) {   /* --chat: a line's answer */
        if (b->c.read_status == 200) chat_said++;
        else chat_refused++;
    }
    if ((ev & CC_EV_CHAT) && measuring) chat_heard++;
    if ((ev & CC_EV_READ) && b->c.read_tag == 2 && !O.arena) {   /* --arena-make: the arena made */
        if (b->c.read_status != 200 || !cc_client_arena_take(&b->c, b->c.read_buf, b->c.read_len, now_ms_d(), 1)) {
            fprintf(stderr, "chessbench: the arena was refused (%d)\n", b->c.read_status);
            stopping = true;
            cb_loop_stop(L);
            return;
        }
        cw_arena_hdr h;
        memcpy(&h, b->c.arena, sizeof h);
        O.arena = h.id;
        printf("arena %u\n", h.id);
        fflush(stdout);
        for (int i = 0; i < O.bots; i++)   /* everyone already in waits for it */
            if (B[i].state == 3 && B[i].c.ready && !B[i].watcher && !B[i].joined) seek(&B[i]);
    }
    if (ev & CC_EV_READY) {
        b->joined = false;
        if (b->watcher) return;
        if (b->playing) { cc_client_open(&b->c, b->playing); bot_flush(b); }
        else seek(b);
    }
    if (ev & CC_EV_ERROR) errors++;
    if (ev & CC_EV_START) {
        uint32_t g = b->c.start_game;
        if (b->playing != g) {
            b->playing = g;
            GAME_BOT[2 * (g % GAME_SLOTS) + b->c.start_color] = b->idx;
            cc_client_open(&b->c, g);
            bot_flush(b);
            if (O.chat) say(b, g, "gl hf");
            /* the watchers follow the first games they hear of */
            for (int i = 0, w = 0; i < O.bots && w < O.watchers && b->c.start_color == 0; i++) {
                bot *x = &B[i];
                if (x->watcher && !x->c.game && x->state == 3) { cc_client_open(&x->c, g); bot_flush(x); w++; }
            }
        }
    }
    if (ev & CC_EV_GAME) {
        cc_client *c = &b->c;
        /* our move came back */
        if (b->sent_us && c->game == b->sent_game && c->confirmed_ply >= b->sent_ply) {
            if (measuring) { sample(&rt_us, now_us() - b->sent_us); moves_measured++; }
            b->sent_us = 0;
        }
        if (c->synced && !cc_game_over(&c->g)) schedule_move(b);
    }
    if (ev & CC_EV_MOVED) {
        /* the opponent's move arrived: its send time is on the opponent's bot */
        uint32_t g = b->c.game;
        int other = GAME_BOT[2 * (g % GAME_SLOTS) + (b->c.color ^ 1)];
        if (other >= 0 && other < O.bots) {
            bot *o = &B[other];
            if (measuring && o->sent_game == g && o->sent_ply == b->c.g.ply && o->last_sent_us) { sample(&relay_us, now_us() - o->last_sent_us); o->last_sent_us = 0; }
        }
    }
    if (ev & CC_EV_OVER) {
        if (b->watcher) { cc_client_close_game(&b->c); bot_flush(b); return; }
        if (b->c.color == 0) { games_ended++; if (measuring) games_measured++; }
        if (O.chat && b->c.game) say(b, b->c.game, "gg");
        b->playing = 0;
        cc_client_close_game(&b->c);
        bot_flush(b);
        if (!O.arena && (O.forever || !stopping)) seek(b);
    }
}

static void bot_frames(bot *b) {
    for (;;) {
        cb_ws_frame f;
        int n = cb_ws_frame_parse(b->in.p, b->in.len, CB_WS_MAX_MESSAGE, &f);
        if (n == 0) return;
        if (n < 0) { bot_close(b); return; }
        uint8_t *payload = b->in.p + f.header_len;
        frames_in++;
        if (f.opcode == CB_WS_OP_BIN) {
            cc_client_in(&b->c, payload, (size_t)f.len, now_ms_d());
            on_events(b, cc_client_events(&b->c));
        } else if (f.opcode == CB_WS_OP_PING) {
            uint8_t hdr[14], mask[4] = { 1, 2, 3, 4 };
            size_t hl = cb_ws_frame_header(hdr, true, CB_WS_OP_PONG, f.len, mask);
            cb_buf_append(&b->out, hdr, hl);
            size_t at = b->out.len;
            cb_buf_append(&b->out, payload, (size_t)f.len);
            cb_ws_unmask(b->out.p + at, (size_t)f.len, mask);
        } else if (f.opcode == CB_WS_OP_CLOSE) { bot_close(b); return; }
        cb_buf_consume(&b->in, (size_t)n);
    }
}

static void bot_io(cb_loop *l, int fd, int events, void *ud) {
    bot *b = ud;
    if (b->state == 1) {
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { bot_close(b); return; }
        b->state = 2;
        if (TLS_CTX) {
            b->ssl = SSL_new(TLS_CTX);
            SSL_set_fd(b->ssl, fd);
            SSL_set_tlsext_host_name(b->ssl, "localhost");
            if (O.resume && TLS_SESS) SSL_set_session(b->ssl, TLS_SESS);
            b->state = 4;
        }
    }
    if (b->state == 4) {   /* the TLS handshake */
        int r = SSL_connect(b->ssl);
        if (r != 1) {
            int e = SSL_get_error(b->ssl, r);
            if (e == SSL_ERROR_WANT_READ) { cb_loop_set(L, fd, CB_EV_READ); return; }
            if (e == SSL_ERROR_WANT_WRITE) { cb_loop_set(L, fd, CB_EV_READ | CB_EV_WRITE); return; }
            bot_close(b);
            return;
        }
        if (SSL_session_reused(b->ssl)) tls_resumed++; else tls_full++;
        b->state = 2;
        bot_flush(b);   /* the upgrade request */
        return;
    }
    if (events & CB_EV_ERR) { bot_close(b); return; }
    if (events & CB_EV_READ) {
        for (;;) {
            if (!cb_buf_reserve(&b->in, 16384)) { bot_close(b); return; }
            ssize_t k = bot_recv(b, b->in.p + b->in.len, 16384);
            if (k == -2) break;
            if (k <= 0) { bot_close(b); return; }
            bytes_in += k;
            b->in.len += (size_t)k;
            b->in.p[b->in.len] = 0;
            if ((size_t)k < 16384 && !b->ssl) break;   /* TLS: read until OpenSSL has nothing more */
        }
        if (b->state == 2) {
            char *end = strstr((char *)b->in.p, "\r\n\r\n");
            if (!end) return;
            if (strncmp((char *)b->in.p, "HTTP/1.1 101", 12) != 0) { fprintf(stderr, "upgrade refused: %.60s\n", (char *)b->in.p); bot_close(b); return; }
            cb_buf_consume(&b->in, (size_t)(end + 4 - (char *)b->in.p));
            b->state = 3;
            n_open++;
            if (b->ssl && !TLS_SESS) TLS_SESS = SSL_get1_session(b->ssl);   /* its tickets came before this answer */
            cc_client_hello(&b->c);
        }
        if (b->state == 3) bot_frames(b);
    }
    if (b->fd >= 0) bot_flush(b);
}

static int bot_connect(bot *b) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)O.port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    b->fd = fd;
    b->state = 1;
    cb_buf_reset(&b->out);
    cb_buf_reset(&b->in);
    cb_buf_printf(&b->out, "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nUser-Agent: chessbench\r\n\r\n");
    return cb_loop_add(L, fd, CB_EV_WRITE | CB_EV_READ, bot_io, b) == CB_OK ? 0 : -1;
}

static void reconnect_tick(cb_loop *l, void *ud);
static int next_connect;
static void connect_tick(cb_loop *l, void *ud) {
    int per = O.connect_rate / 10 + 1;
    for (int k = 0; k < per && next_connect < O.bots; k++, next_connect++) bot_connect(&B[next_connect]);
    if (next_connect < O.bots) cb_loop_timer(L, 100, connect_tick, NULL);
    else cb_loop_timer(L, 1000, reconnect_tick, NULL);
}
/* A bot whose socket closed comes back (a token refused is fatal: say so). */
static void reconnect_tick(cb_loop *l, void *ud) {
    if (stopping) return;
    for (int i = 0; i < O.bots; i++)
        if (B[i].fd < 0 && B[i].state == 0) bot_connect(&B[i]);
    cb_loop_timer(L, 1000, reconnect_tick, NULL);
}

/* ---- accounts (blocking HTTP, before the loop) ---- */

static int http_post(const char *path, const void *body, size_t n, uint8_t *out, size_t cap, size_t *out_n) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)O.port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    char head[256];
    int hl = snprintf(head, sizeof head, "POST %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/octet-stream\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", path, n);
    static __thread char buf[16384];
    size_t got = 0;
    ssize_t k;
    if (TLS_CTX) {
        SSL *s = SSL_new(TLS_CTX);
        SSL_set_fd(s, fd);
        if (SSL_connect(s) != 1 || SSL_write(s, head, hl) != hl || SSL_write(s, body, (int)n) != (int)n) { SSL_free(s); close(fd); ERR_clear_error(); return -1; }
        while ((k = SSL_read(s, buf + got, (int)(sizeof buf - 1 - got))) > 0) got += (size_t)k;
        SSL_free(s);
        ERR_clear_error();
    } else {
        if (write(fd, head, (size_t)hl) != hl || write(fd, body, n) != (ssize_t)n) { close(fd); return -1; }
        while ((k = read(fd, buf + got, sizeof buf - 1 - got)) > 0) got += (size_t)k;
    }
    close(fd);
    buf[got] = 0;
    int status = 0;
    if (sscanf(buf, "HTTP/1.1 %d", &status) != 1) return -1;
    char *body_at = strstr(buf, "\r\n\r\n");
    if (!body_at) return -1;
    body_at += 4;
    *out_n = got - (size_t)(body_at - buf);
    if (*out_n > cap) return -1;
    memcpy(out, body_at, *out_n);
    return status;
}

static int next_account;
static pthread_mutex_t acct_mu = PTHREAD_MUTEX_INITIALIZER;

static void *account_worker(void *ud) {
    (void)ud;
    for (;;) {
        pthread_mutex_lock(&acct_mu);
        int i = next_account++;
        pthread_mutex_unlock(&acct_mu);
        if (i >= O.bots) return NULL;
        bot *b = &B[i];
        if (b->session_len) continue;
        cw_auth_req q;
        int st = 0;
        for (int mode = CW_AUTH_SIGNIN; mode <= CW_AUTH_SIGNIN + 1 && st != 200; mode++) {
            cc_client_auth_body(b->name, b->password, mode == CW_AUTH_SIGNIN ? CW_AUTH_SIGNIN : CW_AUTH_SIGNUP, &q);
            st = http_post("/c/auth", &q, sizeof q, b->session, sizeof b->session, &b->session_len);
        }
        if (st != 200) { fprintf(stderr, "account %s: %d\n", b->name, st); b->session_len = 0; }
    }
}

static void accounts(void) {
    FILE *f = fopen(O.accounts, "rb");
    int loaded = 0;
    if (f) {
        uint32_t n;
        while (fread(&n, 4, 1, f) == 1) {
            char name[CW_NAME + 1] = { 0 };
            uint32_t len;
            if (fread(name, CW_NAME, 1, f) != 1 || fread(&len, 4, 1, f) != 1 || len > sizeof B[0].session) break;
            uint8_t tmp[sizeof B[0].session];
            if (fread(tmp, len, 1, f) != 1) break;
            if (n < (uint32_t)O.bots && strcmp(name, B[n].name) == 0) { memcpy(B[n].session, tmp, len); B[n].session_len = len; loaded++; }
        }
        fclose(f);
    }
    int64_t t0 = now_ms_d();
    pthread_t th[8];
    for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, account_worker, NULL);
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    if (!O.quiet) fprintf(stderr, "accounts: %d reused, %d signed in or up in %lld ms\n", loaded, O.bots - loaded, (long long)(now_ms_d() - t0));
    if ((f = fopen(O.accounts, "wb"))) {
        for (uint32_t i = 0; i < (uint32_t)O.bots; i++) {
            uint32_t len = (uint32_t)B[i].session_len;
            if (!len) continue;
            char name[CW_NAME] = { 0 };
            memcpy(name, B[i].name, strlen(B[i].name));
            fwrite(&i, 4, 1, f); fwrite(name, CW_NAME, 1, f); fwrite(&len, 4, 1, f); fwrite(B[i].session, len, 1, f);
        }
        fclose(f);
    }
    for (int i = 0; i < O.bots; i++) cc_client_auth_answer(&B[i].c, 200, B[i].session, B[i].session_len);
}

/* ---- the server's numbers ---- */
static bool proc_usage(pid_t pid, uint64_t *rss, uint64_t *user_us, uint64_t *sys_us) {
    if (!pid) return false;
#ifdef __APPLE__
    struct rusage_info_v2 ri;
    if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *)&ri) != 0) return false;
    /* the times are mach absolute time units, not nanoseconds (125/3 ns each on Apple silicon) */
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    /* the physical footprint (dirty memory, compressed included), not the resident size: macOS
     * compresses and purges idle pages, so a resident size can fall while memory is in use */
    *rss = ri.ri_phys_footprint;
    *user_us = ri.ri_user_time * tb.numer / tb.denom / 1000;
    *sys_us = ri.ri_system_time * tb.numer / tb.denom / 1000;
    return true;
#else
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    unsigned long ut = 0, st = 0;
    long rss_pages = 0;
    int ok = fscanf(f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %ld", &ut, &st, &rss_pages);
    fclose(f);
    long hz = sysconf(_SC_CLK_TCK);
    *rss = (uint64_t)rss_pages * (uint64_t)sysconf(_SC_PAGESIZE);
    *user_us = ut * 1000000ULL / (uint64_t)hz;
    *sys_us = st * 1000000ULL / (uint64_t)hz;
    return ok == 3;
#endif
}

static uint64_t rss0, u0, s0, rss_open;
static int64_t t_measure;
static long moves_at_start;

static void stop_tick(cb_loop *l, void *ud) { stopping = true; cb_loop_stop(L); }
static void measure_tick(cb_loop *l, void *ud) {
    measuring = true;
    t_measure = now_ms_d();
    proc_usage(O.pid, &rss_open, &u0, &s0);
    if (!O.quiet) fprintf(stderr, "measuring for %d s: %d sockets open, %ld games ended in the warm-up\n", O.duration, n_open, games_ended);
    cb_loop_timer(L, O.duration * 1000, stop_tick, NULL);
}

/* --handshakes N: TLS connections with one request each (connect, handshake, GET /health, close) from --threads
 * blocking threads, full or resumed (--resume), and the server's CPU per handshake. */
static atomic_long hs_left, hs_done, hs_resumed, hs_failed;
static void *hs_worker(void *ud) {
    (void)ud;
    SSL_SESSION *sess = NULL;
    while (atomic_fetch_sub(&hs_left, 1) > 0) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)O.port) };
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); atomic_fetch_add(&hs_failed, 1); continue; }
        if (!TLS_CTX) {   /* the same connection and request in plaintext: the baseline TLS adds to */
            static const char rq[] = "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            char b[512];
            if (write(fd, rq, sizeof rq - 1) > 0) { while (read(fd, b, sizeof b) > 0) {} atomic_fetch_add(&hs_done, 1); }
            else atomic_fetch_add(&hs_failed, 1);
            close(fd);
            continue;
        }
        SSL *s = SSL_new(TLS_CTX);
        SSL_set_fd(s, fd);
        if (O.resume && sess) SSL_set_session(s, sess);
        if (SSL_connect(s) == 1) {
            atomic_fetch_add(&hs_done, 1);
            if (SSL_session_reused(s)) atomic_fetch_add(&hs_resumed, 1);
            /* one small request on it, as a page's first one: its answer comes after TLS 1.3's tickets,
             * and with --resume the newest ticket is the one the next connection offers (a ticket
             * is for one use) */
            static const char rq[] = "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            char b[512];
            if (SSL_write(s, rq, (int)sizeof rq - 1) > 0)
                while (SSL_read(s, b, sizeof b) > 0) {}
            if (O.resume) {
                if (sess) SSL_SESSION_free(sess);
                sess = SSL_get1_session(s);
            }
            SSL_shutdown(s);
        } else atomic_fetch_add(&hs_failed, 1);
        SSL_free(s);
        ERR_clear_error();
        close(fd);
    }
    if (sess) SSL_SESSION_free(sess);
    return NULL;
}

static int handshake_bench(void) {
    atomic_store(&hs_left, O.handshakes);
    uint64_t rss0_, u0_ = 0, s0_ = 0, u1_ = 0, s1_ = 0, rss1_;
    proc_usage(O.pid, &rss0_, &u0_, &s0_);
    int64_t t0 = now_ms_d();
    pthread_t th[64];
    int n = O.threads < 1 ? 1 : O.threads > 64 ? 64 : O.threads;
    for (int i = 0; i < n; i++) pthread_create(&th[i], NULL, hs_worker, NULL);
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    double secs = (double)(now_ms_d() - t0) / 1000.0;
    bool have = proc_usage(O.pid, &rss1_, &u1_, &s1_);
    long done = atomic_load(&hs_done), res = atomic_load(&hs_resumed);
    printf("handshakes: %ld (%ld resumed, %ld failed) from %d threads in %.2f s = %.0f/s\n", done, res, atomic_load(&hs_failed), n, secs, done / secs);
    if (have && done) {
        double cpu = (double)((u1_ - u0_) + (s1_ - s0_));
        printf("server: cpu %.2f s = %.1f us per handshake, %.0f handshakes per second per core\n", cpu / 1e6, cpu / done, done / (cpu / 1e6));
    }
    return 0;
}

static void on_sigint(int s) { (void)s; stopping = true; if (L) cb_loop_stop(L); }

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--port")) O.port = atoi(v), i++;
        else if (!strcmp(a, "--bots")) O.bots = atoi(v), i++;
        else if (!strcmp(a, "--tc")) { sscanf(v, "%d+%d", &O.base, &O.inc); i++; }
        else if (!strcmp(a, "--think")) O.think = atoi(v), i++;
        else if (!strcmp(a, "--spread")) O.spread = atoi(v), i++;
        else if (!strcmp(a, "--watchers")) O.watchers = atoi(v), i++;
        else if (!strcmp(a, "--duration")) O.duration = atoi(v), i++;
        else if (!strcmp(a, "--warmup")) O.warmup = atoi(v), i++;
        else if (!strcmp(a, "--pid")) O.pid = (pid_t)atoi(v), i++;
        else if (!strcmp(a, "--accounts")) O.accounts = v, i++;
        else if (!strcmp(a, "--prefix")) O.prefix = v, i++;
        else if (!strcmp(a, "--connect-rate")) O.connect_rate = atoi(v), i++;
        else if (!strcmp(a, "--rated")) O.rated = atoi(v), i++;
        else if (!strcmp(a, "--forever")) O.forever = true;
        else if (!strcmp(a, "--quiet")) O.quiet = true;
        else if (!strcmp(a, "--tls")) O.tls = true;
        else if (!strcmp(a, "--ai")) O.ai = atoi(v), i++;
        else if (!strcmp(a, "--arena")) O.arena = (uint32_t)strtoul(v, NULL, 10), i++;
        else if (!strcmp(a, "--arena-make")) O.arena_make = atoi(v), i++;
        else if (!strcmp(a, "--arena-name")) O.arena_name = v, i++;
        else if (!strcmp(a, "--chat")) O.chat = true;
        else if (!strcmp(a, "--groups")) O.groups = v, i++;   /* TLS key exchange groups, e.g. X25519 (default: OpenSSL's, X25519MLKEM768 first) */
        else if (!strcmp(a, "--resume")) O.resume = atoi(v) != 0, i++;
        else if (!strcmp(a, "--handshakes")) O.handshakes = atoi(v), i++;
        else if (!strcmp(a, "--threads")) O.threads = atoi(v), i++;
        else { fprintf(stderr, "chessbench: unknown option %s\n", a); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    srand((unsigned)getpid());
    ch_init();
    if (O.tls) {
        TLS_CTX = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(TLS_CTX, TLS1_3_VERSION);
        SSL_CTX_set_session_cache_mode(TLS_CTX, SSL_SESS_CACHE_CLIENT);
        if (O.groups && SSL_CTX_set1_groups_list(TLS_CTX, O.groups) != 1) { fprintf(stderr, "chessbench: bad --groups %s\n", O.groups); return 2; }
    }
    if (O.handshakes) return handshake_bench();
    int total = O.bots + O.watchers;
    B = calloc((size_t)total, sizeof *B);
    GAME_BOT = malloc(sizeof *GAME_BOT * 2 * GAME_SLOTS);
    if (!B || !GAME_BOT) { fprintf(stderr, "chessbench: out of memory\n"); return 1; }
    memset(GAME_BOT, 0xff, sizeof *GAME_BOT * 2 * GAME_SLOTS);
    for (int i = 0; i < total; i++) {
        bot *b = &B[i];
        cc_client_init(&b->c);
        b->idx = i;
        b->fd = -1;
        b->watcher = i >= O.bots;
        snprintf(b->name, sizeof b->name, "%s%d", O.prefix, i);
        snprintf(b->password, sizeof b->password, "bench-pw-%d-x", i);
        cb_buf_init(&b->in);
        cb_buf_init(&b->out);
    }
    int players = O.bots;
    O.bots = total;   /* the accounts and sockets of the watchers too */
    accounts();
    O.bots = players;
    if (!O.quiet) fprintf(stderr, "chessbench: %d bots playing %d+%d %s, think %d..%d ms, %d watchers, port %d\n", O.bots, O.base, O.inc,
                          O.rated ? "rated" : "casual", O.think, O.think + O.spread, O.watchers, O.port);
    uint64_t dummy;
    proc_usage(O.pid, &rss0, &dummy, &dummy);
    L = cb_loop_new();
    int keep = O.bots;
    O.bots = total;
    next_connect = 0;
    cb_loop_timer(L, 0, connect_tick, NULL);
    if (!O.forever) cb_loop_timer(L, O.warmup * 1000 + total * 1000 / (O.connect_rate ? O.connect_rate : 1), measure_tick, NULL);
    cb_loop_run(L);
    O.bots = keep;
    if (O.forever) return 0;
    double secs = (double)(now_ms_d() - t_measure) / 1000.0;
    uint64_t rss1 = 0, u1 = 0, s1 = 0;
    bool have = proc_usage(O.pid, &rss1, &u1, &s1);
    qsort(rt_us.v, rt_us.n, sizeof *rt_us.v, cmp_u32);
    qsort(relay_us.v, relay_us.n, sizeof *relay_us.v, cmp_u32);
    printf("chessbench: %d bots, %d+%d, think %d..%d ms, %d watchers\n", O.bots, O.base, O.inc, O.think, O.think + O.spread, O.watchers);
    printf("sockets open %d; measured %.1f s: moves %ld (%.0f/s), games ended %ld (%.1f/s), refusals %ld\n", n_open, secs, moves_measured,
           moves_measured / secs, games_measured, games_measured / secs, errors);
    printf("move round trip (us): n %zu p50 %u p90 %u p99 %u p999 %u max %u\n", rt_us.n, pct(&rt_us, 0.5), pct(&rt_us, 0.9), pct(&rt_us, 0.99),
           pct(&rt_us, 0.999), rt_us.n ? rt_us.v[rt_us.n - 1] : 0);
    printf("move relay      (us): n %zu p50 %u p90 %u p99 %u p999 %u max %u\n", relay_us.n, pct(&relay_us, 0.5), pct(&relay_us, 0.9),
           pct(&relay_us, 0.99), pct(&relay_us, 0.999), relay_us.n ? relay_us.v[relay_us.n - 1] : 0);
    printf("frames out %ld in %ld, bytes out %ld in %ld\n", frames_out, frames_in, bytes_out, bytes_in);
    if (O.chat)
        printf("chat: lines said %ld (%.0f/s), refused %ld, heard %ld (%.0f/s)\n", chat_said, chat_said / secs, chat_refused, chat_heard,
               chat_heard / secs);
    if (TLS_CTX) printf("tls: %ld full and %ld resumed handshakes by the bots\n", tls_full, tls_resumed);
    if (have) {
        double cpu = (double)((u1 - u0) + (s1 - s0));
        printf("server pid %d: rss %.1f MB before the bots, %.1f MB with them connected, %.1f MB at the end (%.2f KB per socket); "
               "cpu user %.2f s sys %.2f s = %.1f%% of one core, %.1f us per move\n", (int)O.pid, rss0 / 1048576.0, rss_open / 1048576.0,
               rss1 / 1048576.0, n_open ? (double)(rss1 - rss0) / 1024.0 / n_open : 0, (u1 - u0) / 1e6, (s1 - s0) / 1e6, cpu / 1e4 / secs,
               moves_measured ? cpu / (double)moves_measured : 0);
    }
    (void)moves_at_start;
    return 0;
}
