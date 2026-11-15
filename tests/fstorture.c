#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fnu/sys.h>
#include <err.h>

static int nodots(const struct dirent *d) {
  return strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0;
}

static void freelist(struct dirent **list, int n) {
  int i;

  for (i = 0; i < n; i++)
    free(list[i]);
  free(list);
}

static uint64_t BIG = 12 * 1024 * 1024;
#define MANY 300

static int bad;
static char path[256];

static int64_t lap(void) {
  static int64_t last;
  struct fnu_timespec t;
  int64_t d;

  fnu_gettime(&t, FNU_CLOCK_WALL);
  d = last ? t.sec - last : 0;
  last = t.sec;
  return d;
}

static void check(int ok, const char *what) {
  if (!ok) {
    printf("fstorture: %s: FAILED\n", what);
    bad++;
  }
}

static const char *at(const char *dir, const char *name) {
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  return path;
}

static char pattern(uint64_t i) {
  return (char)((i * 7 + (i >> 8) * 13 + (i >> 16)) & 0xff);
}

static int64_t size_of(const char *p) {
  struct stat st;
  int fd = open(p, O_RDONLY);
  int r;

  if (fd < 0)
    return -1;
  r = fstat(fd, &st);
  close(fd);
  return r < 0 ? -1 : (int64_t)st.st_size;
}

static uint nlink_of(const char *p) {
  struct stat st;
  int fd = open(p, O_RDONLY);
  int r;

  if (fd < 0)
    return 0;
  r = fstat(fd, &st);
  close(fd);
  return r < 0 ? 0 : st.st_nlink;
}

static void big_file(const char *dir) {
  static char buf[65536];
  const char *p = at(dir, "big");
  uint64_t off = 0;
  int fd, i, chunk;

  fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0666);
  check(fd >= 0, "create big");
  if (fd < 0)
    return;
  for (chunk = 1; off < BIG; chunk = (chunk * 3 + 977) % 60000 + 1) {
    uint64_t n = (uint64_t)chunk < BIG - off ? (uint64_t)chunk : BIG - off;
    for (i = 0; i < (int)n; i++)
      buf[i] = pattern(off + i);
    if (write(fd, buf, (int)n) != (int)n) {
      check(0, "write big");
      close(fd);
      return;
    }
    off += n;
  }
  close(fd);
  check(size_of(p) == (int64_t)BIG, "big size");

  fd = open(p, O_RDONLY);
  check(fd >= 0, "open big");
  if (fd < 0)
    return;
  off = 0;
  while (off < BIG) {
    int n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
      check(0, "read big");
      break;
    }
    for (i = 0; i < n; i++)
      if (buf[i] != pattern(off + i)) {
        printf("fstorture: big: byte %llu is %x not %x\n", off + i, buf[i] & 0xff,
               pattern(off + i) & 0xff);
        check(0, "big contents");
        close(fd);
        return;
      }
    off += n;
  }
  check(off == BIG, "big length read");
  for (i = 0; i < 40; i++) {
    uint64_t where = ((uint64_t)i * 7919 * 4093) % (BIG - 1000);
    int n;
    lseek(fd, (int64_t)where, SEEK_SET);
    n = read(fd, buf, 1000);
    check(n == 1000, "seek read");
    for (chunk = 0; chunk < n; chunk++)
      if (buf[chunk] != pattern(where + chunk)) {
        check(0, "seek contents");
        break;
      }
  }
  close(fd);
  check(unlink(p) == 0, "unlink big");
}

static void holes(const char *dir) {
  static char buf[8192];
  const char *p = at(dir, "holey");
  int fd, i, n, zeros = 1;

  fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0666);
  check(fd >= 0, "create holey");
  if (fd < 0)
    return;
  lseek(fd, 5 * 1024 * 1024 + 123, SEEK_SET);
  check(write(fd, "end", 3) == 3, "write past a hole");
  lseek(fd, 100000, SEEK_SET);
  check(write(fd, "mid", 3) == 3, "write into a hole");
  close(fd);
  check(size_of(p) == 5 * 1024 * 1024 + 126, "holey size");
  fd = open(p, O_RDONLY);
  lseek(fd, 100000 - 4000, SEEK_SET);
  n = read(fd, buf, 8000);
  check(n == 8000, "read around the middle");
  for (i = 0; i < n; i++)
    if (i != 4000 && i != 4001 && i != 4002 && buf[i] != 0)
      zeros = 0;
  check(zeros && memcmp(buf + 4000, "mid", 3) == 0, "the hole reads as zeros");
  lseek(fd, 5 * 1024 * 1024 + 123, SEEK_SET);
  n = read(fd, buf, 100);
  check(n == 3 && memcmp(buf, "end", 3) == 0, "the end after the hole");
  close(fd);
  check(unlink(p) == 0, "unlink holey");
}

