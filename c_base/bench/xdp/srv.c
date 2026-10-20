/* srv.c : the spike server's app, meter, control socket, kernel-TCP backend and main.
 *
 *   xsrv kern   --port P --ctl C --spin US [--tp-sys 1]
 *   xsrv xdp    --if IF --ip6 ADDR [--gw-mac MAC] --port P --ctl C --spin US [--zc auto|zc|copy]
 *               [--drop-rx PERMILLE] [--drop-tx PERMILLE] [--tp-sys 1]
 *   xsrv attach --if IF --mode drv|skb --obj xdp_prog.o     (load, pin the maps under /sys/fs/bpf/xdpspike, attach)
 *   xsrv detach --if IF
 *
 * The app: each MOVE received on a connection spins `--spin` microseconds of calibrated work (a loop that reads and
 * writes a 64 KiB buffer, so the handler leaves a cache footprint), then sends one FRAME to each of the K members of
 * the mover's group. A snapshot of every counter is served on the control port: send "S\n", read one line.
 */
#define _GNU_SOURCE
#include "be.h"
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

volatile int g_quit;
struct sc_count g_sc;
struct be_stats g_bs;
static const struct backend *g_be;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---------- argument helpers ---------- */
const char *arg_str(int argc, char **argv, const char *k, const char *def) {
  for (int i = 2; i + 1 < argc; i++)
    if (!strcmp(argv[i], k)) return argv[i + 1];
  return def;
}
long arg_long(int argc, char **argv, const char *k, long def) {
  const char *s = arg_str(argc, argv, k, NULL);
  return s ? strtol(s, NULL, 0) : def;
}

/* ---------- the handler's calibrated work ---------- */
static uint8_t spinbuf[1 << 16];
static volatile uint64_t g_sink;
static uint64_t g_spin_iters;
static double g_ns_per_iter;

static void work(uint64_t n) {
  uint64_t h = g_sink;
  for (uint64_t i = 0; i < n; i++) {
    size_t o = (size_t)(i * 64 + (h & 63)) & (sizeof spinbuf - 1);
    h = h * 0x9E3779B97F4A7C15ull + spinbuf[o];
    spinbuf[(o + 32) & (sizeof spinbuf - 1)] = (uint8_t)h;
  }
  g_sink = h;
}

static void calibrate(long spin_us) {
  double best = 1e30;
  for (int r = 0; r < 9; r++) {
    uint64_t t0 = now_ns();
    work(200000);
    double d = (double)(now_ns() - t0) / 200000.0;
    if (d < best) best = d;
  }
  g_ns_per_iter = best;
  g_spin_iters = spin_us > 0 ? (uint64_t)((double)spin_us * 1000.0 / best) : 0;
}

/* ---------- the app ---------- */
struct conn {
  int used, group, slot, k;
  uint32_t size, dseq, inlen;
  uint8_t in[8192];
};
static struct conn *conns;
static int (*group_members)[64]; /* [group][slot] -> cid or -1 */
static uint64_t c_moves, c_frames, c_fbytes, c_rxmsgs, c_missing, c_badmsg, c_opens, c_closes;
static uint8_t fbuf[FRAME_MAX];

void app_on_open(int cid) {
  struct conn *c = &conns[cid];
  memset(c, 0, offsetof(struct conn, in));
  c->used = 1;
  c->group = -1;
  c_opens++;
}

void app_on_close(int cid) {
  struct conn *c = &conns[cid];
  if (c->used && c->group >= 0 && c->slot < 64 && group_members[c->group][c->slot] == cid)
    group_members[c->group][c->slot] = -1;
  c->used = 0;
  c_closes++;
}

