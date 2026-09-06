# lwIP integration

PRONINX uses the upstream lwIP 2.2.1 source-only vendor import in
`third_party/lwip`.  This directory is the PRONINX-owned, single-core `NO_SYS`
port.  It enables Ethernet, ARP, IPv4, ICMP, DHCP, UDP and TCP via the lwIP
raw/callback API; IPv6 and netconn APIs are disabled.
