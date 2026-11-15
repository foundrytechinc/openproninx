#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fnu/sys.h>

#define NTP_EPOCH 2208988800U

int main(int argc, char **argv) {
  struct sockaddr_in server, from;
  socklen_t fromlen = sizeof(struct sockaddr_in);
  uchar packet[48];
  struct in_addr addr;
  uint seconds;
  int s, i, length;

  if (argc != 2 || !inet_aton(argv[1], &addr)) {
    dprintf(2, "usage: ntp IPv4-server\n");
    exit(1);
  }
  memset(&server, 0, sizeof(server));
  server.sin_len = sizeof(server);
  server.sin_family = AF_INET;
  server.sin_port = htons(123);
  server.sin_addr.s_addr = addr.s_addr;
  for (i = 0; i < (int)sizeof(packet); i++) packet[i] = 0;
  packet[0] = 0x1b;
  if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    dprintf(2, "ntp: socket: no descriptor\n");
    exit(1);
  }
  if (sendto(s, packet, sizeof(packet), 0, (struct sockaddr *)&server, sizeof(server)) != (int)sizeof(packet) ||
      fnu_waitread(s, 3000) < 0 ||
      (length = recvfrom(s, packet, sizeof(packet), 0, (struct sockaddr *)&from, &fromlen)) < 48) {
    dprintf(2, "ntp: no valid response\n");
    close(s);
    exit(1);
  }
  seconds = ((uint)packet[40] << 24) | ((uint)packet[41] << 16) |
            ((uint)packet[42] << 8) | packet[43];
  if (seconds < NTP_EPOCH) {
    dprintf(2, "ntp: invalid server time\n");
    close(s);
    exit(1);
  }
  printf("ntp: Unix time %u seconds (server %s)\n", seconds - NTP_EPOCH,
         inet_ntoa(from.sin_addr));
  close(s);
  exit(0);
}
