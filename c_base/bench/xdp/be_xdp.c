/* be_xdp.c : the AF_XDP + lwIP backend, and the XDP program loader.
 *
 * The XDP program (xdp_prog.c) redirects TCP to --port into an AF_XDP socket (one per NIC RX queue, all in one
 * XSKMAP) and passes everything else to the kernel, so SSH, SSM, IMDS, DNS and S3 keep working through the kernel.
 * `xsrv attach` loads and attaches it and pins its maps under /sys/fs/bpf/xdpspike; this backend opens the pinned
 * maps, binds its sockets (zero-copy first, copy mode as the fallback, need-wakeup on), puts them in the map and sets
 * the port. A run therefore never re-attaches the program (on ENA an attach resets the device).
 *
 * The loop is interrupt driven: poll() on the AF_XDP fds and the control fd, with lwIP's next timer as the timeout,
 * so the thread sleeps until the NIC's RX interrupt (its softirq fills the RX ring and wakes the socket) or a timer.
 * One pass: drain every RX ring into lwIP (each frame is copied into a pbuf and goes to ip6_input; the UMEM frame goes
 * straight back to the fill ring), run lwIP's due timers, tcp_output() every connection the app wrote to, then one
 * sendto() kick for the whole pass's TX descriptors, then reclaim the completion ring.
 * lwIP runs with NO_SYS=1 and the raw callback API, IPv6 only. Its netif's output_ip6 writes the Ethernet header
 * (destination: the router/neighbour MAC the kernel uses, given by --gw-mac or learnt from the first RX frame) and
 * copies the packet into a UMEM TX frame: lwIP does no neighbour discovery (ND/ICMPv6 stay with the kernel).
 */
#define _GNU_SOURCE
#include "be.h"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <dirent.h>
#include <errno.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lwip/init.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"

#ifndef SOL_XDP
#define SOL_XDP 283
#endif
#ifndef AF_XDP
#define AF_XDP 44
#endif

const char *arg_str(int argc, char **argv, const char *k, const char *def);
long arg_long(int argc, char **argv, const char *k, long def);

#define PIN_DIR "/sys/fs/bpf/xdpspike"
#define FRAME_SZ 4096u
#define NFRAMES 4096u /* half for RX (the fill ring), half for TX */
#define RING_SZ 2048u
#define MAXQ 8

/* ---------- the loader ---------- */
int xdp_attach_cmd(const char *ifname, const char *mode, const char *objpath) {
  int ifindex = (int)if_nametoindex(ifname);
  if (!ifindex) { fprintf(stderr, "no interface %s\n", ifname); return 1; }
  mkdir(PIN_DIR, 0700);
  const char *names[] = {"xsks", "cfg", "stats"};
  char p[128];
  for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, PIN_DIR "/%s", names[i]); unlink(p); }
  struct bpf_object *o = bpf_object__open_file(objpath, NULL);
  if (!o || libbpf_get_error(o)) { fprintf(stderr, "open %s failed\n", objpath); return 1; }
  for (int i = 0; i < 3; i++) {
    struct bpf_map *m = bpf_object__find_map_by_name(o, names[i]);
    snprintf(p, sizeof p, PIN_DIR "/%s", names[i]);
    if (!m || bpf_map__set_pin_path(m, p)) { fprintf(stderr, "map %s missing\n", names[i]); return 1; }
  }
  if (bpf_object__load(o)) { fprintf(stderr, "load failed: %s\n", strerror(errno)); return 1; }
  struct bpf_program *prog = bpf_object__find_program_by_name(o, "xdp_main");
  int fd = prog ? bpf_program__fd(prog) : -1;
  if (fd < 0) { fprintf(stderr, "no program\n"); return 1; }
  __u32 flags = !strcmp(mode, "skb") ? XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE;
  int err = bpf_xdp_attach(ifindex, fd, flags, NULL);
  if (err) {
    fprintf(stderr, "attach %s mode=%s failed: %d %s\n", ifname, mode, err, strerror(-err));
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, PIN_DIR "/%s", names[i]); unlink(p); }
    return 1;
  }
  printf("attached %s mode=%s prog_fd=%d\n", ifname, mode, fd);
  return 0;
}

