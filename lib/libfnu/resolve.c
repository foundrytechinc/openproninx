// NAMES TO ADDRESSES
#include "inc/fnu.h"

#define HOSTS "/etc/hosts"
#define SERVERS_MAX 4
#define NAMELEN_MAX 253
#define PACKET 512
#define LINE 512
#define DNS_PORT 53
#define T_A 1
#define T_CNAME 5
#define C_IN 1

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

static int same(const char *a, const char *b) {
  while (*a != 0 && lower(*a) == lower(*b)) {
    a++;
    b++;
  }
  return *a == 0 && *b == 0;
}

static int blank(int c) { return c == ' ' || c == '\t'; }

static int hosts_line(char *line, const char *name, uint8 addr[4]) {
  char *p = line, *word, *cut;
  uint8 a[4];

  for (cut = line; *cut != 0 && *cut != '#'; cut++)
    ;
  *cut = 0;
  while (blank(*p))
    p++;
  for (word = p; *p != 0 && !blank(*p); p++)
    ;
  if (*p == 0)
    return 0;
  *p++ = 0;
  if (fnu_addr_parse(word, a) < 0)
    return 0;
  for (;;) {
    while (blank(*p))
      p++;
    if (*p == 0)
      return 0;
    for (word = p; *p != 0 && !blank(*p); p++)
      ;
    if (*p != 0)
      *p++ = 0;
    if (same(word, name)) {
      addr[0] = a[0];
      addr[1] = a[1];
      addr[2] = a[2];
      addr[3] = a[3];
      return 1;
    }
  }
}

static int from_hosts(const char *name, uint8 addr[4]) {
  char chunk[1024], line[LINE];
  struct fnu_segment seg = {chunk, sizeof(chunk)};
  int fd, used = 0, found = 0, i;
  long n;

  if ((fd = fnu_open(FNU_FD_CWD, HOSTS, FNU_OPEN_READ | FNU_OPEN_SYSFILE, 0)) < 0)
    return 0;
  while (!found && (n = fnu_read(fd, &seg, 1, FNU_IO_HERE)) > 0)
    for (i = 0; i < n && !found; i++) {
      if (chunk[i] != '\n') {
        if (used < LINE - 1)
          line[used++] = chunk[i];
        continue;
      }
      line[used] = 0;
      used = 0;
      found = hosts_line(line, name, addr);
    }
  if (!found && used > 0) {
    line[used] = 0;
    found = hosts_line(line, name, addr);
  }
  fnu_close(fd);
  return found;
}

static int servers(uint8 out[SERVERS_MAX][4]) {
  char text[128], word[16];
  uint64 len = sizeof(text);
  int n = 0, i = 0, w;

  if (fnu_sysctl("net.inet.dns.servers", text, &len, 0, 0) < 0)
    return 0;
  text[sizeof(text) - 1] = 0;
  while (text[i] != 0 && n < SERVERS_MAX) {
    while (text[i] == ' ')
      i++;
    for (w = 0; text[i] != 0 && text[i] != ' '; i++)
      if (w < (int)sizeof(word) - 1)
        word[w++] = text[i];
    word[w] = 0;
    if (w > 0 && fnu_addr_parse(word, out[n]) == 0)
      n++;
  }
  return n;
}

static int query(uint8 *q, const char *name, uint16 id) {
  int len = 12, label, i;

  for (i = 0; i < 12; i++)
    q[i] = 0;
  q[0] = (uint8)(id >> 8);
  q[1] = (uint8)id;
  q[2] = 0x01; // RD
  q[5] = 1;    // one question
  while (*name != 0) {
    for (label = 0; name[label] != 0 && name[label] != '.'; label++)
      ;
    if (label == 0 || label > 63)
      return -FNU_ERR_INVALID;
    q[len++] = (uint8)label;
    for (i = 0; i < label; i++)
      q[len++] = (uint8)name[i];
    name += label;
    if (*name == '.')
      name++;
  }
  q[len++] = 0;
  q[len++] = 0;
  q[len++] = T_A;
  q[len++] = 0;
  q[len++] = C_IN;
  return len;
}

static int skip_name(const uint8 *p, int at, int len) {
  while (at < len) {
    if (p[at] == 0)
      return at + 1;
    if ((p[at] & 0xc0) == 0xc0)
      return at + 2 <= len ? at + 2 : -1;
    at += p[at] + 1;
  }
  return -1;
}

