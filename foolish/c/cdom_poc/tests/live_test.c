/* live_test.c - cdom's network imports against a real local c_base (tests/live_test.mjs starts it).
 *
 * The host is the web's (src/common/backend.ts), written over cdom's imports: the kernel's client
 * (netclient.h) says what to send and reads every answer; this file only moves bytes. A request
 * goes out as fetch with the Authorization value the client wrote and, on a POST, Content-Type
 * application/octet-stream; its status and body go back to nc_on_response (0: it never arrived).
 * The socket is a plain binary WebSocket to the client's stream path; every frame nc_stream_next
 * names is sent, every message goes to nc_stream_in, and the pump runs again on a timer when
 * nc_due_ms says.
 *
 * The run: sign up a fresh name, list the user's games, open the stream (the HELLO first, the
 * READY back), create a game; then the failure paths the imports exist for: a wrong Bearer
 * (401 reaches C), a 1 ms deadline (TIMEOUT, not FAILED), a port nobody listens on (FAILED),
 * sockets opened and closed 20 times (one id), bufferedAmount 0 once the stream drained.
 * Each line of the verdict goes into the page's log: "ok ..." or "FAIL ...", then "done". */
#include "cdom.h"
#include "cdom_net.h"
#include "netclient.h"
#include "../../../../shared/c/stream_frame.h"

#define EXPORT(n) __attribute__((export_name(n)))

static char base[40];      /* http://127.0.0.1:<port> */
static uint32_t base_len;
static char ws_base[40];   /* ws://127.0.0.1:<port> */
static uint32_t ws_base_len;
static uint32_t dead_port;
static char name[24], password[32];
static int name_len, password_len;

/* ---- the log -------------------------------------------------------------------------- */
static cd_id log_box;
static int failures;
static char line[512];
static uint32_t line_len;
static void put_s(const char *s) { while (*s && line_len < sizeof line - 1) line[line_len++] = *s++; }
static void put_n(const char *s, uint32_t n) { for (uint32_t i = 0; i < n && line_len < sizeof line - 1; i++) line[line_len++] = s[i]; }
static void put_i(int64_t v) {
  char b[24];
  uint32_t k = cd_fmt(b, (int32_t)v, 0, CD_UNIT_NONE);
  put_n(b, k);
}
/* Ends the line being written as one row of the log. */
static void emit(void) {
  cd_at(log_box, 0);
  cd_open(CD_LIT("div"));
  cd_text(cd_tmp(line, line_len));
  cd_close();
  line_len = 0;
}
static void check(int ok, const char *what) {
  if (!ok) failures++;
  put_s(ok ? "ok   " : "FAIL ");
  put_s(what);
}

/* ---- the clock: netclient takes epoch ms ---------------------------------------------- */
static int64_t now_ms(void) { return (int64_t)cd_epoch_ms(); }

/* ---- requests: the client's, and the raw ones the failure checks send --------------------- */
#define RAW 0x80000000u
enum { RAW_WRONG_BEARER = 1, RAW_DEADLINE, RAW_DEAD_PORT };
/* What each of the client's requests was, for the log (the client reuses no id in a run this short). */
#define SENT_MAX 32
static struct { uint32_t id; uint8_t method; uint8_t path_len; char path[NC_PATH_MAX]; } sent[SENT_MAX];
static uint32_t n_sent;

static char headers[NC_AUTH_MAX + 128];
static char url[NC_PATH_MAX + 64];
static const char CONTENT_TYPE[] = "Content-Type: application/octet-stream\n";

static uint32_t cat(char *dst, uint32_t at, const char *src, uint32_t n) {
  __builtin_memcpy(dst + at, src, n);
  return at + n;
}
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

/* One of the client's requests out as fetch, as backend.ts send() does. A deadline of 15 s: a
 * request that hangs is answered TIMEOUT, which the client is told as "never arrived" (0). */
