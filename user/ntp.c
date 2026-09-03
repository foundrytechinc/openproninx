#include "user.h"

static int ipv4(const char *text, uchar out[4]) {
  int i, value;
  for (i = 0; i < 4; i++) {
    value = 0;
    if (*text < '0' || *text > '9') return -1;
    while (*text >= '0' && *text <= '9') { value = value * 10 + *text++ - '0'; if (value > 255) return -1; }
    out[i] = value;
    if (i < 3 && *text++ != '.') return -1;
  }
  return *text ? -1 : 0;
}

int main(int argc, char **argv) {
  uchar packet[48];
  struct network_endpoint server, peer;
  uint seconds;
  int socket, i, length;
  if (argc != 2 || ipv4(argv[1], server.address) < 0) {
    printf("usage: ntp IPv4-server\n"); exit();
  }
  server.port = 123;
  for (i = 0; i < (int)sizeof(packet); i++) packet[i] = 0;
  packet[0] = 0x1b; /* NTPv3 client request. */
  socket = udp_open();
  if (socket < 0 || udp_sendto(socket, packet, sizeof(packet), &server) != sizeof(packet) ||
      (length = udp_recvfrom(socket, packet, sizeof(packet), &peer, 3000)) < 48) {
    printf("ntp: no valid response\n"); if (socket >= 0) udp_close(socket); exit();
  }
  seconds = ((uint)packet[40] << 24) | ((uint)packet[41] << 16) |
            ((uint)packet[42] << 8) | packet[43];
  if (seconds < 2208988800U) printf("ntp: invalid server time\n");
  else printf("ntp: Unix time %d seconds (server %d.%d.%d.%d)\n", seconds - 2208988800U,
              peer.address[0], peer.address[1], peer.address[2], peer.address[3]);
  udp_close(socket);
  exit();
}
