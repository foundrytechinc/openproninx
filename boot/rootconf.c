#include "boot/ufs2read.h"

#define TABLE "/etc/filesystems"
#define LOADER_CONF "/boot/loader.conf"
#define LINE 256
#define CHUNK 512

void putstr(const char *s);

static const uint8 ufs_type[16] = {0xb6, 0x7c, 0x6e, 0x51, 0xcf, 0x6e,
                                   0xd6, 0x11, 0x8f, 0xf8, 0x00, 0x02,
                                   0x2d, 0x09, 0x71, 0x2b};

static struct ufs2rd fs;

static uint32 len(const char *s) {
  uint32 n = 0;

  while (s[n] != 0)
    n++;
  return n;
}

void env_init(struct bootenv *e, char *buf, uint32 size) {
  e->buf = buf;
  e->size = size;
  e->len = 0;
  if (size >= 1)
    buf[0] = 0;
}

// the entry for name, or 0
static char *env_find(struct bootenv *e, const char *name) {
  uint32 n = len(name), at = 0, i;

  while (at < e->len) {
    for (i = 0; i < n && e->buf[at + i] == name[i]; i++)
      ;
    if (i == n && e->buf[at + n] == '=')
      return e->buf + at;
    at += len(e->buf + at) + 1;
  }
  return 0;
}

const char *env_get(struct bootenv *e, const char *name) {
  char *p = env_find(e, name);

  return p != 0 ? p + len(name) + 1 : 0;
}

// a later value takes the place of the earlier
int env_set(struct bootenv *e, const char *name, const char *value) {
  uint32 n = len(name), v = len(value), i, gone;
  char *old = env_find(e, name);

  if (old != 0) {
    gone = len(old) + 1;
    for (i = (uint32)(old - e->buf); i + gone <= e->len; i++)
      e->buf[i] = e->buf[i + gone];
    e->len -= gone;
    e->buf[e->len] = 0;
  }

  if (e->len + n + 1 + v + 2 > e->size)
    return -1;
  for (i = 0; i < n; i++)
    e->buf[e->len++] = name[i];
  e->buf[e->len++] = '=';
  for (i = 0; i < v; i++)
    e->buf[e->len++] = value[i];
  e->buf[e->len++] = 0;
  e->buf[e->len] = 0;
  return 0;
}

int env_true(struct bootenv *e, const char *name) {
  const char *v = env_get(e, name);

  return v != 0 && (v[0] == 'Y' || v[0] == 'y' || v[0] == '1');
}

void guid_text(const uint8 raw[16], char out[37]) {
  static const char hex[] = "0123456789abcdef";
  static const uint8 order[16] = {3, 2, 1, 0, 5, 4, 7, 6,
                                  8, 9, 10, 11, 12, 13, 14, 15};
  int i, n = 0;

  for (i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      out[n++] = '-';
    out[n++] = hex[raw[order[i]] >> 4];
    out[n++] = hex[raw[order[i]] & 15];
  }
  out[n] = 0;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static int field(const char *line, const char *key, char *out, uint32 size) {
  const char *p = line;
  uint32 n = len(key), i, k;

  while (*p != 0) {
    while (is_space(*p))
      p++;
    for (i = 0; i < n && p[i] == key[i]; i++)
      ;
    if (i == n && p[n] == '=') {
      p += n + 1;
      for (k = 0; *p != 0 && !is_space(*p) && k < size - 1; k++)
        out[k] = *p++;
      out[k] = 0;
      return k > 0;
    }
    while (*p != 0 && !is_space(*p))
      p++;
  }
  return 0;
}

static int mountfrom(const char *type, const char *from, char *out,
                     uint32 size) {
  uint32 t = len(type), o = 0, i;

  while (*from != 0) {
    if (o != 0) {
      if (o + 1 >= size)
        return -1;
      out[o++] = ';';
    }
    if (o + t + 1 >= size)
      return -1;
    for (i = 0; i < t; i++)
      out[o++] = type[i];
    out[o++] = ':';
    for (; *from != 0 && *from != ';'; from++) {
      if (o + 1 >= size)
        return -1;
      out[o++] = *from;
    }
    if (*from == ';')
      from++;
  }
  out[o] = 0;
  return o > 0 ? 0 : -1;
}

static int root_line(const char *line, struct bootenv *e) {
  char on[4], from[LINE], type[16], mode[64], value[LINE + 64];
  const char *p = line;

  while (is_space(*p))
    p++;
  if (*p == 0 || *p == '#' || !field(line, "on", on, sizeof(on)) ||
      on[0] != '/' || on[1] != 0 || !field(line, "from", from, sizeof(from)))
    return -1;
  if (mountfrom(field(line, "type", type, sizeof(type)) ? type : "auto", from,
                value, sizeof(value)) < 0 ||
      env_set(e, "vfs.root.mountfrom", value) < 0)
    return -1;
  if (field(line, "mode", mode, sizeof(mode)) &&
      env_set(e, "vfs.root.mountfrom.options", mode) < 0)
    return -1;
  putstr("root: ");
  putstr(value);
  putstr("\n");
  return 0;
}

static int conf_line(const char *line, struct bootenv *e) {
  char name[64], value[LINE];
  const char *p = line;
  uint32 n = 0, v = 0;

  while (is_space(*p))
    p++;
  if (*p == 0 || *p == '#')
    return -1;
  while (*p != 0 && *p != '=' && !is_space(*p) && n < sizeof(name) - 1)
    name[n++] = *p++;
  name[n] = 0;
  while (is_space(*p))
    p++;
  if (n == 0 || *p++ != '=')
    return -1;
  while (is_space(*p))
    p++;
  if (*p == '"') {
    for (p++; *p != 0 && *p != '"' && v < sizeof(value) - 1;)
      value[v++] = *p++;
    if (*p != '"')
      return -1;
  } else
    while (*p != 0 && *p != '#' && !is_space(*p) && v < sizeof(value) - 1)
      value[v++] = *p++;
  value[v] = 0;
  env_set(e, name, value);
  return -1;
}

static int scan_file(uint32 ino, uint64 size, struct bootenv *e,
                     int (*each)(const char *, struct bootenv *)) {
  static char chunk[CHUNK];
  char line[LINE];
  uint64 off = 0;
  uint32 n = 0, i, got;
  int skip = 0;

  while (off < size) {
    got = size - off > CHUNK ? CHUNK : (uint32)(size - off);
    if (ufs2rd_read(&fs, ino, off, chunk, got) < 0)
      return -1;
    off += got;
    for (i = 0; i < got; i++) {
      if (chunk[i] != '\n') {
        if (n < LINE - 1)
          line[n++] = chunk[i];
        else
          skip = 1;
        continue;
      }
      line[n] = 0;
      if (!skip && each(line, e) == 0)
        return 0;
      n = 0;
      skip = 0;
    }
  }
  line[n] = 0;
  return !skip && n > 0 ? each(line, e) : -1;
}

int root_env(const struct bootdisk *d, struct bootenv *e) {
  struct gpt_part part;
  uint32 ino;
  uint64 size;

  if (gpt_scan(d, ufs_type, "fnu-root", &part) != 0 &&
      gpt_scan(d, ufs_type, 0, &part) != 0)
    return -1;
  fs.disk = d;
  fs.start = part.start;
  if (ufs2rd_mount(&fs) < 0)
    return -1;
  if (ufs2rd_lookup(&fs, LOADER_CONF, &ino, &size) == 0)
    scan_file(ino, size, e, conf_line);
  if (ufs2rd_lookup(&fs, TABLE, &ino, &size) < 0)
    return -1;
  return scan_file(ino, size, e, root_line);
}