// a write past four gigabytes
static void huge(const char *dir) {
  static char buf[4096];
  const char *p = at(dir, "huge");
  int64_t far = (5LL << 30) + 77;
  int fd, n;

  fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0666);
  check(fd >= 0, "create huge");
  if (fd < 0)
    return;
  if (lseek(fd, far, SEEK_SET) != far || write(fd, "far", 3) != 3) {
    printf("fstorture: huge: this filesystem stops short of 4 GB, skipped\n");
    close(fd);
    unlink(p);
    return;
  }
  close(fd);
  check(size_of(p) == far + 3, "huge size");
  fd = open(p, O_RDONLY);
  check(lseek(fd, far - 100, SEEK_SET) == far - 100, "seek past 4 GB");
  n = read(fd, buf, 200);
  check(n == 103 && memcmp(buf + 100, "far", 3) == 0, "read past 4 GB");
  for (n = 0; n < 100; n++)
    if (buf[n] != 0)
      check(0, "the hole before it reads as zeros");
  check(lseek(fd, 1LL << 32, SEEK_SET) == (1LL << 32) && read(fd, buf, 16) == 16 && buf[0] == 0,
        "a read inside the hole");
  close(fd);
  check(unlink(p) == 0, "unlink huge");
}

static void growing(const char *dir) {
  static char buf[4096];
  const char *p = at(dir, "grow");
  int fd, i, n, off = 0;

  fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0666);
  check(fd >= 0, "create grow");
  if (fd < 0)
    return;
  for (i = 0; i < 200; i++) {
    for (n = 0; n < 100 + i; n++)
      buf[n] = pattern(off + n);
    if (write(fd, buf, 100 + i) != 100 + i) {
      check(0, "write grow");
      break;
    }
    off += 100 + i;
  }
  close(fd);
  check(size_of(p) == off, "grow size");
  fd = open(p, O_RDONLY);
  i = 0;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    int k;
    for (k = 0; k < n; k++)
      if (buf[k] != pattern(i + k)) {
        check(0, "grow contents");
        break;
      }
    i += n;
  }
  check(i == off, "grow length read");
  close(fd);
  check(unlink(p) == 0, "unlink grow");
}

static void many_names(const char *dir) {
  char name[128], sub[256];
  struct dirent **names;
  int i, fd, count;

  snprintf(sub, sizeof(sub), "%s/many", dir);
  lap();
  check(mkdir(sub, 0777) == 0, "mkdir many");
  for (i = 0; i < MANY; i++) {
    snprintf(name, sizeof(name), "file-%d-with-a-longish-name", i);
    fd = open(at(sub, name), O_CREAT | O_WRONLY, 0666);
    if (fd < 0) {
      check(0, "create in many");
      return;
    }
    write(fd, name, strlen(name));
    close(fd);
  }
  printf("fstorture: many: created in %lld s\n", lap());
  count = scandir(sub, &names, nodots, 0);
  check(count == MANY, "many: readdir count");
  freelist(names, count);
  printf("fstorture: many: listed in %lld s\n", lap());
  for (i = 0; i < MANY; i += 2) {
    snprintf(name, sizeof(name), "file-%d-with-a-longish-name", i);
    check(unlink(at(sub, name)) == 0, "unlink in many");
  }
  printf("fstorture: many: half unlinked in %lld s\n", lap());
  count = scandir(sub, &names, nodots, 0);
  check(count == MANY / 2, "many: readdir after unlink");
  freelist(names, count);
  memset(name, 'L', 100);
  name[100] = 0;
  fd = open(at(sub, name), O_CREAT | O_WRONLY, 0666);
  check(fd >= 0, "create a long name into gaps");
  if (fd >= 0)
    close(fd);
  count = scandir(sub, &names, nodots, 0);
  check(count == MANY / 2 + 1, "many: readdir after the long name");
  freelist(names, count);
  check(unlink(at(sub, name)) == 0, "unlink the long name");
  for (i = 0; i < MANY; i += 4) {
    snprintf(name, sizeof(name), "again-%d", i);
    fd = open(at(sub, name), O_CREAT | O_WRONLY, 0666);
    check(fd >= 0, "create again in many");
    if (fd >= 0)
      close(fd);
  }
  count = scandir(sub, &names, nodots, 0);
  check(count == MANY / 2 + MANY / 4, "many: readdir after reuse");
  for (i = 0; i < count; i++)
    unlink(at(sub, names[i]->d_name));
  freelist(names, count);
  count = scandir(sub, &names, nodots, 0);
  check(count == 0, "many: empty again");
  freelist(names, count);
  check(rmdir(sub) == 0, "rmdir many");
}