static uint16 be16(const uint8 *p) { return (uint16)((p[0] << 8) | p[1]); }

static int answer(const uint8 *p, int len, uint16 id, uint8 addr[4]) {
  int at, an, i;

  if (len < 12 || be16(p) != id || !(p[2] & 0x80))
    return -1;
  if ((p[3] & 0x0f) == 3)
    return 0;
  if ((p[3] & 0x0f) != 0)
    return -2;
  if ((at = skip_name(p, 12, len)) < 0 || (at += 4) > len)
    return -1;
  an = be16(p + 6);
  for (i = 0; i < an; i++) {
    uint16 type, class, rdlen;

    if ((at = skip_name(p, at, len)) < 0 || at + 10 > len)
      return -1;
    type = be16(p + at);
    class = be16(p + at + 2);
    rdlen = be16(p + at + 8);
    at += 10;
    if (at + rdlen > len)
      return -1;
    if (type == T_A && class == C_IN && rdlen == 4) {
      addr[0] = p[at];
      addr[1] = p[at + 1];
      addr[2] = p[at + 2];
      addr[3] = p[at + 3];
      return 1;
    }
    at += rdlen;
  }
  return 0;
}

static int ask(int fd, const uint8 server[4], const uint8 *q, int qlen, uint16 id,
               int ms, uint8 addr[4]) {
  struct fnu_inetaddr to, from;
  struct fnu_segment seg;
  struct fnu_msg m;
  uint8 reply[PACKET];
  long n;
  int r, i;

  for (i = 0; i < (int)sizeof(to); i++)
    ((uint8 *)&to)[i] = 0;
  to.len = sizeof(to);
  to.family = FNU_NET_INET;
  to.port = (uint16)((DNS_PORT >> 8) | (DNS_PORT << 8));
  for (i = 0; i < 4; i++)
    to.addr[i] = server[i];
  seg.base = (void *)q;
  seg.len = (uint64)qlen;
  m.iov = &seg;
  m.iovcnt = 1;
  m.flags = 0;
  m.addr = &to;
  m.addrlen = sizeof(to);
  m.nfds = 0;
  m.fds = 0;
  if ((n = fnu_send(fd, &m, 0)) < 0)
    return (int)n;
  for (;;) {
    if ((r = fnu_waitread(fd, ms)) < 0)
      return r;
    seg.base = reply;
    seg.len = sizeof(reply);
    m.addr = &from;
    m.addrlen = sizeof(from);
    if ((n = fnu_recv(fd, &m, 0)) < 0)
      return (int)n;
    if (from.port != to.port || from.addr[0] != server[0] ||
        from.addr[1] != server[1] || from.addr[2] != server[2] ||
        from.addr[3] != server[3])
      continue;
    if ((r = answer(reply, (int)n, id, addr)) == -2)
      return -FNU_ERR_REFUSED;
    if (r >= 0)
      return r ? 0 : -FNU_ERR_NOTFOUND;
  }
}

int fnu_resolve(const char *name, uint8 addr[4], int ms) {
  uint8 list[SERVERS_MAX][4], q[PACKET];
  int nserv, qlen, fd, r = -FNU_ERR_NOTFOUND, round, i, each;
  uint16 id;

  for (i = 0; name[i] != 0; i++)
    if (i >= NAMELEN_MAX)
      return -FNU_ERR_NAMETOOLONG;
  if (i == 0 || ms <= 0)
    return -FNU_ERR_INVALID;
  if (fnu_addr_parse(name, addr) == 0 || from_hosts(name, addr))
    return 0;
  if ((nserv = servers(list)) == 0)
    return -FNU_ERR_NOTFOUND;
  if (fnu_random(&id, sizeof(id)) < 0)
    id = 0x5f5f;
  if ((qlen = query(q, name, id)) < 0)
    return qlen;
  if ((fd = fnu_socket(FNU_NET_INET, FNU_SOCKET_DATAGRAM, 0)) < 0)
    return fd;
  each = ms / (2 * nserv);
  if (each < 1)
    each = 1;
  for (round = 0; round < 2; round++)
    for (i = 0; i < nserv; i++) {
      r = ask(fd, list[i], q, qlen, id, each, addr);
      if (r == 0 || r == -FNU_ERR_NOTFOUND)
        goto done;
    }
done:
  fnu_close(fd);
  return r;
}
