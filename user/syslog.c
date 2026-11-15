#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <ctype.h>
#include <fnu/sys.h>
#include <pwd.h>
#include <err.h>

static ssize_t readln(FILE *fp, char **out, size_t *cap) {
  ssize_t n;

  if ((n = getline(out, cap, fp)) < 0)
    return -1;
  if (n > 0 && (*out)[n - 1] == '\n')
    (*out)[--n] = 0;
  return n;
}

#define SYSLOG_PORT 514
#define LINE 1024

struct code {
  const char *name;
  int value;
};

static const struct code facilities[] = {
    {"kern", 0},   {"user", 1},   {"mail", 2},    {"daemon", 3}, {"auth", 4},
    {"syslog", 5}, {"lpr", 6},    {"news", 7},    {"uucp", 8},   {"cron", 9},
    {"authpriv", 10}, {"ftp", 11}, {"local0", 16}, {"local1", 17},
    {"local2", 18}, {"local3", 19}, {"local4", 20}, {"local5", 21},
    {"local6", 22}, {"local7", 23}, {0, 0}};

static const struct code severities[] = {
    {"emerg", 0}, {"alert", 1},  {"crit", 2},   {"err", 3},
    {"warning", 4}, {"notice", 5}, {"info", 6}, {"debug", 7}, {0, 0}};

static int decode(const char *name, const struct code *table) {
  int i;

  if (isdigit((unsigned char)*name))
    return atoi(name);
  for (i = 0; table[i].name != 0; i++)
    if (strcmp(table[i].name, (char *)name) == 0)
      return table[i].value;
  return -1;
}

static int pencode(char *s) {
  char *dot = strchr(s, '.');
  int fac = 1, lev;

  if (dot != 0) {
    *dot = 0;
    if ((fac = decode(s, facilities)) < 0) {
      dprintf(2, "syslog: %s: unknown facility name\n", s);
      exit(1);
    }
    s = dot + 1;
  }
  if ((lev = decode(s, severities)) < 0) {
    dprintf(2, "syslog: %s: unknown priority name\n", s);
    exit(1);
  }
  return fac * 8 + lev;
}

static int sock = -1;
static struct sockaddr_in server;
static const char *tag;
static int pri = 1 * 8 + 5;
static int logpid, tostderr, tocons;

static void console_line(const char *line) {
  int fd = open("/dev/console", O_WRONLY);

  if (fd < 0)
    return;
  dprintf(fd, "%s\n", line);
  close(fd);
}

static void send_line(const char *message) {
  char line[LINE];
  int n;

  n = snprintf(line, sizeof(line), "<%d>%s", pri, tag != 0 ? tag : "");
  if (logpid && n < (int)sizeof(line))
    n += snprintf(line + n, sizeof(line) - n, "[%d]", getpid());
  if (n < (int)sizeof(line))
    n += snprintf(line + n, sizeof(line) - n, "%s%s",
                  tag != 0 || logpid ? ": " : "", message);
  if (n > (int)sizeof(line) - 1)
    n = (int)sizeof(line) - 1;
  if (tostderr)
    dprintf(2, "%s\n", line);
  if (sock < 0 || sendto(sock, line, n, 0, (struct sockaddr *)&server, sizeof(server)) != n) {
    if (tocons) {
      console_line(line);
      return;
    }
    warn("send");
    exit(1);
  }
}

static void usage(void) {
  dprintf(2, "usage: syslog -h host [-cis] [-f file] [-p pri] [-t tag] "
             "[message ...]\n");
  exit(1);
}

int main(int argc, char **argv) {
  struct passwd *account;
  static char *line;
  static size_t cap;
  char message[LINE];
  const char *host = 0, *from = 0;
  struct in_addr addr;
  int i = 1, n = 0, k;

  for (; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
    char *p = argv[i] + 1;

    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (; *p != 0; p++) {
      if (*p == 'i')
        logpid = 1;
      else if (*p == 's')
        tostderr = 1;
      else if (*p == 'c')
        tocons = 1;
      else if (*p == 'h' || *p == 'f' || *p == 'p' || *p == 't') {
        char *v = p[1] != 0 ? p + 1 : (i + 1 < argc ? argv[++i] : 0);

        if (v == 0)
          usage();
        if (*p == 'h')
          host = v;
        else if (*p == 'f')
          from = v;
        else if (*p == 't')
          tag = v;
        else
          pri = pencode(v);
        p += strlen(p) - 1;
      } else
        usage();
    }
  }
  if (host == 0)
    usage();
  if (!inet_aton(host, &addr)) {
    dprintf(2, "syslog: %s is not an address\n", host);
    exit(1);
  }
  if (tag == 0 && (account = getpwuid(getuid())) != 0)
    tag = account->pw_name;

  memset(&server, 0, sizeof(server));
  server.sin_len = sizeof(server);
  server.sin_family = AF_INET;
  server.sin_port = htons(SYSLOG_PORT);
  server.sin_addr.s_addr = addr.s_addr;
  if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0 && !tocons) {
    dprintf(2, "syslog: socket: no descriptor\n");
    exit(1);
  }

  if (from != 0) {
    FILE *in;
    int fd = strcmp(from, "-") == 0 ? 0 : open(from, O_RDONLY);

    if (fd < 0) {
      warn("%s", from);
      exit(1);
    }
    in = fd == 0 ? stdin : fdopen(fd, "r");
    while (readln(in, &line, &cap) >= 0)
      send_line(line);
    if (fd > 0)
      close(fd);
    close(sock);
    exit(0);
  }
  if (i >= argc) {
    FILE *in;

    in = stdin;
    while (readln(in, &line, &cap) >= 0)
      send_line(line);
    close(sock);
    exit(0);
  }
  for (; i < argc; i++) {
    if (n > 0 && n < (int)sizeof(message) - 1)
      message[n++] = ' ';
    for (k = 0; argv[i][k] != 0 && n < (int)sizeof(message) - 1; k++)
      message[n++] = argv[i][k];
  }
  message[n] = 0;
  send_line(message);
  close(sock);
  exit(0);
}
