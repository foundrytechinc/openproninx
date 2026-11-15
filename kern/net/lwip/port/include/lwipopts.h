#ifndef PRONINX_LWIPOPTS_H
#define PRONINX_LWIPOPTS_H

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_TIMERS 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_RAW 1
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ICMP 1
#define LWIP_DHCP 1
#define LWIP_ACD 0
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_AUTOIP 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_IGMP 0
#define LWIP_DNS 0
#define DHCP_ADD_EXTRA_REQUEST_OPTIONS , DHCP_OPTION_DNS_SERVER
#define LWIP_HOOK_DHCP_PARSE_OPTION(netif, dhcp, state, msg, msg_type, option, len, pbuf, offset) \
  proninx_dhcp_option((msg_type), (option), (len), (pbuf), (offset))
#define SO_REUSE 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_NETIF_API 0
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_HAVE_LOOPIF 1
#define LWIP_NETIF_LOOPBACK 1
#define LWIP_LOOPBACK_MAX_PBUFS 64
#define LWIP_STATS 0
#define LWIP_NOASSERT 1
#define LWIP_CHECKSUM_CTRL_PER_NETIF 0
#define LWIP_CHKSUM_ALGORITHM 3
#define MEM_CUSTOM_ALLOCATOR 1
#define MEM_CUSTOM_MALLOC proninx_lwip_malloc
#define MEM_CUSTOM_CALLOC proninx_lwip_calloc
#define MEM_CUSTOM_FREE proninx_lwip_free
#define MEMP_MEM_MALLOC 1
#define MEM_ALIGNMENT 8
#define LWIP_SUPPORT_CUSTOM_PBUF 1
#define TCP_MSS 1460
// sockbuf ceiling; the scale is the least that reaches it
#define PRONINX_SB_MAX (2 * 1024 * 1024)
#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE 6
#define TCP_SND_BUF PRONINX_SB_MAX
#define TCP_SNDLOWAT (2 * TCP_MSS + 1) // netconn only
#define TCP_WND PRONINX_SB_MAX
#define LWIP_TCP_PCB_NUM_EXT_ARGS 1
#define TCP_LISTEN_BACKLOG 1
#define PBUF_POOL_BUFSIZE 1600
#define ARP_TABLE_SIZE 64
#define ARP_QUEUEING 1
#define ETHARP_SUPPORT_STATIC_ENTRIES 0

#define LWIP_HOOK_FILENAME "lwip_hooks.h"
#define LWIP_HOOK_TCP_ISN proninx_tcp_isn
#define LWIP_HOOK_TCP_OUT_ADD_TCPOPTS proninx_tcp_out

#endif