static void send_request(const NcRequest *r) {
  uint32_t h = 0;
  if (r->auth_len) {
    h = cat(headers, h, "Authorization: ", 15);
    h = cat(headers, h, r->auth, r->auth_len);
    h = cat(headers, h, "\n", 1);
  }
  if (r->method == NC_POST) h = cat(headers, h, CONTENT_TYPE, sizeof CONTENT_TYPE - 1);
  uint32_t u = cat(url, 0, base, base_len);
  u = cat(url, u, r->path, r->path_len);
  if (n_sent < SENT_MAX) {
    sent[n_sent].id = r->id;
    sent[n_sent].method = r->method;
    sent[n_sent].path_len = r->path_len;
    __builtin_memcpy(sent[n_sent].path, r->path, r->path_len);
    n_sent++;
  }
  int post = r->method == NC_POST;
  cd_http(r->id, post ? "POST" : "GET", post ? 4 : 3, url, u, headers, h, r->body, post ? r->body_len : 0, 15000);
}

/* ---- the stream --------------------------------------------------------------------------- */
static uint32_t stream_id = ~0u;
static int stream_open;
static int sent_first = -1;        /* the kind of the first frame sent on the stream */
static uint32_t hello_buffered;    /* bufferedAmount right after the HELLO went */
static uint8_t kinds_seen[16];
static uint32_t n_kinds;
static int ready_seen;

static void stream_send(void) {
  if (!stream_open) return;
  int64_t now = now_ms();
  for (const NcOut *o = nc_stream_next(now); o; o = nc_stream_next(now)) {
    if (o->len == 0) { cd_ws_close(stream_id); stream_open = 0; return; }
    cd_ws_send(stream_id, o->bytes, o->len);
    if (sent_first < 0) {
      sent_first = o->len >= 2 && o->bytes[0] == SF_V ? o->bytes[1] : 0;
      hello_buffered = cd_ws_buffered(stream_id);
    }
  }
}

/* ---- the pump ----------------------------------------------------------------------------- */
static uint32_t pump_gen;
static void settle(const NcDone *d);
static void pump(void) {
  int64_t now = now_ms();
  for (const NcRequest *r = nc_next_request(now); r; r = nc_next_request(now)) send_request(r);
  for (const NcDone *d = nc_next_done(); d; d = nc_next_done()) settle(d);
  stream_send();
  int ms = nc_due_ms(now);
  if (ms >= 0) cd_timer_start((uint32_t)ms, ++pump_gen);   /* an older timer finds another generation */
}
void cd_on_timer(uint32_t token) { if (token == pump_gen) pump(); }

static uint32_t submit(uint8_t kind) {
  static NcIntent in;
  __builtin_memset(&in, 0, sizeof in);
  in.kind = kind;
  if (kind == NC_DO_SIGN_UP) {
    in.name_len = (uint8_t)name_len;
    __builtin_memcpy(in.name, name, (uint32_t)name_len);
    in.password_len = (uint8_t)password_len;
    __builtin_memcpy(in.password, password, (uint32_t)password_len);
  }
  in.intent = -1;
  uint32_t t = nc_do(&in, now_ms());
  if (!t) { check(0, "the client refused an intent at once: why "); put_i(nc_error()->why); emit(); }
  return t;
}

/* ---- the steps ---------------------------------------------------------------------------- */
static uint32_t t_signup, t_games, t_create;
static void failure_paths(void);
static void cycle_open(uint32_t id);

static void open_stream(void) {
  const NcRequest *s = nc_req_stream();
  uint32_t u = cat(url, 0, ws_base, ws_base_len);
  u = cat(url, u, s->path, s->path_len);
  nc_stream_sub(NW_SUB_GAMES, 0, 0);
  stream_id = cd_ws_open(url, u);
}

