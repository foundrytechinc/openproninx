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
    if (i < 3 && *text++ != '.') return -1;
  }
  return *text ? -1 : 0;
}

int main(int argc, char **argv) {
  int listener, connection, length, port;
  char message[256];
  struct network_endpoint peer;
  if (argc != 3 && argc != 4) {
    printf("usage: tcpecho PORT [IPv4 MESSAGE]\n");
    exit();
  }
  port = number(argv[1]);
  if (port <= 0 || (argc == 4 && address(argv[2], peer.address) < 0)) {
    printf("tcpecho: invalid endpoint\n"); exit();
  }
  if (argc == 4) {
    connection = tcp_open(); peer.port = port;
    length = 0;
    while (argv[3][length] && length < (int)sizeof(message))
      message[length] = argv[3][length], length++;
    if (connection < 0 || tcp_connect(connection, &peer, 3000) < 0 ||
        tcp_send(connection, message, length) != length ||
        (length = tcp_recv(connection, message, sizeof(message), 3000)) < 0)
      printf("tcpecho: connection or reply failed\n");
    else
      printf("tcp reply: %.*s\n", length, message);
    if (connection >= 0) tcp_close(connection);
  } else {
    listener = tcp_open();
    if (listener < 0 || tcp_bind(listener, port) < 0 || tcp_listen(listener) < 0) {
      printf("tcpecho: cannot listen on TCP %d\n", port); exit();
    }
    printf("tcpecho: listening on TCP %d\n", port);
    for (;;) {
      connection = tcp_accept(listener, 0x7fffffff);
      if (connection < 0) continue;
      while ((length = tcp_recv(connection, message, sizeof(message), 0x7fffffff)) > 0)
        if (tcp_send(connection, message, length) != length) break;
      tcp_close(connection);
    }
  }
  exit();
}