int xdp_detach_cmd(const char *ifname) {
  int ifindex = (int)if_nametoindex(ifname);
  if (!ifindex) return 1;
  int a = bpf_xdp_detach(ifindex, XDP_FLAGS_DRV_MODE, NULL);
  int b = bpf_xdp_detach(ifindex, XDP_FLAGS_SKB_MODE, NULL);
  const char *names[] = {"xsks", "cfg", "stats"};
  char p[128];
  for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, PIN_DIR "/%s", names[i]); unlink(p); }
  printf("detached %s drv=%d skb=%d\n", ifname, a, b);
  return 0;
}

/* ---------- AF_XDP rings ---------- */
struct ring { uint32_t *prod, *cons, *flags; void *desc; uint32_t mask, size, local; };
struct xsk {
  int fd, qid, zc;
  uint8_t *umem;
  struct ring fq, cq, rx, tx;
  uint64_t freel[NFRAMES];
  uint32_t nfree;
};
static struct xsk xs[MAXQ];
static int nq;
static int m_xsks = -1, m_cfg = -1, m_stats = -1;
static int g_kick_poll; /* 1: rely on the next poll() to transmit instead of a sendto() kick */
static int g_tx_pending;
static uint64_t x_kicks_skipped, x_rx_nomem, x_drop_tx, x_tcp_out, x_fill_wakeups, x_learned_mac;