static void handle_move(int cid, const struct move *m) {
  struct conn *c = &conns[cid];
  if (c->group < 0) { c_badmsg++; return; }
  if (g_spin_iters) work(g_spin_iters);
  uint32_t size = c->size;
  struct frame *f = (struct frame *)fbuf;
  f->h.len = size;
  f->h.type = MSG_FRAME;
  f->group = (uint32_t)c->group;
  f->mover = (uint32_t)c->slot;
  f->mseq = m->mseq;
  f->t_send = m->t_send;
  for (uint32_t i = FRAME_MIN; i < size; i++) fbuf[i] = body_byte(f->mover, f->mseq, i);
  int *mem = group_members[c->group];
  for (int j = 0; j < c->k; j++) {
    int d = mem[j];
    if (d < 0) { c_missing++; continue; }
    f->dseq = conns[d].dseq++;
    g_be->send(d, fbuf, size);
    c_frames++;
    c_fbytes += size;
  }
  c_moves++;
}

void app_on_data(int cid, const uint8_t *p, size_t n) {
  struct conn *c = &conns[cid];
  while (n > 0) {
    size_t take = sizeof c->in - c->inlen;
    if (take > n) take = n;
    memcpy(c->in + c->inlen, p, take);
    c->inlen += (uint32_t)take;
    p += take;
    n -= take;
    uint32_t off = 0;
    while (c->inlen - off >= sizeof(struct hdr)) {
      struct hdr h;
      memcpy(&h, c->in + off, sizeof h);
      if (h.len < sizeof h || h.len > 1024) { c_badmsg++; c->inlen = 0; return; }
      if (c->inlen - off < h.len) break;
      c_rxmsgs++;
      if (h.type == MSG_HELLO && h.len >= sizeof(struct hello)) {
        struct hello hl;
        memcpy(&hl, c->in + off, sizeof hl);
        if (hl.group < MAX_CONNS && hl.slot < 64 && hl.k >= 1 && hl.k <= 64 && hl.size >= FRAME_MIN && hl.size <= FRAME_MAX) {
          c->group = (int)hl.group;
          c->slot = (int)hl.slot;
          c->k = (int)hl.k;
          c->size = hl.size;
          group_members[hl.group][hl.slot] = cid;
        } else c_badmsg++;
      } else if (h.type == MSG_MOVE && h.len >= sizeof(struct move)) {
        struct move m;
        memcpy(&m, c->in + off, sizeof m);
        handle_move(cid, &m);
      } else c_badmsg++;
      off += h.len;
    }
    if (off) {
      memmove(c->in, c->in + off, c->inlen - off);
      c->inlen -= off;
    }
  }
}

/* ---------- the meter ---------- */
static int ncpu;
static int idle_fd[64], idle_filtered;
static int tp_sys_fd = -1;
static char g_ifname[32] = "";

static int read_file(const char *p, char *b, size_t cap) {
  int fd = open(p, O_RDONLY);
  if (fd < 0) return -1;
  size_t t = 0;
  ssize_t r;
  while (t + 1 < cap && (r = read(fd, b + t, cap - 1 - t)) > 0) t += (size_t)r;
  close(fd);
  b[t] = 0;
  return (int)t;
}

static long tp_id(const char *ev) {
  char p[256], b[64];
  const char *roots[] = {"/sys/kernel/tracing/events/", "/sys/kernel/debug/tracing/events/"};
  for (int i = 0; i < 2; i++) {
    snprintf(p, sizeof p, "%s%s/id", roots[i], ev);
    if (read_file(p, b, sizeof b) > 0) return atol(b);
  }
  return -1;
}

static int perf_open(long id, int pid, int cpu) {
  struct perf_event_attr a;
  memset(&a, 0, sizeof a);
  a.type = PERF_TYPE_TRACEPOINT;
  a.size = sizeof a;
  a.config = (uint64_t)id;
  a.disabled = 1;
  return (int)syscall(SYS_perf_event_open, &a, pid, cpu, -1, 0);
}