static void settle(const NcDone *d) {
  if (d->ticket == t_signup) {
    const NcSessionInfo *si = nc_session_info();
    const NcRequest *g = nc_req_games(now_ms());   /* built to read its Authorization, never sent */
    int has_token = g && g->auth_len > 7 && __builtin_memcmp(g->auth, "Bearer ", 7) == 0;
    check(d->why == 0 && si->signed_in && has_token, "signup: signed in as ");
    put_n(si->username, si->username_len);
    put_s(", access token ");
    put_i(g ? g->auth_len - 7 : 0);
    put_s(" bytes, why ");
    put_i(d->why);
    emit();
    t_games = submit(NC_DO_GAMES);
  } else if (d->ticket == t_games) {
    check(d->why == 0 && d->list_kind == NW_GAMES, "games: a list of ");
    put_i(d->n_rows);
    put_s(" rows, why ");
    put_i(d->why);
    emit();
    open_stream();
  } else if (d->ticket == t_create) {
    check(d->why == 0 && d->a_len > 0 && d->gid_len > 0, "create: game ");
    put_n(d->gid, d->gid_len);
    put_s(", envelope ");
    put_i(d->a_len);
    put_s(" bytes, why ");
    put_i(d->why);
    emit();
    failure_paths();
  }
}

void cd_on_ws_open(uint32_t id) {
  if (id != stream_id) { cycle_open(id); return; }
  stream_open = 1;
  nc_stream_opened(now_ms());
  pump();
}

void cd_on_ws_msg(uint32_t id, const uint8_t *p, uint32_t len) {
  if (id != stream_id) return;
  uint8_t kind = len >= 2 && p[0] == SF_V ? p[1] : 0;
  if (n_kinds < sizeof kinds_seen) kinds_seen[n_kinds++] = kind;
  int64_t now = now_ms();
  nc_stream_in(p, (int)len, 0, now);
  if (kind == SF_READY && !ready_seen) {
    ready_seen = 1;
    check(sent_first == SF_HELLO, "stream: the first frame sent is HELLO (");
    put_i(sent_first);
    put_s("), READY (61) back");
    emit();
    t_create = submit(NC_DO_CREATE);
  }
  pump();
}

/* ---- the failure paths ---------------------------------------------------------------------- */
static uint32_t raw_left;
static uint32_t cycles_left, cycles_done, cycle_lo = ~0u, cycle_hi;
static void cycle_next(void);
static void finish(void);

static void raw(uint32_t which, const char *u0, const char *auth, uint32_t deadline_ms) {
  uint32_t h = cat(headers, 0, "Authorization: ", 15);
  h = cat(headers, h, auth, slen(auth));
  uint32_t u = cat(url, 0, u0, slen(u0));
  u = cat(url, u, "/v1/games", 9);
  cd_http(RAW | which, "GET", 3, url, u, headers, h, 0, 0, deadline_ms);
}

static char real_auth[NC_AUTH_MAX + 1];
static char dead_base[40];
static void failure_paths(void) {
  const NcRequest *g = nc_req_games(now_ms());
  __builtin_memcpy(real_auth, g->auth, g->auth_len);
  real_auth[g->auth_len] = 0;
  base[base_len] = 0;
  raw_left = 3;
  raw(RAW_WRONG_BEARER, base, "Bearer not-a-token", 15000);
  raw(RAW_DEADLINE, base, real_auth, 1);
  uint32_t d = cat(dead_base, 0, "http://127.0.0.1:", 17);
  d += cd_fmt(dead_base + d, (int32_t)dead_port, 0, CD_UNIT_NONE);
  dead_base[d] = 0;
  raw(RAW_DEAD_PORT, dead_base, real_auth, 15000);
}

static void raw_answer(uint32_t which, uint32_t status) {
  if (which == RAW_WRONG_BEARER) {
    check(status == 401, "a wrong Bearer: status ");
  } else if (which == RAW_DEADLINE) {
    check(status == CD_HTTP_TIMEOUT, "a 1 ms deadline: aborted, status ");
  } else {
    check(status == CD_HTTP_FAILED, "a port nobody listens on: never arrived, status ");
  }
  put_i(status);
  emit();
  if (--raw_left == 0) {
    cycles_left = 20;
    cycle_next();
  }
}

