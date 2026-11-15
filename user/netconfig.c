#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fnu/sys.h>

static void usage(void) {
  dprintf(2, "usage: netconfig interface dhcp\n"
             "       netconfig interface address netmask gateway [dns]\n");
  exit(1);
}

int main(int argc, char **argv) {
  struct fnu_ifconfig config;

  memset(&config, 0, sizeof(config));
  if (argc < 3 || strlen(argv[1]) >= sizeof(config.name))
    usage();
  strncpy(config.name, argv[1], sizeof(config.name) - 1);
  if (argc == 3 && strcmp(argv[2], "dhcp") == 0)
    config.dhcp = 1;
  else if ((argc == 5 || argc == 6) && fnu_addr_parse(argv[2], config.address) == 0 &&
           fnu_addr_parse(argv[3], config.netmask) == 0 &&
           fnu_addr_parse(argv[4], config.gateway) == 0 &&
           (argc == 5 || fnu_addr_parse(argv[5], config.dns_server) == 0))
    config.dhcp = 0;
  else
    usage();
  if (fnu_netconfig(&config) < 0) {
    dprintf(2, "netconfig: %s: refused, or no such interface\n", config.name);
    exit(1);
  }
  printf("netconfig: %s %s\n", config.name,
         config.dhcp ? "asks for a lease" : "configured");
  exit(0);
}
