#ifndef PRONINX_LWIP_HOOKS_H
#define PRONINX_LWIP_HOOKS_H

#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

struct tcp_hdr;
struct tcp_pcb;

u32_t *proninx_tcp_out(struct pbuf *p, struct tcp_hdr *hdr,
                       const struct tcp_pcb *pcb, u32_t *opts);
u32_t proninx_tcp_isn(const ip_addr_t *local_ip, u16_t local_port,
                      const ip_addr_t *remote_ip, u16_t remote_port);
void proninx_dhcp_option(u8_t msg_type, u8_t option, u8_t len, struct pbuf *p,
                         u16_t offset);

#endif