static void cycle_next(void) {
  const NcRequest *s = nc_req_stream();
  uint32_t u = cat(url, 0, ws_base, ws_base_len);
  u = cat(url, u, s->path, s->path_len);
  cd_ws_open(url, u);
}
static void cycle_open(uint32_t id) {
  if (id < cycle_lo) cycle_lo = id;
  if (id > cycle_hi) cycle_hi = id;
  cd_ws_close(id);
}

void cd_on_ws_close(uint32_t id, uint32_t code) {
  if (id == stream_id && stream_open) {
    stream_open = 0;
    check(0, "the stream closed: code ");
    put_i(code);
    emit();
    return;
  }
  if (!cycles_left) return;
  cycles_done++;
  if (--cycles_left) cycle_next(); else finish();
}

static void finish(void) {
  check(cycles_done == 20 && cycle_lo == cycle_hi, "sockets: 20 opened and closed, ids ");
  put_i(cycle_lo);
  put_s(" to ");
  put_i(cycle_hi);
  emit();
  uint32_t buffered = cd_ws_buffered(stream_id);
  check(stream_open && hello_buffered > 0 && buffered == 0, "bufferedAmount: ");
  put_i(hello_buffered);
  put_s(" right after the HELLO, ");
  put_i(buffered);
  put_s(" once drained");
  emit();
  put_s("stream frames seen:");
  for (uint32_t i = 0; i < n_kinds; i++) { put_s(" "); put_i(kinds_seen[i]); }
  emit();
  put_s("requests:");
  for (uint32_t i = 0; i < n_sent; i++) {
    put_s(sent[i].method == NC_POST ? " POST " : " GET ");
    put_n(sent[i].path, sent[i].path_len);
  }
  emit();
  put_s(failures ? "done, failures " : "done");
  if (failures) put_i(failures);
  emit();
}

void cd_on_http(uint32_t token, uint32_t status, const uint8_t *p, uint32_t len) {
  if (token & RAW) { raw_answer(token & ~RAW, status); return; }
  for (uint32_t i = 0; i < n_sent; i++) {
    if (sent[i].id != token) continue;
    put_s("http ");
    put_s(sent[i].method == NC_POST ? "POST " : "GET ");
    put_n(sent[i].path, sent[i].path_len);
    put_s(" -> ");
    put_i(status);
    emit();
    break;
  }
  int64_t now = now_ms();
  const NcDone *d = nc_on_response(token, status == CD_HTTP_TIMEOUT ? 0 : (int)status, p, (int)len, now);
  if (d) settle(d);
  pump();
}

void cd_on_event(const cd_event_rec *e) { (void)e; }

void cd_main(void) {
  cd_at(CD_MOUNT, 0);
  cd_open(CD_LIT("pre"));
  cd_attr(CD_LIT("id"), CD_LIT("log"));
  cd_name_open(log_box = cd_id_new());
  cd_close();
}

/* The page says where the server is, a port nobody listens on, and a nonce for a fresh name. */
EXPORT("live_start") void live_start(uint32_t port, uint32_t dead, uint32_t nonce) {
  base_len = cat(base, 0, "http://127.0.0.1:", 17);
  base_len += cd_fmt(base + base_len, (int32_t)port, 0, CD_UNIT_NONE);
  ws_base_len = cat(ws_base, 0, "ws://127.0.0.1:", 15);
  ws_base_len += cd_fmt(ws_base + ws_base_len, (int32_t)port, 0, CD_UNIT_NONE);
  dead_port = dead;
  static const char HEX[] = "0123456789ABCDEF";
  name_len = (int)cat(name, 0, "CDOMLIVE", 8);
  for (int i = 0; i < 8; i++) name[name_len++] = HEX[(nonce >> (28 - 4 * i)) & 15];
  password_len = (int)cat(password, 0, "cdom-live-", 10);
  for (int i = 0; i < 8; i++) password[password_len++] = HEX[(nonce >> (4 * i)) & 15];
  nc_reset();
  t_signup = submit(NC_DO_SIGN_UP);
  pump();
}
