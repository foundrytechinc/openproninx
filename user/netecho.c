
#include <arpa/inet.h>
#include <err.h>
#include <fnu/sys.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int udp;

static void usage(void) {
  dprintf(2, "usage: netecho [-u] port [address message]\n");
  exit(1);
}

static int port_of(const char *text) {
  char *end;
  long v = strtol(text, &end, 10);

  return *text == 0 || *end != 0 || v <= 0 || v > 65535 ? -1 : (int)v;
}

static void address(struct sockaddr_in *sa, uint32_t addr, int port) {
  memset(sa, 0, sizeof(*sa));
  sa->sin_len = sizeof(*sa);
  sa->sin_family = AF_INET;
  sa->sin_port = htons(port);
  sa->sin_addr.s_addr = addr;
}

static int client(const char *host, int port, const char *text) {
  struct sockaddr_in to, from;
  socklen_t fromlen = sizeof(from);
  struct in_addr addr;
  char reply[256];
  int s, n, len = (int)strlen(text);

  if (!inet_aton(host, &addr))
    errx(1, "%s is not an address", host);
  address(&to, addr.s_addr, port);
  if ((s = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0)) < 0)
    err(1, "socket");
  if (!udp && connect(s, (struct sockaddr *)&to, sizeof(to)) < 0)
    err(1, "%s:%d", host, port);
  if (sendto(s, text, (size_t)len, 0, udp ? (struct sockaddr *)&to : 0,
             udp ? sizeof(to) : 0) != len)
    err(1, "send");
  if (fnu_waitread(s, 3000) < 0 ||
      (n = (int)recvfrom(s, reply, sizeof(reply), 0, (struct sockaddr *)&from,
                         &fromlen)) <= 0)
    errx(1, "no reply");
  printf("reply: %.*s\n", n, reply);
  close(s);
  return 0;
}

static int server(int port) {
  struct sockaddr_in me, from;
  socklen_t fromlen;
  char buf[256];
  int s, c, n;

  address(&me, INADDR_ANY, port);
  if ((s = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0)) < 0 ||
      bind(s, (struct sockaddr *)&me, sizeof(me)) < 0 ||
      (!udp && listen(s, 5) < 0))
    err(1, "port %d", port);
  printf("netecho: listening on %s %d\n", udp ? "UDP" : "TCP", port);
  for (;;) {
    fromlen = sizeof(from);
    if (udp) {
      if ((n = (int)recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from,
                             &fromlen)) > 0)
        sendto(s, buf, (size_t)n, 0, (struct sockaddr *)&from, fromlen);
      continue;
    }
    if ((c = accept(s, (struct sockaddr *)&from, &fromlen)) < 0)
      continue;
    printf("netecho: %s:%d\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
    while ((n = (int)recv(c, buf, sizeof(buf), 0)) > 0)
      if (send(c, buf, (size_t)n, 0) != n)
        break;
    close(c);
  }
}

int main(int argc, char **argv) {
  int ch, port;

  while ((ch = getopt(argc, argv, "u")) != -1) {
    if (ch != 'u')
      usage();
    udp = 1;
  }
  argc -= optind;
  argv += optind;
  if ((argc != 1 && argc != 3) || (port = port_of(argv[0])) < 0)
    usage();
  exit(argc == 3 ? client(argv[1], port, argv[2]) : server(port));
}
