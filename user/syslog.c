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
  struct network_endpoint server;
  char message[512];
  int socket, i, length = 0;
  if (argc != 3 || ipv4(argv[1], server.address) < 0) {
    printf("usage: syslog IPv4-server MESSAGE\n"); exit();
  }
  server.port = 514;
  /* RFC 3164-compatible user.notice message. */
  message[length++] = '<'; message[length++] = '1'; message[length++] = '3'; message[length++] = '>';
  for (i = 0; "PRONINX: "[i]; i++) message[length++] = "PRONINX: "[i];
  for (i = 0; argv[2][i] && length < (int)sizeof(message); i++) message[length++] = argv[2][i];
  socket = udp_open();
  if (socket < 0 || udp_sendto(socket, message, length, &server) != length)
    printf("syslog: send failed\n");
  else
    printf("syslog: sent %d bytes\n", length);
  if (socket >= 0) udp_close(socket);
  exit();
}
