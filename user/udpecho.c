#include "user.h"

static int number(const char *text) {
  int value = 0;
  if (!*text) return -1;
  while (*text) {
    if (*text < '0' || *text > '9') return -1;
    value = value * 10 + *text++ - '0';
    if (value > 65535) return -1;
  }
  return value;
}

static int address(const char *text, uchar out[4]) {
  int i, value;
  for (i = 0; i < 4; i++) {
    value = 0;
    if (*text < '0' || *text > '9') return -1;
    while (*text >= '0' && *text <= '9') {
      value = value * 10 + *text++ - '0';
      if (value > 255) return -1;
    }
    out[i] = value;
    if (i != 3) { if (*text++ != '.') return -1; }
  }
  return *text ? -1 : 0;
}

int main(int argc, char **argv) {
  int socket, port, length;
  char message[256];
  struct network_endpoint peer;
  if (argc != 3 && argc != 4) {
    printf("usage: udpecho PORT [IPv4 MESSAGE]\n");
    exit();
  }
  port = number(argv[1]);
  if (port <= 0 || (argc == 4 && address(argv[2], peer.address) < 0)) {
    printf("udpecho: invalid endpoint\n"); exit();
  }
  socket = udp_open();
  if (socket < 0 || udp_bind(socket, port) < 0) {
    printf("udpecho: cannot bind UDP port %d\n", port); exit();
  }
  if (argc == 4) {
    peer.port = port;
    length = 0;
    while (argv[3][length] && length < (int)sizeof(message)) message[length] = argv[3][length], length++;
    if (udp_sendto(socket, message, length, &peer) != length ||
        (length = udp_recvfrom(socket, message, sizeof(message), &peer, 2000)) < 0)
      printf("udpecho: no reply\n");
    else
      printf("udp reply from %d.%d.%d.%d:%d: %.*s\n", peer.address[0], peer.address[1], peer.address[2], peer.address[3], peer.port, length, message);
  } else {
    printf("udpecho: listening on UDP %d\n", port);
    for (;;) {
      length = udp_recvfrom(socket, message, sizeof(message), &peer, 0x7fffffff);
      if (length > 0) udp_sendto(socket, message, length, &peer);
    }
  }
  udp_close(socket);
  exit();
}
