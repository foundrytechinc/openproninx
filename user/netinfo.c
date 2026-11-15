#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fnu/sys.h>

static const char *link_name(int link) {
  switch (link) {
  case FNU_LINK_DOWN:
    return "down";
  case FNU_LINK_UP:
    return "up";
  case FNU_LINK_FULL_DUPLEX:
    return "up, full duplex";
  default:
    return "unknown";
  }
}

static void show(const struct fnu_ifinfo *e) {
  printf("%s: index %u mtu %u link %s", e->name, e->index, e->mtu,
         link_name(e->link));
  if (e->baudrate != 0)
    printf(" %lu Mbps", (unsigned long)(e->baudrate / 1000000));
  printf("\n\tether %02x:%02x:%02x:%02x:%02x:%02x\n", e->lladdr[0], e->lladdr[1],
         e->lladdr[2], e->lladdr[3], e->lladdr[4], e->lladdr[5]);
  printf("\tinet %d.%d.%d.%d netmask %d.%d.%d.%d gateway %d.%d.%d.%d%s%s\n",
         e->address[0], e->address[1], e->address[2], e->address[3],
         e->netmask[0], e->netmask[1], e->netmask[2], e->netmask[3],
         e->gateway[0], e->gateway[1], e->gateway[2], e->gateway[3],
         (e->flags & FNU_IFF_DHCP) ? " leased" : "",
         (e->flags & FNU_IFF_DEFAULT) ? " default" : "");
  printf("\tin %lu packets %lu bytes %lu dropped, out %lu packets %lu bytes "
         "%lu errors\n",
         (unsigned long)e->ipackets, (unsigned long)e->ibytes,
         (unsigned long)e->iqdrops, (unsigned long)e->opackets,
         (unsigned long)e->obytes, (unsigned long)e->oerrors);
}

int main(int argc, char **argv) {
  struct fnu_ifinfo *ifs;
  char dns[128];
  uint64_t len = sizeof(dns);
  int n, i, shown = 0;

  if (argc > 2) {
    dprintf(2, "usage: netinfo [interface]\n");
    exit(1);
  }
  if ((n = fnu_interfaces(&ifs)) < 0) {
    dprintf(2, "netinfo: the interfaces cannot be read\n");
    exit(1);
  }
  for (i = 0; i < n; i++)
    if (argc == 1 || strcmp(argv[1], ifs[i].name) == 0) {
      show(&ifs[i]);
      shown++;
    }
  if (argc == 2 && shown == 0) {
    dprintf(2, "netinfo: %s: no such interface\n", argv[1]);
    exit(1);
  }
  if (argc == 1 && n == 0)
    printf("no interfaces\n");
  if (argc == 1 && fnu_sysctl("net.inet.dns.servers", dns, &len, 0, 0) == 0 &&
      dns[0] != 0)
    printf("dns %s\n", dns);
  fnu_table_free(ifs);
  exit(0);
}
