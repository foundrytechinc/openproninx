#include <err.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fnu/sys.h>

#define ECHO_REPLY 0
#define ECHO_REQUEST 8
#define DATA 56
#define HDR 8

static long sent, got;
static double tmin, tmax, tsum, tsum2;
static volatile sig_atomic_t stop;
static char host_text[16];
static const char *host;

static void interrupt(int sig) {
  (void)sig;
  stop = 1;
}

static double now_ms(void) {
  struct fnu_timespec t;

  fnu_gettime(&t, FNU_CLOCK_UPTIME);
  return (double)t.sec * 1000.0 + (double)t.nsec / 1e6;
}

static uint16 checksum(const uint8 *p, int n) {
  uint32 sum = 0;
  int i;

  for (i = 0; i + 1 < n; i += 2)
    sum += (uint32)(p[i] | p[i + 1] << 8);
  if (n & 1)
    sum += p[n - 1];
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return (uint16)~sum;
}

static void send_echo(int fd, const uint8 to[4], uint16 id, uint16 seq) {
  struct fnu_inetaddr a;
  struct fnu_segment seg;
  struct fnu_msg m;
  uint8 pkt[HDR + DATA];
  double t = now_ms();
  uint16 sum;
  int i;

  memset(pkt, 0, sizeof(pkt));
  pkt[0] = ECHO_REQUEST;
  pkt[4] = (uint8)(id >> 8);
  pkt[5] = (uint8)id;
  pkt[6] = (uint8)(seq >> 8);
  pkt[7] = (uint8)seq;
  memcpy(pkt + HDR, &t, sizeof(t));
  for (i = (int)sizeof(t); i < DATA; i++)
    pkt[HDR + i] = (uint8)i;
  sum = checksum(pkt, sizeof(pkt));
  pkt[2] = (uint8)sum;
  pkt[3] = (uint8)(sum >> 8);
  memset(&a, 0, sizeof(a));
  a.len = sizeof(a);
  a.family = FNU_NET_INET;
  memcpy(a.addr, to, 4);
  seg.base = pkt;
  seg.len = sizeof(pkt);
  memset(&m, 0, sizeof(m));
  m.iov = &seg;
  m.iovcnt = 1;
  m.addr = &a;
  m.addrlen = sizeof(a);
  if (fnu_send(fd, &m, 0) < 0)
    warnx("sendto %s failed", host_text);
  else
    sent++;
}

static void take_replies(int fd, const uint8 from[4], uint16 id, double ms) {
  double end = now_ms() + ms, left, t;
  struct fnu_inetaddr a;
  struct fnu_segment seg;
  struct fnu_msg m;
  uint8 pkt[1500];
  long n;
  int hl;

  while (!stop && (left = end - now_ms()) > 0) {
    if (fnu_waitread(fd, (int)left + 1) < 0)
      return;
    seg.base = pkt;
    seg.len = sizeof(pkt);
    memset(&m, 0, sizeof(m));
    m.iov = &seg;
    m.iovcnt = 1;
    m.addr = &a;
    m.addrlen = sizeof(a);
    if ((n = fnu_recv(fd, &m, 0)) <= 0)
      continue;
    hl = (pkt[0] & 0x0f) * 4;
    if (n < hl + HDR + (long)sizeof(double) || pkt[hl] != ECHO_REPLY ||
        memcmp(a.addr, from, 4) != 0 ||
        (uint16)(pkt[hl + 4] << 8 | pkt[hl + 5]) != id)
      continue;
    memcpy(&t, pkt + hl + HDR, sizeof(t));
    t = now_ms() - t;
    got++;
    tsum += t;
    tsum2 += t * t;
    if (got == 1 || t < tmin)
      tmin = t;
    if (t > tmax)
      tmax = t;
    printf("%ld bytes from %s: icmp_seq=%u ttl=%u time=%.3f ms\n", n - hl,
           host_text, (unsigned)(pkt[hl + 6] << 8 | pkt[hl + 7]), pkt[8], t);
    fflush(stdout);
  }
}

static void usage(void) {
  fprintf(stderr, "usage: ping [-c count] [-i wait] host\n");
  exit(1);
}

int main(int argc, char **argv) {
  struct fnu_ident id;
  uint8 to[4];
  long count = -1;
  double wait = 1000;
  int fd, ch, r;
  uint16 ident;

  fd = fnu_socket(FNU_NET_INET, FNU_SOCKET_RAW, FNU_PROTO_ICMP);
  if (fnu_ident_get(&id) < 0)
    err(1, "ident");
  if (fnu_ident_set(&id, FNU_IDENT_UID) < 0)
    errx(1, "cannot leave the root identity");
  if (fd < 0)
    errx(1, "raw socket: %s", fnu_errstr(fd));

  while ((ch = getopt(argc, argv, "c:i:")) != -1)
    switch (ch) {
    case 'c':
      if ((count = strtol(optarg, 0, 10)) < 1)
        errx(1, "bad count: %s", optarg);
      break;
    case 'i':
      if ((wait = strtod(optarg, 0) * 1000) < 100 && id.uid != 0)
        errx(1, "only root may wait less than 0.1 s");
      if (wait <= 0)
        errx(1, "bad wait: %s", optarg);
      break;
    default:
      usage();
    }
  if (argc - optind != 1)
    usage();
  host = argv[optind];
  if ((r = fnu_resolve(host, to, 5000)) < 0)
    errx(1, "%s: %s", host, fnu_errstr(r));
  snprintf(host_text, sizeof(host_text), "%u.%u.%u.%u", to[0], to[1], to[2], to[3]);
  if (pledge("stdio", 0) < 0)
    err(1, "pledge");
  signal(SIGINT, interrupt);
  ident = (uint16)getpid();
  printf("PING %s (%s): %d data bytes\n", host, host_text, DATA);
  fflush(stdout);
  while (!stop && (count < 0 || sent < count)) {
    send_echo(fd, to, ident, (uint16)sent);
    take_replies(fd, to, ident, count >= 0 && sent >= count ? 2000 : wait);
  }
  printf("\n--- %s ping statistics ---\n", host);
  printf("%ld packets transmitted, %ld packets received, %.1f%% packet loss\n", sent,
         got, sent ? (double)(sent - got) * 100 / (double)sent : 0.0);
  if (got > 0) {
    double avg = tsum / (double)got;

    printf("round-trip min/avg/max/std-dev = %.3f/%.3f/%.3f/%.3f ms\n", tmin, avg,
           tmax, sqrt(fmax(tsum2 / (double)got - avg * avg, 0)));
  }
  return got == 0 ? 2 : 0;
}
