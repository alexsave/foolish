/* be.h : the one tiny interface between the server's app (srv.c) and its two network backends.
 *
 * A backend owns the event loop and the connections and names each connection by a small integer id.
 * It calls the app when a connection opens, when bytes arrive, and when it closes; the app calls be_send.
 * be_send may queue the bytes (the lwIP backend always does, until the end of the loop pass); bytes are never dropped.
 */
#ifndef XDP_SPIKE_BE_H
#define XDP_SPIKE_BE_H
#include <stddef.h>
#include <stdint.h>

#define MAX_CONNS 4096

/* app side, implemented in srv.c */
void app_on_open(int cid);
void app_on_data(int cid, const uint8_t *p, size_t n);
void app_on_close(int cid);
/* the control socket (kernel TCP, port --ctl) is served by srv.c; a backend polls ctl_fd() and calls ctl_ready() */
int ctl_fd(void);
void ctl_ready(void);
extern volatile int g_quit;

/* syscall counters, bumped by the backends at each call site (exact, and free compared to tracing every syscall) */
struct sc_count { uint64_t wait, recv, send, accept, kick, other, ctl; };
extern struct sc_count g_sc;
/* backend counters shown in the snapshot */
struct be_stats { uint64_t passes, rx_pkts, tx_pkts, tx_full, rexmit, sendq_bytes, rx_bytes; };
extern struct be_stats g_bs;

struct backend {
  const char *name;
  int (*run)(void);                     /* the loop; returns at g_quit */
  int (*send)(int cid, const void *p, size_t n);
  void (*snap_extra)(char *out, size_t cap); /* backend-specific k=v pairs appended to a snapshot */
};

extern const struct backend be_kern;
extern const struct backend be_xdp;
int be_kern_init(int port);
int be_xdp_init(int argc, char **argv);
/* the XDP program loader (be_xdp.c), also used by the attach/detach/kernel-with-xdp commands */
int xdp_attach_cmd(const char *ifname, const char *mode, const char *obj);
int xdp_detach_cmd(const char *ifname);

#endif
