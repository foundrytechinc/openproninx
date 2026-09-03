#include "user.h"

static int ipv4(const char *text, uchar out[4]) {
  int i, value;
  for (i = 0; i < 4; i++) {
    value = 0;
    if (*text < '0' || *text > '9') return -1;
    while (*text >= '0' && *text <= '9') { value = value * 10 + *text++ - '0'; if (value > 255) return -1; }
    out[i] = value;
    if (i < 3) { if (*text++ != '.') return -1; }
  }
  return *text ? -1 : 0;
}

static int equal(const char *left, const char *right) {
  while (*left && *left == *right) { left++; right++; }
  return *left == *right;
}

int main(int argc, char **argv) {
  struct network_ipv4_config config;
  int i;
  for (i = 0; i < (int)sizeof(config); i++) ((char *)&config)[i] = 0;
  if (argc == 2 && equal(argv[1], "dhcp")) {
    config.dhcp = 1;
  } else if ((argc == 4 || argc == 5) && ipv4(argv[1], config.address) == 0 &&
             ipv4(argv[2], config.netmask) == 0 && ipv4(argv[3], config.gateway) == 0 &&
             (argc == 4 || ipv4(argv[4], config.dns_server) == 0)) {
    config.dhcp = 0;
  } else {
    printf("usage: netconfig dhcp | netconfig ADDRESS NETMASK GATEWAY [DNS]\n");
    exit();
  }
  if (netconfig(&config) < 0) printf("netconfig: rejected or network unavailable\n");
  else printf("netconfig: %s configured\n", config.dhcp ? "DHCP" : "static IPv4");
  exit();
}
