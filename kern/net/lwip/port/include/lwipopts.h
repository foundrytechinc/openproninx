#ifndef PRONINX_LWIPOPTS_H
#define PRONINX_LWIPOPTS_H

/* A single-core, polling lwIP port. Network input and timers run in kernel
 * interrupt context, so the sequential and BSD socket APIs stay disabled. */
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
#define LWIP_DNS 1
#define DNS_TABLE_SIZE 4
#define DNS_MAX_NAME_LENGTH 64
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_NETIF_API 0
#define LWIP_HAVE_LOOPIF 0
#define LWIP_NETIF_LOOPBACK 0
#define LWIP_STATS 0
#define LWIP_DEBUG 0
#define LWIP_NOASSERT 1
#define LWIP_CHECKSUM_CTRL_PER_NETIF 0
#define LWIP_CHKSUM_ALGORITHM 3
#define MEM_LIBC_MALLOC 0
#define MEM_SIZE (16 * 1024)
#define MEMP_MEM_MALLOC 0
#define MEMP_NUM_PBUF 16
#define MEMP_NUM_RAW_PCB 2
#define MEMP_NUM_UDP_PCB 8
#define MEMP_NUM_TCP_PCB 8
#define MEMP_NUM_TCP_PCB_LISTEN 4
#define TCP_MSS 1460
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_WND (4 * TCP_MSS)
#define MEMP_NUM_SYS_TIMEOUT 16
#define PBUF_POOL_SIZE 16
#define PBUF_POOL_BUFSIZE 1600
#define ARP_TABLE_SIZE 8
#define ARP_QUEUEING 0
#define ETHARP_SUPPORT_STATIC_ENTRIES 0

#endif
