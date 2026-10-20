/* load.c : the spike's load client (kernel TCP, one epoll thread).
 *
 *   xload --host IP6 [--port 7000] [--ctl 7100] --mode hum|sat|smoke|idle --conns N --k K --size BYTES
 *         [--warm S] [--dur S] [--moves N] [--label L] [--out FILE]
 *
 * N connections in groups of K. A MOVE sent on one connection must come back as one FRAME on each of the K connections
 * of its group; latency is from the move's send() to the last of the K frames read.
 *   hum   each connection sends a move every U(2,4) s (300 connections give about 100 moves/s)
 *   sat   closed loop per group: the next move (from the next slot) goes out as soon as the last one completed
 *   smoke sat until --moves moves were sent, then drain (the correctness run)
 *   idle  connections open, no moves (the box's floor with N idle connections)
 * Every frame is checked: length, group, mover, the move's send time, the per-connection dseq (no gap, no dup) and every
 * body byte. Server counters are read on the control port at the start and the end of the measurement window.
 */
#define _GNU_SOURCE
#include "common.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static const char *arg(int argc, char **argv, const char *k, const char *def) {
  for (int i = 1; i + 1 < argc; i++)
    if (!strcmp(argv[i], k)) return argv[i + 1];
  return def;
}

struct conn { int fd, group, slot; uint32_t exp_dseq, inlen; uint64_t next_t; uint8_t in[65536]; };
struct mv { uint64_t t_send; uint32_t group; uint16_t recv; uint8_t mover, in_win; };
struct grp { int next_slot, inflight; };

static struct conn *C;
static struct grp *G;
static struct mv *M;
static uint32_t nmoves, maxmoves;
static int N, K, NG, SIZE;
static uint64_t *lat;
static uint32_t nlat;
static uint64_t gaps, dups, badbytes, badmeta, frames, completed, sent_win, done_win, send_fail;
static int in_window, sending = 1;
static struct sockaddr_in6 srv;

static int connect_to(int port) {
  int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  struct sockaddr_in6 a = srv;
  a.sin6_port = htons((uint16_t)port);
  if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  return fd;
}

static int send_all(int fd, const void *p, size_t n) {
  size_t t = 0;
  int spins = 0;
  while (t < n) {
    ssize_t w = send(fd, (const uint8_t *)p + t, n - t, MSG_NOSIGNAL);
    if (w > 0) { t += (size_t)w; continue; }
    if (w < 0 && (errno == EAGAIN || errno == EINTR) && ++spins < 1000000) continue;
    return -1;
  }
  return 0;
}

static int ctl_port;
static char *ctl(const char *cmd) {
  static char buf[16384];
  buf[0] = 0;
  for (int attempt = 0; attempt < 5; attempt++) {
    int fd = connect_to(ctl_port);
    if (fd < 0) { usleep(200000); continue; }
    send_all(fd, cmd, strlen(cmd));
    size_t t = 0;
    ssize_t r;
    while (t + 1 < sizeof buf && (r = recv(fd, buf + t, sizeof buf - 1 - t, 0)) > 0) {
      t += (size_t)r;
      if (memchr(buf, '\n', t)) break;
    }
    buf[t] = 0;
    close(fd);
    if (t) return buf;
  }
  return buf;
}

static void send_move(int ci) {
  if (nmoves >= maxmoves) { send_fail++; return; }
  struct conn *c = &C[ci];
  uint32_t id = nmoves++;
  struct move m = {{sizeof m, MSG_MOVE}, id, 0, now_ns()};
  M[id] = (struct mv){m.t_send, (uint32_t)c->group, 0, (uint8_t)c->slot, (uint8_t)in_window};
  if (in_window) sent_win++;
  if (send_all(c->fd, &m, sizeof m)) send_fail++;
}

static int cmp64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}