static inline uint32_t ld_acq(uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void st_rel(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

static int map_ring(int fd, struct ring *r, const struct xdp_ring_offset *o, size_t elem, off_t pgoff) {
  size_t len = o->desc + RING_SZ * elem;
  uint8_t *b = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, pgoff);
  if (b == MAP_FAILED) return -1;
  r->prod = (uint32_t *)(b + o->producer);
  r->cons = (uint32_t *)(b + o->consumer);
  r->flags = (uint32_t *)(b + o->flags);
  r->desc = b + o->desc;
  r->size = RING_SZ;
  r->mask = RING_SZ - 1;
  return 0;
}

/* returns 0 or -errno of the failing step; *step names it */
static int xsk_open(struct xsk *x, int ifindex, int qid, int zc, int wakeup, const char **step) {
  memset(x, 0, sizeof *x);
  x->qid = qid;
  x->fd = socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
  if (x->fd < 0) { *step = "socket"; return -errno; }
  x->umem = mmap(NULL, (size_t)NFRAMES * FRAME_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (x->umem == MAP_FAILED) { *step = "umem mmap"; return -errno; }
  struct xdp_umem_reg mr;
  memset(&mr, 0, sizeof mr);
  mr.addr = (uintptr_t)x->umem;
  mr.len = (uint64_t)NFRAMES * FRAME_SZ;
  mr.chunk_size = FRAME_SZ;
  mr.headroom = 0;
  int n = RING_SZ, e = 0;
  if (setsockopt(x->fd, SOL_XDP, XDP_UMEM_REG, &mr, sizeof mr)) { *step = "UMEM_REG"; e = -errno; goto fail; }
  if (setsockopt(x->fd, SOL_XDP, XDP_UMEM_FILL_RING, &n, sizeof n) || setsockopt(x->fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &n, sizeof n) ||
      setsockopt(x->fd, SOL_XDP, XDP_RX_RING, &n, sizeof n) || setsockopt(x->fd, SOL_XDP, XDP_TX_RING, &n, sizeof n)) {
    *step = "ring sizes"; e = -errno; goto fail;
  }
  struct xdp_mmap_offsets off;
  socklen_t ol = sizeof off;
  if (getsockopt(x->fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &ol)) { *step = "MMAP_OFFSETS"; e = -errno; goto fail; }
  if (map_ring(x->fd, &x->fq, &off.fr, sizeof(uint64_t), XDP_UMEM_PGOFF_FILL_RING) ||
      map_ring(x->fd, &x->cq, &off.cr, sizeof(uint64_t), XDP_UMEM_PGOFF_COMPLETION_RING) ||
      map_ring(x->fd, &x->rx, &off.rx, sizeof(struct xdp_desc), XDP_PGOFF_RX_RING) ||
      map_ring(x->fd, &x->tx, &off.tx, sizeof(struct xdp_desc), XDP_PGOFF_TX_RING)) {
    *step = "ring mmap"; e = -errno; goto fail;
  }
  /* RX frames into the fill ring before bind, so the driver has buffers from the first packet */
  uint64_t *fd = x->fq.desc;
  for (uint32_t i = 0; i < NFRAMES / 2; i++) fd[i & x->fq.mask] = (uint64_t)i * FRAME_SZ;
  x->fq.local = NFRAMES / 2;
  st_rel(x->fq.prod, x->fq.local);
  for (uint32_t i = NFRAMES / 2; i < NFRAMES; i++) x->freel[x->nfree++] = (uint64_t)i * FRAME_SZ;
  struct sockaddr_xdp sa;
  memset(&sa, 0, sizeof sa);
  sa.sxdp_family = AF_XDP;
  sa.sxdp_ifindex = (uint32_t)ifindex;
  sa.sxdp_queue_id = (uint32_t)qid;
  sa.sxdp_flags = (uint16_t)((zc ? XDP_ZEROCOPY : XDP_COPY) | (wakeup ? XDP_USE_NEED_WAKEUP : 0));
  if (bind(x->fd, (struct sockaddr *)&sa, sizeof sa)) { *step = "bind"; e = -errno; goto fail; }
  x->zc = zc;
  return 0;
fail:
  close(x->fd);
  munmap(x->umem, (size_t)NFRAMES * FRAME_SZ);
  x->fd = -1;
  return e;
}

static void cq_reclaim(struct xsk *x) {
  uint32_t prod = ld_acq(x->cq.prod), cons = x->cq.local;
  uint64_t *d = x->cq.desc;
  while (cons != prod) x->freel[x->nfree++] = d[cons++ & x->cq.mask];
  x->cq.local = cons;
  st_rel(x->cq.cons, cons);
}

static void kick(struct xsk *x) {
  if (!x->zc || (*x->tx.flags & XDP_RING_NEED_WAKEUP)) {
    sendto(x->fd, NULL, 0, MSG_DONTWAIT, NULL, 0);
    g_sc.kick++;
  } else x_kicks_skipped++;
}

/* ---------- lwIP glue ---------- */
static struct netif nif;
static uint8_t my_mac[6], gw_mac[6];
static int have_gw_mac;
static long drop_tx_permille;
static uint32_t seq_hi[65536];
static uint8_t seq_seen[65536];

u32_t sys_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* count retransmitted data segments: a segment whose end is not past the highest end already sent to that port */
static void note_tx_segment(const uint8_t *ip) {
  if (ip[6] != 6) return;
  uint32_t plen = ((uint32_t)ip[4] << 8) | ip[5];
  const uint8_t *t = ip + 40;
  uint32_t dport = ((uint32_t)t[2] << 8) | t[3];
  uint32_t seq = ((uint32_t)t[4] << 24) | ((uint32_t)t[5] << 16) | ((uint32_t)t[6] << 8) | t[7];
  uint32_t thl = (uint32_t)(t[12] >> 4) * 4;
  uint8_t fl = t[13];
  uint32_t dlen = plen > thl ? plen - thl : 0;
  if (fl & 0x02) { seq_seen[dport] = 1; seq_hi[dport] = seq + 1; return; }
  if (!dlen) return;
  uint32_t end = seq + dlen;
  if (seq_seen[dport] && (int32_t)(end - seq_hi[dport]) <= 0) g_bs.rexmit++;
  else { seq_hi[dport] = end; seq_seen[dport] = 1; }
}

static err_t x_output_ip6(struct netif *n, struct pbuf *p, const ip6_addr_t *dst) {
  (void)n; (void)dst;
  if (drop_tx_permille && (random() % 1000) < drop_tx_permille) { x_drop_tx++; return ERR_OK; }
  struct xsk *x = &xs[0];
  if (!x->nfree) cq_reclaim(x);
  if (!x->nfree) { kick(x); cq_reclaim(x); }
  if (!x->nfree) { g_bs.tx_full++; return ERR_MEM; }
  uint32_t len = 14u + p->tot_len;
  if (len > FRAME_SZ) return ERR_VAL;
  uint64_t fr = x->freel[--x->nfree];
  uint8_t *f = x->umem + fr;
  memcpy(f, gw_mac, 6);
  memcpy(f + 6, my_mac, 6);
  f[12] = 0x86;
  f[13] = 0xdd;
  pbuf_copy_partial(p, f + 14, p->tot_len, 0);
  note_tx_segment(f + 14);
  struct xdp_desc *d = x->tx.desc;
  d[x->tx.local & x->tx.mask] = (struct xdp_desc){.addr = fr, .len = len, .options = 0};
  x->tx.local++;
  st_rel(x->tx.prod, x->tx.local);
  g_bs.tx_pkts++;
  g_tx_pending = 1;
  return ERR_OK;
}

static err_t nif_init(struct netif *n) {
  n->name[0] = 'x';
  n->name[1] = 's';
  n->output_ip6 = x_output_ip6;
  n->mtu = 1500;
  n->hwaddr_len = 6;
  memcpy(n->hwaddr, my_mac, 6);
  n->flags |= NETIF_FLAG_ETHERNET;
  return ERR_OK;
}

static void rx_frame(const uint8_t *d, uint32_t len) {
  g_bs.rx_pkts++;
  g_bs.rx_bytes += len;
  if (!have_gw_mac) { memcpy(gw_mac, d + 6, 6); have_gw_mac = 1; x_learned_mac = 1; }
  if (len < 14 + 40) return;
  struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)(len - 14), PBUF_POOL);
  if (!p) { x_rx_nomem++; return; }
  pbuf_take(p, d + 14, (u16_t)(len - 14));
  ip6_input(p, &nif); /* consumes p */
}

