#include "inc/string.h"
#include "user.h"

char *fmtname(char *path) {
  static char buf[NAMEBUFSZ];
  char *p;

  // Find first character after last slash.
  for (p = path + strlen(path); p >= path && *p != '/'; p--) {
    ;
  }
  p++;

  safestrcpy(buf, p, sizeof(buf));
  return buf;
}

static int is_dot_entry(const char *name) {
  return (name[0] == '.' && name[1] == '\0') ||
         (name[0] == '.' && name[1] == '.' && name[2] == '\0');
}

#define LS_COLUMNS 4
#define LS_CELL_WIDTH 20

static void print_entry(const char *name, const struct stat *st) {
  int i, used = 0;
  int length = strlen(name);
  int directory = st->type == T_DIR;
  int shortened = length + directory > LS_CELL_WIDTH;
  int visible = shortened ? LS_CELL_WIDTH - 3 - directory : length;

  dprintf(1, directory ? "\033[1;34m" : "\033[1;32m");
  for (i = 0; i < visible; i++) {
    dprintf(1, "%c", name[i]);
    used++;
  }
  if (shortened) {
    dprintf(1, "...");
    used += 3;
  }
  if (directory) {
    dprintf(1, "/");
    used++;
  }
  dprintf(1, "\033[0m");
  while (used++ < LS_CELL_WIDTH)
    dprintf(1, " ");
}

void ls(char *path) {
  char buf[512], entries[512], *p;
  int fd, n, entry_offset, columns;
  struct stat st;

  if ((fd = open(path, 0)) < 0) {
    dprintf(2, "ls: cannot open %s\n", path);
    return;
  }

  if (stat(fd, &st) < 0) {
    dprintf(2, "ls: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch (st.type) {
  case T_FILE:
    print_entry(fmtname(path), &st);
    dprintf(1, "\n");
    break;

  case T_DIR:
    if (strlen(path) + 1 + MAXNAMLEN + 1 > sizeof(buf)) {
      dprintf(1, "ls: path too long\n");
      break;
    }
    strncpy(buf, path, sizeof(buf));
    p = buf + strlen(buf);
    if (p == buf || p[-1] != '/')
      *p++ = '/';
    dprintf(1, "\033[1m%s\033[0m\n", path);
    columns = 0;
    while ((n = getdents(fd, (struct linux_dirent64 *)entries,
                         sizeof(entries))) > 0) {
      for (entry_offset = 0; entry_offset < n;) {
        struct linux_dirent64 *entry =
            (struct linux_dirent64 *)(entries + entry_offset);
        int name_length;

        if (entry->d_reclen < 20 || entry->d_reclen > n - entry_offset)
          break;
        entry_offset += entry->d_reclen;
        if (entry->d_ino == 0 || is_dot_entry(entry->d_name))
          continue;
        name_length = strlen(entry->d_name);
        if (name_length > MAXNAMLEN ||
            (uint)(p - buf) + name_length + 1 > sizeof(buf)) {
          dprintf(2, "ls: name too long\n");
          continue;
        }
        memmove(p, entry->d_name, name_length + 1);
        if (stat_path(buf, &st) < 0) {
          dprintf(2, "ls: cannot stat %s\n", buf);
          continue;
        }
        print_entry(fmtname(buf), &st);
        if (++columns == LS_COLUMNS) {
          dprintf(1, "\n");
          columns = 0;
        }
      }
    }
    if (columns != 0)
      dprintf(1, "\n");
    break;
  }
  close(fd);
}

int main(int argc, char *argv[]) {
  int i;

  if (argc < 2) {
    ls(".");
    exit();
  }
  for (i = 1; i < argc; i++)
    ls(argv[i]);
  exit();
}