static void on_frame(int ci, const uint8_t *p, uint32_t len, uint64_t t, int sat) {
  struct conn *c = &C[ci];
  struct frame f;
  memcpy(&f, p, sizeof f);
  frames++;
  if (f.h.type != MSG_FRAME || len != (uint32_t)SIZE || f.group != (uint32_t)c->group) { badmeta++; return; }
  if (f.dseq != c->exp_dseq) {
    if (f.dseq < c->exp_dseq) dups++; else gaps += f.dseq - c->exp_dseq;
  }
  c->exp_dseq = f.dseq + 1;
  for (uint32_t i = FRAME_MIN; i < len; i++)
    if (p[i] != body_byte(f.mover, f.mseq, i)) { badbytes++; break; }
  if (f.mseq >= nmoves) { badmeta++; return; }
  struct mv *m = &M[f.mseq];
  if (m->t_send != f.t_send || m->group != f.group || m->mover != f.mover) { badmeta++; return; }
  m->recv++;
  if (m->recv == K) {
    completed++;
    if (m->in_win) { done_win++; lat[nlat++] = t - m->t_send; }
    if (sat) {
      struct grp *g = &G[m->group];
      g->inflight = 0;
      if (sending) {
        g->next_slot = (g->next_slot + 1) % K;
        g->inflight = 1;
        send_move((int)m->group * K + g->next_slot);
      }
    }
  } else if (m->recv > K) dups++;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *host = arg(argc, argv, "--host", NULL);
  const char *mode = arg(argc, argv, "--mode", "hum");
  const char *label = arg(argc, argv, "--label", "run");
  const char *out = arg(argc, argv, "--out", "results.txt");
  int port = atoi(arg(argc, argv, "--port", "7000"));
  ctl_port = atoi(arg(argc, argv, "--ctl", "7100"));
  K = atoi(arg(argc, argv, "--k", "2"));
  N = atoi(arg(argc, argv, "--conns", "300")) / K * K;
  SIZE = atoi(arg(argc, argv, "--size", "300"));
  double warm = atof(arg(argc, argv, "--warm", "10")), dur = atof(arg(argc, argv, "--dur", "45"));
  uint32_t target = (uint32_t)atol(arg(argc, argv, "--moves", "10000"));
  int hum = !strcmp(mode, "hum"), sat = !strcmp(mode, "sat") || !strcmp(mode, "smoke"), smoke = !strcmp(mode, "smoke"),
      idle = !strcmp(mode, "idle");
  if (!host || K < 1 || K > 64 || N < K || SIZE < (int)FRAME_MIN || SIZE > (int)FRAME_MAX) { fprintf(stderr, "bad args\n"); return 2; }
  memset(&srv, 0, sizeof srv);
  srv.sin6_family = AF_INET6;
  if (inet_pton(AF_INET6, host, &srv.sin6_addr) != 1) { fprintf(stderr, "bad host\n"); return 2; }
  NG = N / K;
  maxmoves = 8u << 20;
  C = calloc((size_t)N, sizeof *C);
  G = calloc((size_t)NG, sizeof *G);
  M = calloc(maxmoves, sizeof *M);
  lat = malloc(sizeof *lat * maxmoves);
  srandom((unsigned)getpid());

  int ep = epoll_create1(EPOLL_CLOEXEC);
  for (int i = 0; i < N; i++) {
    int fd = -1;
    for (int a = 0; a < 50 && fd < 0; a++) { fd = connect_to(port); if (fd < 0) usleep(100000); }
    if (fd < 0) { fprintf(stderr, "connect %d failed: %s\n", i, strerror(errno)); return 1; }
    C[i] = (struct conn){.fd = fd, .group = i / K, .slot = i % K};
    struct hello h = {{sizeof h, MSG_HELLO}, (uint32_t)(i / K), (uint32_t)(i % K), (uint32_t)K, (uint32_t)SIZE};
    send_all(fd, &h, sizeof h);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct epoll_event e = {.events = EPOLLIN, .data.u32 = (uint32_t)i};
    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e);
  }
  usleep(1000000); /* the hellos land and the server's group table fills */

  char snap0[16384] = "", snap1[16384] = "";
  uint64_t t0 = now_ns();
  uint64_t tw0 = t0 + (uint64_t)(warm * 1e9), tw1 = tw0 + (uint64_t)(dur * 1e9);
  if (smoke) { strcpy(snap0, ctl("S\n")); in_window = 1; tw0 = t0; tw1 = UINT64_MAX; }
  if (hum)
    for (int i = 0; i < N; i++) C[i].next_t = t0 + (uint64_t)(random() % 3000) * 1000000ull;
  if (sat)
    for (int g = 0; g < NG; g++) { G[g].inflight = 1; send_move(g * K); }

  struct rusage ru0;
  getrusage(RUSAGE_SELF, &ru0);
  struct epoll_event ev[512];
  uint64_t drain_end = 0;
  for (;;) {
    uint64_t t = now_ns();
    if (!smoke && !in_window && sending && t >= tw0) { strcpy(snap0, ctl("S\n")); in_window = 1; getrusage(RUSAGE_SELF, &ru0); }
    if (!smoke && in_window && sending && t >= tw1) { strcpy(snap1, ctl("S\n")); in_window = 0; sending = 0; }
    if (smoke && sending && nmoves >= target) { sending = 0; }
    if (!sending) {
      if (!drain_end) drain_end = t + 10000000000ull;
      if (completed == nmoves || t >= drain_end) break;
    }
    if (hum && sending)
      for (int i = 0; i < N; i++)
        if (C[i].next_t <= t) {
          send_move(i);
          C[i].next_t = t + (2000 + (uint64_t)(random() % 2001)) * 1000000ull;
        }
    (void)idle;
    int n = epoll_wait(ep, ev, 512, 1);
    for (int i = 0; i < n; i++) {
      int ci = (int)ev[i].data.u32;
      struct conn *c = &C[ci];
      ssize_t r = recv(c->fd, c->in + c->inlen, sizeof c->in - c->inlen, 0);
      if (r <= 0) {
        if (r < 0 && errno == EAGAIN) continue;
        fprintf(stderr, "conn %d closed by server (%zd %s)\n", ci, r, strerror(errno));
        epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
        continue;
      }
      uint64_t tr = now_ns();
      c->inlen += (uint32_t)r;
      uint32_t off = 0;
      while (c->inlen - off >= sizeof(struct hdr)) {
        struct hdr h;
        memcpy(&h, c->in + off, sizeof h);
        if (h.len < FRAME_MIN || h.len > FRAME_MAX) { badmeta++; off = c->inlen; break; }
        if (c->inlen - off < h.len) break;
        on_frame(ci, c->in + off, h.len, tr, sat);
        off += h.len;
      }
      memmove(c->in, c->in + off, c->inlen - off);
      c->inlen -= off;
    }
  }
  if (smoke) strcpy(snap1, ctl("S\n"));
  struct rusage ru1;
  getrusage(RUSAGE_SELF, &ru1);
  uint64_t lost = 0;
  for (uint32_t i = 0; i < nmoves; i++) lost += M[i].recv < K;
  qsort(lat, nlat, sizeof *lat, cmp64);
  double mean = 0;
  for (uint32_t i = 0; i < nlat; i++) mean += (double)lat[i];
  mean = nlat ? mean / nlat / 1000.0 : 0;