static void meter_init(int tp_sys) {
  ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
  if (ncpu > 64) ncpu = 64;
  long id = tp_id("power/cpu_idle");
  idle_filtered = 1;
  for (int c = 0; c < ncpu; c++) {
    idle_fd[c] = id >= 0 ? perf_open(id, -1, c) : -1;
    if (idle_fd[c] >= 0) {
      /* count the exits only (state is (u32)-1 on exit) */
      if (ioctl(idle_fd[c], PERF_EVENT_IOC_SET_FILTER, "state == 4294967295") != 0) idle_filtered = 0;
      ioctl(idle_fd[c], PERF_EVENT_IOC_ENABLE, 0);
    }
  }
  if (tp_sys) {
    long sid = tp_id("raw_syscalls/sys_enter");
    tp_sys_fd = sid >= 0 ? perf_open(sid, 0, -1) : -1;
    if (tp_sys_fd >= 0) ioctl(tp_sys_fd, PERF_EVENT_IOC_ENABLE, 0);
  }
}

struct sbuf { char *b; size_t n, cap; };
static void sb(struct sbuf *s, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(s->b + s->n, s->cap - s->n, fmt, ap);
  va_end(ap);
  if (r > 0 && (size_t)r < s->cap - s->n) s->n += (size_t)r;
}

