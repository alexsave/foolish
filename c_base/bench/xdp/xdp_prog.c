/* xdp_prog.c : TCP over IPv6 to cfg[0] (the test port) goes to the AF_XDP socket of the packet's RX queue; every other
 * packet (SSH, SSM, IMDS, DNS, S3, ICMPv6/ND, the control port) gets XDP_PASS to the kernel.
 * cfg[0] = 0 passes everything (the kernel backend with the program attached), cfg[1] = drop per mille of the
 * redirected packets (the retransmission test). A queue without a socket falls back to XDP_PASS (the redirect flag).
 * stats (per cpu): 0 redirected, 1 dropped by the test, 2 passed, 3 redirect failed.
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

struct {
  __uint(type, BPF_MAP_TYPE_XSKMAP);
  __uint(max_entries, 64);
  __type(key, __u32);
  __type(value, __u32);
} xsks SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, 4);
  __type(key, __u32);
  __type(value, __u32);
} cfg SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 4);
  __type(key, __u32);
  __type(value, __u64);
} stats SEC(".maps");

static __always_inline void bump(__u32 k) {
  __u64 *v = bpf_map_lookup_elem(&stats, &k);
  if (v) (*v)++;
}

SEC("xdp")
int xdp_main(struct xdp_md *ctx) {
  void *data = (void *)(long)ctx->data, *end = (void *)(long)ctx->data_end;
  struct ethhdr *eth = data;
  if ((void *)(eth + 1) > end || eth->h_proto != bpf_htons(ETH_P_IPV6)) goto pass;
  struct ipv6hdr *ip6 = (void *)(eth + 1);
  if ((void *)(ip6 + 1) > end || ip6->nexthdr != IPPROTO_TCP) goto pass;
  struct tcphdr *tcp = (void *)(ip6 + 1);
  if ((void *)(tcp + 1) > end) goto pass;
  __u32 k = 0;
  __u32 *port = bpf_map_lookup_elem(&cfg, &k);
  if (!port || !*port || tcp->dest != bpf_htons((__u16)*port)) goto pass;
  k = 1;
  __u32 *drop = bpf_map_lookup_elem(&cfg, &k);
  if (drop && *drop && bpf_get_prandom_u32() % 1000 < *drop) {
    bump(1);
    return XDP_DROP;
  }
  int r = bpf_redirect_map(&xsks, ctx->rx_queue_index, XDP_PASS);
  bump(r == XDP_REDIRECT ? 0 : 3);
  return r;
pass:
  bump(2);
  return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