#define PCT(p) (nlat ? (double)lat[(size_t)((double)(nlat - 1) * (p))] / 1000.0 : 0.0)
  long ccpu = (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) * 1000000L + (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) +
              (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) * 1000000L + (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec);
  char line[1024];
  snprintf(line, sizeof line,
           "RUN label=%s mode=%s k=%d size=%d conns=%d warm=%.1f dur=%.1f moves_sent=%u completed=%llu sent_win=%llu done_win=%llu "
           "lost=%llu dups=%llu gaps=%llu badbytes=%llu badmeta=%llu send_fail=%llu frames=%llu p50_us=%.1f p95_us=%.1f p99_us=%.1f "
           "p999_us=%.1f max_us=%.1f mean_us=%.1f client_cpu_us=%ld\n",
           label, mode, K, SIZE, N, warm, dur, nmoves, (unsigned long long)completed, (unsigned long long)sent_win,
           (unsigned long long)done_win, (unsigned long long)lost, (unsigned long long)dups, (unsigned long long)gaps,
           (unsigned long long)badbytes, (unsigned long long)badmeta, (unsigned long long)send_fail, (unsigned long long)frames, PCT(0.5),
           PCT(0.95), PCT(0.99), PCT(0.999), nlat ? (double)lat[nlat - 1] / 1000.0 : 0.0, mean, ccpu);
  fputs(line, stdout);
  FILE *f = fopen(out, "a");
  if (f) {
    fputs(line, f);
    fprintf(f, "SNAP0 label=%s %s", label, snap0[0] ? snap0 : "none\n");
    fprintf(f, "SNAP1 label=%s %s", label, snap1[0] ? snap1 : "none\n");
    fclose(f);
  }
  for (int i = 0; i < N; i++) close(C[i].fd);
  return (lost || dups || gaps || badbytes || badmeta) ? 3 : 0;
}