static void names(const char *dir) {
  char a[256], b[256], c[256], target[300];
  int fd, n, dirlinks;

  snprintf(a, sizeof(a), "%s/a.txt", dir);
  snprintf(b, sizeof(b), "%s/b.txt", dir);
  snprintf(c, sizeof(c), "%s/d1", dir);
  fd = open(a, O_CREAT | O_WRONLY | O_TRUNC, 0666);
  check(fd >= 0, "create a");
  write(fd, "one\n", 4);
  close(fd);
  dirlinks = nlink_of(dir) >= 2;
  if (link(a, b) == 0) {
    check(nlink_of(a) == 2 && nlink_of(b) == 2, "nlink 2");
    check(unlink(a) == 0, "unlink a");
    check(nlink_of(b) == 1, "nlink 1");
    check(open(a, O_RDONLY) < 0, "a is gone");
  } else {
    printf("fstorture: hard links are not offered here, skipped\n");
    check(rename(a, b) == 0, "rename a to b");
  }
  check(rename(b, a) == 0, "rename");
  check(size_of(a) == 4 && open(b, O_RDONLY) < 0, "renamed");

  check(mkdir(c, 0777) == 0, "mkdir d1");
  check(!dirlinks || nlink_of(dir) >= 3, "parent nlink after mkdir");
  snprintf(b, sizeof(b), "%s/d1/inner", dir);
  fd = open(b, O_CREAT | O_WRONLY, 0666);
  check(fd >= 0, "create inner");
  close(fd);
  check(unlink(c) < 0, "unlink refuses a directory");
  check(rmdir(c) < 0 && errno == ENOTEMPTY, "rmdir refuses a full directory");
  check(unlink(b) == 0, "unlink inner");
  check(rmdir(c) == 0, "rmdir d1");

  {
    char e1[256], e2[256];
    uint before = nlink_of(dir);

    snprintf(e1, sizeof(e1), "%s/e1", dir);
    snprintf(e2, sizeof(e2), "%s/e2", dir);
    check(mkdir(e1, 0777) == 0 && mkdir(e2, 0777) == 0, "mkdir e1 e2");
    check(!dirlinks || nlink_of(dir) == before + 2,
          "parent nlink after two mkdirs");
    check(rename(e1, e2) == 0, "rename a directory over an empty one");
    check(open(e1, O_RDONLY) < 0 && (!dirlinks || nlink_of(e2) == 2),
          "e1 became e2");
    check(!dirlinks || nlink_of(dir) == before + 1,
          "parent nlink after the replacement");
    check(rmdir(e2) == 0, "rmdir e2");
    check(!dirlinks || nlink_of(dir) == before,
          "parent nlink back where it was");
    fd = open(e1, O_CREAT | O_WRONLY, 0666);
    write(fd, "xx", 2);
    close(fd);
    fd = open(e2, O_CREAT | O_WRONLY, 0666);
    write(fd, "yyy", 3);
    close(fd);
    check(rename(e1, e2) == 0 && size_of(e2) == 2 && open(e1, O_RDONLY) < 0,
          "rename a file over a file");
    check(unlink(e2) == 0, "unlink e2");
  }

  snprintf(b, sizeof(b), "%s/short.lnk", dir);
  if (symlink("a.txt", b) == 0) {
    n = readlink(b, target, sizeof(target));
    check(n == 5 && memcmp(target, "a.txt", 5) == 0, "short readlink");
    fd = open(b, O_RDONLY);
    check(fd >= 0 && read(fd, target, 10) == 4,
          "read through the short symlink");
    if (fd >= 0)
      close(fd);
    check(unlink(b) == 0, "unlink short symlink");
    snprintf(b, sizeof(b), "%s/long.lnk", dir);
    memset(target, 'x', 200);
    target[200] = 0;
    check(symlink(target, b) == 0, "long symlink");
    n = readlink(b, c, sizeof(c));
    check(n == 200 && memcmp(target, c, 200) == 0, "long readlink");
    check(unlink(b) == 0, "unlink long symlink");
  } else {
    printf("fstorture: symbolic links are not offered here, skipped\n");
    check(open(b, O_RDONLY) < 0, "a refused symlink leaves no entry");
  }

  {
    char tb[256];
    static char rb[70000];
    int64_t before;
    snprintf(tb, sizeof(tb), "%s/big", dir);
    if ((before = size_of(tb)) > 5000000) {
      if (truncate(tb, 5000000) == 0) {
        check(size_of(tb) == 5000000, "truncate to 5000000");
        fd = open(tb, O_RDONLY);
        lseek(fd, 5000000 - 70000, SEEK_SET);
        n = read(fd, rb, sizeof(rb));
        check(n == 70000, "read up to the cut");
        for (n = 0; n < 70000; n++)
          if (rb[n] != pattern(5000000 - 70000 + n)) {
            check(0, "contents before the cut");
            break;
          }
        close(fd);
        check(truncate(tb, 4321) == 0 && size_of(tb) == 4321, "truncate to 4321");
        check(truncate(tb, 9000) == 0 && size_of(tb) == 9000, "truncate longer");
        fd = open(tb, O_RDONLY);
        lseek(fd, 4000, SEEK_SET);
        n = read(fd, rb, 5000);
        check(n == 5000, "read across the extension");
        for (n = 321; n < 5000; n++)
          if (rb[n] != 0) {
            check(0, "the extension reads as zeros");
            break;
          }
        for (n = 0; n < 321; n++)
          if (rb[n] != pattern(4000 + n)) {
            check(0, "what was kept before the extension");
            break;
          }
        close(fd);
      } else {
        printf("fstorture: truncate to a length is not offered here, skipped\n");
      }
    }
  }

  check(truncate(a, 0) == 0, "truncate");
  check(size_of(a) == 0, "truncated to nothing");
  fd = open(a, O_WRONLY | O_TRUNC);
  check(fd >= 0, "O_TRUNC");
  close(fd);
}