static void rx_drain(struct xsk *x) {
  uint32_t prod = ld_acq(x->rx.prod), cons = x->rx.local;
  if (prod == cons) return;
  struct xdp_desc *rd = x->rx.desc;
  uint64_t *fd = x->fq.desc;
  while (cons != prod) {
    struct xdp_desc d = rd[cons++ & x->rx.mask];
    rx_frame(x->umem + d.addr, d.len);
    fd[x->fq.local++ & x->fq.mask] = d.addr & ~(uint64_t)(FRAME_SZ - 1);
  }
  x->rx.local = cons;
  st_rel(x->rx.cons, cons);
  st_rel(x->fq.prod, x->fq.local);
}

/* ---------- connections ---------- */
struct lconn { struct tcp_pcb *pcb; uint8_t *pend; size_t pn, pcap; int dirty; };
static struct lconn lc[MAX_CONNS];
static int dirty[MAX_CONNS], ndirty, next_cid;

static void mark_dirty(int cid) {
  if (!lc[cid].dirty) { lc[cid].dirty = 1; dirty[ndirty++] = cid; }
}

static void write_pending(int cid) {
  struct lconn *c = &lc[cid];
  size_t done = 0;
  while (c->pcb && done < c->pn) {
    size_t room = tcp_sndbuf(c->pcb);
    if (!room || tcp_sndqueuelen(c->pcb) >= TCP_SND_QUEUELEN - 4) break;
    size_t w = c->pn - done;
    if (w > room) w = room;
    if (w > 0xffff) w = 0xffff;
    if (tcp_write(c->pcb, c->pend + done, (u16_t)w, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
    done += w;
  }
  if (done) { memmove(c->pend, c->pend + done, c->pn - done); c->pn -= done; }
}

static int x_send(int cid, const void *p, size_t n) {
  struct lconn *c = &lc[cid];
  if (!c->pcb) return -1;
  if (c->pn == 0 && n <= tcp_sndbuf(c->pcb) && tcp_sndqueuelen(c->pcb) < TCP_SND_QUEUELEN - 4 &&
      tcp_write(c->pcb, p, (u16_t)n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
    mark_dirty(cid);
    return 0;
  }
  if (c->pn + n > c->pcap) { c->pcap = (c->pn + n) * 2; c->pend = realloc(c->pend, c->pcap); }
  memcpy(c->pend + c->pn, p, n);
  c->pn += n;
  g_bs.sendq_bytes += n;
  mark_dirty(cid);
  return 0;
}

static void conn_gone(int cid) {
  lc[cid].pcb = NULL;
  lc[cid].pn = 0;
  app_on_close(cid);
}

static void on_err(void *arg, err_t err) {
  (void)err;
  conn_gone((int)(intptr_t)arg);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
  (void)pcb; (void)len;
  int cid = (int)(intptr_t)arg;
  if (lc[cid].pn) mark_dirty(cid);
  return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
  (void)err;
  int cid = (int)(intptr_t)arg;
  if (!p) {
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    conn_gone(cid);
    if (tcp_close(pcb) != ERR_OK) { tcp_abort(pcb); return ERR_ABRT; }
    return ERR_OK;
  }
  for (struct pbuf *q = p; q; q = q->next) app_on_data(cid, q->payload, q->len);
  tcp_recved(pcb, p->tot_len);
  pbuf_free(p);
  return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
  (void)arg;
  if (err != ERR_OK || !pcb) return ERR_VAL;
  int cid = -1;
  for (int i = 0; i < MAX_CONNS; i++) {
    int c = (next_cid + i) % MAX_CONNS;
    if (!lc[c].pcb) { cid = c; break; }
  }
  if (cid < 0) { tcp_abort(pcb); return ERR_ABRT; }
  next_cid = cid + 1;
  lc[cid].pcb = pcb;
  lc[cid].pn = 0;
  tcp_arg(pcb, (void *)(intptr_t)cid);
  tcp_recv(pcb, on_recv);
  tcp_sent(pcb, on_sent);
  tcp_err(pcb, on_err);
  tcp_nagle_disable(pcb);
  app_on_open(cid);
  return ERR_OK;
}

static void flush_dirty(void) {
  static int work[MAX_CONNS];
  int n = ndirty; /* take this pass's list; a connection still blocked goes on the fresh one, for the next pass */
  memcpy(work, dirty, sizeof(int) * (size_t)n);
  ndirty = 0;
  for (int i = 0; i < n; i++) {
    int cid = work[i];
    lc[cid].dirty = 0;
    if (!lc[cid].pcb) continue;
    if (lc[cid].pn) write_pending(cid);
    tcp_output(lc[cid].pcb);
    x_tcp_out++;
    if (lc[cid].pn) mark_dirty(cid); /* on_sent re-marks it too when ACKs free the send buffer */
  }
}

/* ---------- init and loop ---------- */
static long drop_rx_permille;

static int read_mac(const char *ifname, uint8_t *mac) {
  char p[128], b[64];
  snprintf(p, sizeof p, "/sys/class/net/%s/address", ifname);
  FILE *f = fopen(p, "r");
  if (!f) return -1;
  int ok = fgets(b, sizeof b, f) != NULL;
  fclose(f);
  unsigned m[6];
  if (!ok || sscanf(b, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) return -1;
  for (int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];
  return 0;
}

static int count_rx_queues(const char *ifname) {
  char p[128];
  snprintf(p, sizeof p, "/sys/class/net/%s/queues", ifname);
  DIR *d = opendir(p);
  if (!d) return 1;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d))) n += !strncmp(e->d_name, "rx-", 3);
  closedir(d);
  return n ? n : 1;
}

int be_xdp_init(int argc, char **argv) {
  const char *ifname = arg_str(argc, argv, "--if", "eth0");
  const char *ip6s = arg_str(argc, argv, "--ip6", NULL);
  const char *gws = arg_str(argc, argv, "--gw-mac", NULL);
  const char *zcs = arg_str(argc, argv, "--zc", "auto");
  int port = (int)arg_long(argc, argv, "--port", 7000);
  int maxq = (int)arg_long(argc, argv, "--queues", 0);
  drop_rx_permille = arg_long(argc, argv, "--drop-rx", 0);
  drop_tx_permille = arg_long(argc, argv, "--drop-tx", 0);
  g_kick_poll = (int)arg_long(argc, argv, "--kick-poll", 0);
  srandom((unsigned)time(NULL));
  if (!ip6s) { fprintf(stderr, "--ip6 required\n"); return -1; }
  int ifindex = (int)if_nametoindex(ifname);
  if (!ifindex || read_mac(ifname, my_mac)) { fprintf(stderr, "interface %s?\n", ifname); return -1; }
  if (gws) {
    unsigned m[6];
    if (sscanf(gws, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
      for (int i = 0; i < 6; i++) gw_mac[i] = (uint8_t)m[i];
      have_gw_mac = 1;
    }
  }
  m_xsks = bpf_obj_get(PIN_DIR "/xsks");
  m_cfg = bpf_obj_get(PIN_DIR "/cfg");
  m_stats = bpf_obj_get(PIN_DIR "/stats");
  if (m_xsks < 0 || m_cfg < 0 || m_stats < 0) { fprintf(stderr, "pinned maps missing: run xsrv attach first\n"); return -1; }

  nq = count_rx_queues(ifname);
  if (maxq > 0 && nq > maxq) nq = maxq;
  if (nq > MAXQ) nq = MAXQ;
  for (int q = 0; q < nq; q++) {
    const char *step = "";
    int e = -EOPNOTSUPP;
    int tried_zc = strcmp(zcs, "copy") != 0;
    if (tried_zc) {
      e = xsk_open(&xs[q], ifindex, q, 1, 1, &step);
      printf("XSK q=%d zerocopy bind: %s%s%s\n", q, e ? "FAILED at " : "ok", e ? step : "", e ? "" : "");
      if (e) printf("XSK q=%d zerocopy error: %s\n", q, strerror(-e));
    }
    if (e && strcmp(zcs, "zc") != 0) {
      e = xsk_open(&xs[q], ifindex, q, 0, 1, &step);
      printf("XSK q=%d copy-mode bind: %s%s\n", q, e ? "FAILED at " : "ok", e ? step : "");
      if (e) printf("XSK q=%d copy-mode error: %s\n", q, strerror(-e));
    }
    if (e) return -1;
    __u32 key = (__u32)q, val = (__u32)xs[q].fd;
    if (bpf_map_update_elem(m_xsks, &key, &val, 0)) { perror("xsks update"); return -1; }
  }

  lwip_init();
  netif_add_noaddr(&nif, NULL, nif_init, netif_input);
  ip6_addr_t a;
  if (!ip6addr_aton(ip6s, &a)) { fprintf(stderr, "bad --ip6\n"); return -1; }
  s8_t idx = -1;
  if (netif_add_ip6_address(&nif, &a, &idx) != ERR_OK) { fprintf(stderr, "netif_add_ip6_address failed\n"); return -1; }
  netif_ip6_addr_set_state(&nif, idx, IP6_ADDR_PREFERRED);
  netif_set_default(&nif);
  netif_set_link_up(&nif);
  netif_set_up(&nif);
  struct tcp_pcb *l = tcp_new_ip_type(IPADDR_TYPE_V6);
  if (!l || tcp_bind(l, IP6_ADDR_ANY, (u16_t)port) != ERR_OK) { fprintf(stderr, "lwip bind failed\n"); return -1; }
  l = tcp_listen_with_backlog(l, 255);
  tcp_accept(l, on_accept);

  /* only now turn the redirect on: from here, TCP to the port goes to the sockets */
  __u32 k0 = 0, k1 = 1, vport = (__u32)port, vdrop = (__u32)drop_rx_permille;
  bpf_map_update_elem(m_cfg, &k1, &vdrop, 0);
  bpf_map_update_elem(m_cfg, &k0, &vport, 0);
  printf("XDP queues=%d zc=%d gw_mac=%s kick=%s drop_rx=%ld drop_tx=%ld\n", nq, xs[0].zc, have_gw_mac ? "given" : "learn-from-rx",
         g_kick_poll ? "poll" : "sendto", drop_rx_permille, drop_tx_permille);
  return 0;
}

static int x_run(void) {
  struct pollfd pf[MAXQ + 1];
  for (int q = 0; q < nq; q++) pf[q] = (struct pollfd){.fd = xs[q].fd, .events = POLLIN};
  pf[nq] = (struct pollfd){.fd = ctl_fd(), .events = POLLIN};
  while (!g_quit) {
    u32_t sl = sys_timeouts_sleeptime();
    int to = sl == SYS_TIMEOUTS_SLEEPTIME_INFINITE ? -1 : (int)sl;
    for (int q = 0; q < nq; q++)
      if (*xs[q].fq.flags & XDP_RING_NEED_WAKEUP) x_fill_wakeups++;
    int n = poll(pf, (nfds_t)(nq + 1), to);
    g_sc.wait++;
    g_bs.passes++;
    if (n < 0 && errno != EINTR) { perror("poll"); return -1; }
    if (n > 0 && (pf[nq].revents & POLLIN)) ctl_ready();
    for (int q = 0; q < nq; q++) rx_drain(&xs[q]);
    sys_check_timeouts();
    flush_dirty();
    if (g_tx_pending) {
      if (g_kick_poll) x_kicks_skipped++; /* the next poll() transmits (copy mode: xsk_generic_xmit; zc: the wakeup) */
      else kick(&xs[0]);
      g_tx_pending = 0;
    }
    cq_reclaim(&xs[0]);
  }
  /* stop redirecting before the sockets close */
  __u32 k0 = 0, k1 = 1, z = 0;
  bpf_map_update_elem(m_cfg, &k0, &z, 0);
  bpf_map_update_elem(m_cfg, &k1, &z, 0);
  return 0;
}

static void x_snap(char *out, size_t cap) {
  uint64_t st[4] = {0};
  int ncpus = libbpf_num_possible_cpus();
  if (ncpus > 0 && ncpus <= 256) {
    uint64_t v[256];
    for (__u32 k = 0; k < 4; k++)
      if (!bpf_map_lookup_elem(m_stats, &k, v))
        for (int c = 0; c < ncpus; c++) st[k] += v[c];
  }
  int npcb = 0;
  for (int i = 0; i < MAX_CONNS; i++) npcb += lc[i].pcb != NULL;
  snprintf(out, cap,
           " xdp_redirect=%llu xdp_drop=%llu xdp_pass=%llu xdp_other=%llu zc=%d nq=%d kicks_skipped=%llu rx_nomem=%llu drop_tx=%llu "
           "tcp_out=%llu fill_wakeups=%llu learned_mac=%llu pcbs=%d",
           (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2], (unsigned long long)st[3], xs[0].zc, nq,
           (unsigned long long)x_kicks_skipped, (unsigned long long)x_rx_nomem, (unsigned long long)x_drop_tx,
           (unsigned long long)x_tcp_out, (unsigned long long)x_fill_wakeups, (unsigned long long)x_learned_mac, npcb);
}

const struct backend be_xdp = {"xdp", x_run, x_send, x_snap};
