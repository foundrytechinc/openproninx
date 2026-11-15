#include <fnu/fstab.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

int fstab_open(struct fstab *table) {
  if (table == 0)
    return -1;
  table->len = 0;
  table->pos = 0;
  if ((table->fd = open(FSTAB_PATH, O_RDONLY)) < 0)
    return -1;
  return 0;
}

void fstab_close(struct fstab *table) {
  if (table != 0 && table->fd >= 0) {
    close(table->fd);
    table->fd = -1;
  }
}

static int readline(struct fstab *table, char *out, int size) {
  int n = 0;
  char c;

  for (;;) {
    if (table->pos == table->len) {
      table->len = (int)read(table->fd, table->buf, sizeof(table->buf));
      table->pos = 0;
      if (table->len <= 0)
        break;
    }
    c = table->buf[table->pos++];
    if (c == '\n')
      break;
    if (n < size - 1)
      out[n++] = c;
  }
  out[n] = 0;
  return (n == 0 && table->len <= 0) ? -1 : n;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static int field(const char *line, const char *name, char *out, int size) {
  const char *p = line;
  int n = (int)strlen(name);
  int k;

  while (*p != 0) {
    while (is_space(*p))
      p++;
    if (strncmp(p, name, (size_t)n) == 0 && p[n] == '=') {
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

int fstab_next(struct fstab *table, struct fstab_entry *entry) {
  char line[FSTAB_LINE];
  char value[FNU_MNT_NAMELEN];
  const char *p;

  if (table == 0 || entry == 0 || table->fd < 0)
    return -1;

  while (readline(table, line, sizeof(line)) >= 0) {
    for (p = line; is_space(*p); p++)
      ;
    if (*p == 0 || *p == '#')
      continue;

    memset(entry, 0, sizeof(*entry));
    if (!field(line, "from", entry->request.from, FNU_MNT_NAMELEN))
      continue;
    if (field(line, "type", entry->request.type, FNU_MNT_TYPELEN) &&
        strcmp(entry->request.type, "auto") == 0)
      entry->request.type[0] = 0;
    if (!field(line, "on", entry->request.on, FNU_MNT_NAMELEN) &&
        strcmp(entry->request.type, "swap") != 0)
      continue;
    if (field(line, "mode", value, sizeof(value))) {
      char *p = value;

      while (*p != 0) {
        char *word = p;

        while (*p != 0 && *p != ',')
          p++;
        if (*p == ',')
          *p++ = 0;
        if (strcmp(word, "ro") == 0)
          entry->request.flags |= FNU_MNT_RDONLY;
        else if (strcmp(word, "nosuid") == 0)
          entry->request.flags |= FNU_MNT_NOSUID;
        else if (strcmp(word, "suid") == 0)
          entry->request.flags &= ~FNU_MNT_NOSUID;
      }
    }

    entry->when = FSTAB_AT_BOOT;
    if (field(line, "at", value, sizeof(value))) {
      if (strcmp(value, "plug") == 0)
        entry->when = FSTAB_AT_PLUG;
      else if (strcmp(value, "manual") == 0)
        entry->when = FSTAB_AT_MANUAL;
    }
    return 0;
  }
  return -1;
}

// device by any name: the entry's name resolves to it
static int names_device(const char *from, dev_t dev) {
  struct stat st;

  return stat(from, &st) == 0 && S_ISBLK(st.st_mode) && st.st_rdev == dev;
}

int fstab_by_mountpoint(const char *on, struct fstab_entry *entry) {
  struct fstab table;
  struct fstab_entry e;
  int found = -1;

  if (on == 0 || fstab_open(&table) < 0)
    return -1;
  while (fstab_next(&table, &e) == 0)
    if (strcmp(e.request.on, on) == 0) {
      *entry = e;
      found = 0;
      break;
    }
  fstab_close(&table);
  return found;
}

int fstab_by_device(dev_t dev, struct fstab_entry *entry) {
  struct fstab table;
  struct fstab_entry e;
  int found = -1;

  if (fstab_open(&table) < 0)
    return -1;
  while (fstab_next(&table, &e) == 0)
    if (names_device(e.request.from, dev)) {
      *entry = e;
      found = 0;
      break;
    }
  fstab_close(&table);
  return found;
}
