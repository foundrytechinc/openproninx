#include "user.h"

int main(void) {
  struct network_status status;
  if (netinfo(&status) < 0) {
    printf("netinfo: network is unavailable\n");
    exit();
  }
  printf("%s  mac %x:%x:%x:%x:%x:%x\n", status.interface_name,
         status.hardware_address[0], status.hardware_address[1],
         status.hardware_address[2], status.hardware_address[3],
         status.hardware_address[4], status.hardware_address[5]);
  printf("ipv4 %d.%d.%d.%d  gateway %d.%d.%d.%d  dns %d.%d.%d.%d  dhcp %d\n",
         status.address[0], status.address[1], status.address[2],
         status.address[3], status.gateway[0], status.gateway[1],
         status.gateway[2], status.gateway[3], status.dns_server[0],
         status.dns_server[1], status.dns_server[2], status.dns_server[3],
         status.dhcp_bound);
  printf("frames rx=%d tx=%d dropped=%d\n", status.received_frames,
         status.transmitted_frames, status.dropped_frames);
  exit();
}
