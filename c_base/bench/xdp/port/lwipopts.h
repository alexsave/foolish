/* lwipopts.h : lwIP for the spike. NO_SYS, raw callback API, IPv6 only, TCP only, heap from libc.
 * lwIP does no neighbour discovery, MLD or autoconfiguration: the kernel owns ICMPv6 on the shared interface and the
 * netif's output_ip6 writes the Ethernet header itself (be_xdp.c). */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_TIMERS 1
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_CALLBACK_API 1
#define MEM_LIBC_MALLOC 1
#define MEMP_MEM_MALLOC 1
#define MEM_ALIGNMENT 8
#define LWIP_STATS 0
#define LWIP_SINGLE_NETIF 1
#define LWIP_NETIF_LOOPBACK 0
#define LWIP_HAVE_LOOPIF 0
#define LWIP_CHKSUM_ALGORITHM 3

#define LWIP_IPV4 0
#define LWIP_ARP 0
#define LWIP_ETHERNET 1
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_IGMP 0
#define LWIP_ICMP 0
#define LWIP_IPV6 1
#define LWIP_ICMP6 1
#define LWIP_IPV6_MLD 0
#define LWIP_IPV6_AUTOCONFIG 0
#define LWIP_IPV6_DUP_DETECT_ATTEMPTS 0
#define LWIP_IPV6_SEND_ROUTER_SOLICIT 0
#define LWIP_ND6_ALLOW_RA_UPDATES 0
#define LWIP_ND6_QUEUEING 0
#define LWIP_IPV6_FRAG 0
#define LWIP_IPV6_REASS 0
#define LWIP_IPV6_DHCP6 0
#define LWIP_IPV6_NUM_ADDRESSES 3

#define LWIP_UDP 0
#define LWIP_RAW 0
#define LWIP_DNS 0
#define LWIP_TCP 1
#define TCP_MSS 1440
#define TCP_WND (32 * TCP_MSS)
#define TCP_SND_BUF (32 * TCP_MSS)
#define TCP_SND_QUEUELEN (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_SNDLOWAT (TCP_MSS * 2)
#define TCP_QUEUE_OOSEQ 1
#define LWIP_TCP_SACK_OUT 0
#define LWIP_TCP_TIMESTAMPS 0
#define LWIP_TCP_KEEPALIVE 0
#define TCP_LISTEN_BACKLOG 1
#define MEMP_NUM_TCP_PCB 4096
#define MEMP_NUM_TCP_PCB_LISTEN 4
#define MEMP_NUM_TCP_SEG 16384
#define PBUF_POOL_SIZE 4096
#define PBUF_POOL_BUFSIZE 1600
#define MEMP_NUM_PBUF 4096
#define LWIP_DEBUG 0
#endif