static int64_t room(const char *dir) {
  struct fnu_fsstat t[32];
  struct stat st;
  int n, i;

  if (stat(dir, &st) < 0 || (n = fnu_fsstat(t, 32)) < 0)
    return -1;
  for (i = 0; i < n && i < 32; i++)
    if (t[i].dev == (uint32_t)st.st_dev)
      return t[i].bavail * (int64_t)t[i].bsize;
  return -1;
}

static int64_t put(const char *p, int64_t want) {
  static char buf[65536];
  int64_t done;
  ssize_t n;
  int fd;

  if ((fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0666)) < 0)
    return -1;
  memset(buf, 0x5a, sizeof(buf));
  for (done = 0; want < 0 || done < want; done += n)
    if ((n = write(fd, buf, want >= 0 && want - done < (int64_t)sizeof(buf)
                                ? (size_t)(want - done)
                                : sizeof(buf))) <= 0)
      break;
  close(fd);
  return done;
}

// full volume: what a removed file held is there again at once
static void reuse(const char *dir) {
  char s[256], full[256];
  int64_t free = room(dir), small = 256 * 1024;

  if (free < 0 || free > 256 * 1024 * 1024) {
    printf("fstorture: reuse: volume too big to fill, skipped\n");
    return;
  }
  snprintf(s, sizeof(s), "%s/reuse-small", dir);
  snprintf(full, sizeof(full), "%s/reuse-full", dir);
  check(put(s, small) == small, "write the small file");
  put(full, -1);
  check(unlink(s) == 0, "unlink the small file");
  check(put(s, small) == small, "its room again at once");
  check(unlink(s) == 0 && unlink(full) == 0, "unlink reuse");
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "/tmp/fstorture";
  char sub[256];

  if (argc > 2 && atoi(argv[2]) > 0)
    BIG = (uint64_t)atoi(argv[2]) * 1024 * 1024;

  snprintf(sub, sizeof(sub), "%s/torture", dir);
  if (mkdir(sub, 0777) < 0 && size_of(sub) < 0) {
    warn("%s", sub);
    exit(1);
  }
  unlink(at(sub, "a.txt"));
  unlink(at(sub, "short.lnk"));
  printf("fstorture: big file\n");
  big_file(sub);
  printf("fstorture: holes\n");
  holes(sub);
  printf("fstorture: huge\n");
  huge(sub);
  printf("fstorture: growing\n");
  growing(sub);
  printf("fstorture: many names\n");
  many_names(sub);
  printf("fstorture: names\n");
  names(sub);
  printf("fstorture: reuse\n");
  reuse(sub);
  printf("fstorture: %s: %s\n", dir, bad ? "FAILED" : "ok");
  exit(bad != 0);
}
