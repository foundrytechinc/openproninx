# lwIP integration

PRONINX uses the upstream lwIP 2.2.1 source-only vendor import in
`third_party/lwip`.  This directory is the PRONINX-owned, single-core `NO_SYS`
port.  It enables Ethernet, ARP, IPv4, ICMP, DHCP and UDP raw APIs; TCP,
IPv6, netconn and BSD socket APIs are deliberately disabled for this first
networking milestone.