static void snapshot(char *out, size_t cap) {
  static char f[65536];
  struct sbuf s = {out, 0, cap};
  sb(&s, "t_ns=%llu", (unsigned long long)now_ns());
  if (read_file("/proc/self/schedstat", f, sizeof f) > 0) sb(&s, " sched_ns=%llu", strtoull(f, NULL, 10));
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  sb(&s, " ut_us=%lld st_us=%lld nvcsw=%ld nivcsw=%ld", (long long)ru.ru_utime.tv_sec * 1000000 + ru.ru_utime.tv_usec,
     (long long)ru.ru_stime.tv_sec * 1000000 + ru.ru_stime.tv_usec, ru.ru_nvcsw, ru.ru_nivcsw);
  sb(&s, " moves=%llu frames=%llu fbytes=%llu rxmsgs=%llu missing=%llu badmsg=%llu opens=%llu closes=%llu",
     (unsigned long long)c_moves, (unsigned long long)c_frames, (unsigned long long)c_fbytes, (unsigned long long)c_rxmsgs,
     (unsigned long long)c_missing, (unsigned long long)c_badmsg, (unsigned long long)c_opens, (unsigned long long)c_closes);
  sb(&s, " sc_wait=%llu sc_recv=%llu sc_send=%llu sc_accept=%llu sc_kick=%llu sc_other=%llu sc_ctl=%llu",
     (unsigned long long)g_sc.wait, (unsigned long long)g_sc.recv, (unsigned long long)g_sc.send, (unsigned long long)g_sc.accept,
     (unsigned long long)g_sc.kick, (unsigned long long)g_sc.other, (unsigned long long)g_sc.ctl);
  sb(&s, " passes=%llu rx_pkts=%llu tx_pkts=%llu tx_full=%llu rexmit=%llu rx_bytes=%llu", (unsigned long long)g_bs.passes,
     (unsigned long long)g_bs.rx_pkts, (unsigned long long)g_bs.tx_pkts, (unsigned long long)g_bs.tx_full,
     (unsigned long long)g_bs.rexmit, (unsigned long long)g_bs.rx_bytes);
  if (tp_sys_fd >= 0) {
    uint64_t v = 0;
    if (read(tp_sys_fd, &v, sizeof v) == sizeof v) sb(&s, " tp_sys=%llu", (unsigned long long)v);
  }
  /* /proc/stat: per-cpu jiffies, context switches, interrupts */
  if (read_file("/proc/stat", f, sizeof f) > 0) {
    for (char *l = strtok(f, "\n"); l; l = strtok(NULL, "\n")) {
      int c;
      unsigned long long v[10] = {0};
      if (l[0] == 'c' && l[1] == 'p' && l[2] == 'u' && l[3] >= '0' && l[3] <= '9' && /* not the aggregate "cpu  " line: %d skips spaces */
          sscanf(l, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &c, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 9)
        sb(&s, " c%d_user=%llu c%d_nice=%llu c%d_sys=%llu c%d_idle=%llu c%d_iowait=%llu c%d_irq=%llu c%d_softirq=%llu c%d_steal=%llu",
           c, v[0], c, v[1], c, v[2], c, v[3], c, v[4], c, v[5], c, v[6], c, v[7]);
      else if (!strncmp(l, "ctxt ", 5)) sb(&s, " ctxt=%s", l + 5);
      else if (!strncmp(l, "intr ", 5)) sb(&s, " intr=%llu", strtoull(l + 5, NULL, 10));
    }
  }
  /* /proc/softirqs: NET_RX NET_TX TIMER SCHED RCU per cpu */
  if (read_file("/proc/softirqs", f, sizeof f) > 0) {
    char *save = NULL;
    for (char *l = strtok_r(f, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
      while (*l == ' ') l++;
      const char *names[] = {"NET_RX:", "NET_TX:", "TIMER:", "SCHED:", "RCU:", "HRTIMER:"};
      for (int i = 0; i < 6; i++) {
        size_t nl = strlen(names[i]);
        if (strncmp(l, names[i], nl)) continue;
        char *q = l + nl;
        for (int c = 0; c < ncpu; c++) {
          unsigned long long v = strtoull(q, &q, 10);
          sb(&s, " si_%.*s_%d=%llu", (int)nl - 1, names[i], c, v);
        }
      }
    }
  }
  /* /proc/interrupts: the NIC's lines, summed per cpu (io queues and the rest separately) */
  if (read_file("/proc/interrupts", f, sizeof f) > 0) {
    unsigned long long io[64] = {0}, mg[64] = {0};
    char *save = NULL;
    for (char *l = strtok_r(f, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
      int isio = (g_ifname[0] && strstr(l, g_ifname)) || strstr(l, "-Tx-Rx-");
      int isena = strstr(l, "ena") != NULL;
      if (!isio && !isena) continue;
      char *q = strchr(l, ':');
      if (!q) continue;
      q++;
      for (int c = 0; c < ncpu; c++) {
        unsigned long long v = strtoull(q, &q, 10);
        if (isio) io[c] += v; else mg[c] += v;
      }
    }
    for (int c = 0; c < ncpu; c++) sb(&s, " nicirq_%d=%llu nicmgmt_%d=%llu", c, io[c], c, mg[c]);
  }
  for (int c = 0; c < ncpu; c++) {
    uint64_t v = 0;
    if (idle_fd[c] >= 0 && read(idle_fd[c], &v, sizeof v) == sizeof v)
      sb(&s, " idlex_%d=%llu", c, (unsigned long long)(idle_filtered ? v : v / 2));
  }
  if (g_be && g_be->snap_extra) {
    char extra[2048];
    extra[0] = 0;
    g_be->snap_extra(extra, sizeof extra);
    sb(&s, "%s", extra);
  }
  sb(&s, "\n");
}

/* ---------- the control socket (always kernel TCP) ---------- */
static int g_ctl = -1;
int ctl_fd(void) { return g_ctl; }

static int listen6(int port, int backlog) {
  int fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in6 a;
  memset(&a, 0, sizeof a);
  a.sin6_family = AF_INET6;
  a.sin6_port = htons((uint16_t)port);
  a.sin6_addr = in6addr_any;
  if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, backlog)) { close(fd); return -1; }
  return fd;
}

void ctl_ready(void) {
  int fd = accept4(g_ctl, NULL, NULL, SOCK_CLOEXEC);
  g_sc.ctl++;
  if (fd < 0) return;
  struct timeval tv = {1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  char cmd[64];
  ssize_t r = recv(fd, cmd, sizeof cmd - 1, 0);
  g_sc.ctl += 2;
  if (r > 0) {
    static char out[16384];
    if (cmd[0] == 'Q') { g_quit = 1; strcpy(out, "bye\n"); }
    else snapshot(out, sizeof out);
    size_t n = strlen(out), t = 0;
    while (t < n) {
      ssize_t w = send(fd, out + t, n - t, MSG_NOSIGNAL);
      g_sc.ctl++;
      if (w <= 0) break;
      t += (size_t)w;
    }
  }
  close(fd);
  g_sc.ctl++;
}

/* ---------- the kernel TCP backend: one epoll loop, one recv per readable socket, one send per destination ---------- */
static int k_ep = -1, k_lfd = -1;
static int cid_fd[MAX_CONNS];
struct outq { uint8_t *b; size_t n, cap; };
static struct outq k_oq[MAX_CONNS];
#define TAG_LISTEN (MAX_CONNS + 1)
#define TAG_CTL (MAX_CONNS + 2)

int be_kern_init(int port) {
  k_lfd = listen6(port, 4096);
  if (k_lfd < 0) { perror("listen"); return -1; }
  k_ep = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event e = {.events = EPOLLIN, .data.u64 = TAG_LISTEN};
  epoll_ctl(k_ep, EPOLL_CTL_ADD, k_lfd, &e);
  e.data.u64 = TAG_CTL;
  epoll_ctl(k_ep, EPOLL_CTL_ADD, g_ctl, &e);
  for (int i = 0; i < MAX_CONNS; i++) cid_fd[i] = -1;
  return 0;
}

static void k_close(int cid) {
  if (cid_fd[cid] < 0) return;
  epoll_ctl(k_ep, EPOLL_CTL_DEL, cid_fd[cid], NULL);
  close(cid_fd[cid]);
  g_sc.other += 2;
  cid_fd[cid] = -1;
  k_oq[cid].n = 0;
  app_on_close(cid);
}

static int k_send(int cid, const void *p, size_t n) {
  int fd = cid_fd[cid];
  if (fd < 0) return -1;
  struct outq *q = &k_oq[cid];
  size_t done = 0;
  if (q->n == 0) {
    ssize_t r = send(fd, p, n, MSG_NOSIGNAL | MSG_DONTWAIT);
    g_sc.send++;
    if (r < 0 && errno != EAGAIN) return -1;
    done = r > 0 ? (size_t)r : 0;
  }
  if (done < n) {
    size_t need = q->n + n - done;
    if (need > q->cap) {
      q->cap = need * 2;
      q->b = realloc(q->b, q->cap);
    }
    memcpy(q->b + q->n, (const uint8_t *)p + done, n - done);
    q->n = need;
    g_bs.sendq_bytes += n - done;
    struct epoll_event e = {.events = EPOLLIN | EPOLLOUT, .data.u64 = (uint64_t)cid};
    epoll_ctl(k_ep, EPOLL_CTL_MOD, fd, &e);
    g_sc.other++;
  }
  return 0;
}

static void k_flush(int cid) {
  struct outq *q = &k_oq[cid];
  int fd = cid_fd[cid];
  while (q->n) {
    ssize_t r = send(fd, q->b, q->n, MSG_NOSIGNAL | MSG_DONTWAIT);
    g_sc.send++;
    if (r <= 0) break;
    memmove(q->b, q->b + r, q->n - (size_t)r);
    q->n -= (size_t)r;
  }
  if (!q->n) {
    struct epoll_event e = {.events = EPOLLIN, .data.u64 = (uint64_t)cid};
    epoll_ctl(k_ep, EPOLL_CTL_MOD, fd, &e);
    g_sc.other++;
  }
}

static int k_run(void) {
  static uint8_t buf[65536];
  struct epoll_event ev[256];
  while (!g_quit) {
    int n = epoll_wait(k_ep, ev, 256, -1);
    g_sc.wait++;
    g_bs.passes++;
    if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); return -1; }
    for (int i = 0; i < n; i++) {
      uint64_t tag = ev[i].data.u64;
      if (tag == TAG_CTL) { ctl_ready(); continue; }
      if (tag == TAG_LISTEN) {
        for (;;) {
          int fd = accept4(k_lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
          g_sc.accept++;
          if (fd < 0) break;
          if (fd >= MAX_CONNS) { close(fd); continue; }
          int one = 1;
          setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
          struct epoll_event e = {.events = EPOLLIN, .data.u64 = (uint64_t)fd};
          epoll_ctl(k_ep, EPOLL_CTL_ADD, fd, &e);
          g_sc.other += 2;
          cid_fd[fd] = fd; /* the fd is the connection id */
          app_on_open(fd);
        }
        continue;
      }
      int cid = (int)tag;
      if (ev[i].events & EPOLLOUT) k_flush(cid);
      if (ev[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
        ssize_t r = recv(cid_fd[cid], buf, sizeof buf, 0);
        g_sc.recv++;
        if (r > 0) { g_bs.rx_bytes += (uint64_t)r; app_on_data(cid, buf, (size_t)r); }
        else if (r == 0 || (errno != EAGAIN && errno != EINTR)) k_close(cid);
      }
    }
  }
  return 0;
}

const struct backend be_kern = {"kern", k_run, k_send, NULL};

/* ---------- main ---------- */
static void on_sig(int s) { (void)s; g_quit = 1; }

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc < 2) {
    fprintf(stderr, "usage: xsrv kern|xdp|attach|detach ...\n");
    return 2;
  }
  const char *cmd = argv[1];
  if (!strcmp(cmd, "attach"))
    return xdp_attach_cmd(arg_str(argc, argv, "--if", "eth0"), arg_str(argc, argv, "--mode", "drv"), arg_str(argc, argv, "--obj", "xdp_prog.o"));
  if (!strcmp(cmd, "detach")) return xdp_detach_cmd(arg_str(argc, argv, "--if", "eth0"));

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sig; /* no SA_RESTART: the loop's wait returns EINTR and sees g_quit */
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);

  conns = calloc(MAX_CONNS, sizeof *conns);
  group_members = malloc(sizeof(int[64]) * MAX_CONNS);
  memset(group_members, 0xff, sizeof(int[64]) * MAX_CONNS);
  long spin = arg_long(argc, argv, "--spin", 20);
  calibrate(spin);
  snprintf(g_ifname, sizeof g_ifname, "%s", arg_str(argc, argv, "--if", ""));
  g_ctl = listen6((int)arg_long(argc, argv, "--ctl", 7100), 64);
  if (g_ctl < 0) { perror("ctl listen"); return 1; }
  meter_init((int)arg_long(argc, argv, "--tp-sys", 0));
  int port = (int)arg_long(argc, argv, "--port", 7000);
  if (!strcmp(cmd, "kern")) {
    g_be = &be_kern;
    if (be_kern_init(port)) return 1;
  } else if (!strcmp(cmd, "xdp")) {
    g_be = &be_xdp;
    if (be_xdp_init(argc, argv)) return 1;
  } else {
    fprintf(stderr, "unknown command %s\n", cmd);
    return 2;
  }
  printf("READY backend=%s port=%d spin_us=%ld spin_iters=%llu ns_per_iter=%.3f idle_tp=%s idle_filtered=%d tp_sys=%d\n", g_be->name, port,
         spin, (unsigned long long)g_spin_iters, g_ns_per_iter, idle_fd[0] >= 0 ? "yes" : "no", idle_filtered, tp_sys_fd >= 0);
  int rc = g_be->run();
  static char out[16384];
  snapshot(out, sizeof out);
  printf("FINAL %s", out);
  return rc ? 1 : 0;
}
