// pidfile(3): the lock is the claim, the number is for others
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <util.h>

#define VARRUN "/var/run/"
#define READ_TRIES 5

extern char *__progname;

struct pidfh {
  int fd;
  char path[256];
  dev_t dev;
  ino_t ino;
};

// the holder may not have written its number yet
static int read_pid(const char *path, pid_t *pid) {
  struct timespec nap = {0, 5 * 1000 * 1000};
  char buf[16], *end;
  ssize_t n = 0;
  long v;
  int fd, tries;

  for (tries = 0; tries < READ_TRIES; tries++) {
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
      return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0)
      break;
    nanosleep(&nap, 0);
  }
  if (n <= 0) {
    errno = EAGAIN;
    return -1;
  }
  buf[n] = 0;
  v = strtol(buf, &end, 10);
  if (end == buf || v <= 0) {
    errno = EINVAL;
    return -1;
  }
  *pid = (pid_t)v;
  return 0;
}

struct pidfh *pidfile_open(const char *path, mode_t mode, pid_t *otherpid) {
  struct pidfh *pfh;
  struct stat st, now;
  int fd, e;

  if ((pfh = calloc(1, sizeof(*pfh))) == 0)
    return 0;
  if (path == 0)
    snprintf(pfh->path, sizeof(pfh->path), "%s%s.pid", VARRUN, __progname);
  else if (strlcpy(pfh->path, path, sizeof(pfh->path)) >= sizeof(pfh->path)) {
    free(pfh);
    errno = ENAMETOOLONG;
    return 0;
  }
  // unlinked between open and lock = not ours
  for (;;) {
    if ((fd = open(pfh->path, O_WRONLY | O_CREAT | O_CLOEXEC, mode)) < 0)
      goto fail;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
      e = errno;
      close(fd);
      if (e == EWOULDBLOCK) {
        if (otherpid != 0 && read_pid(pfh->path, otherpid) < 0)
          *otherpid = -1;
        e = EEXIST;
      }
      errno = e;
      goto fail;
    }
    if (fstat(fd, &st) < 0 || stat(pfh->path, &now) < 0) {
      e = errno;
      close(fd);
      if (e == ENOENT)
        continue;
      errno = e;
      goto fail;
    }
    if (st.st_dev == now.st_dev && st.st_ino == now.st_ino)
      break;
    close(fd);
  }
  if (ftruncate(fd, 0) < 0) {
    e = errno;
    unlink(pfh->path);
    close(fd);
    errno = e;
    goto fail;
  }
  pfh->fd = fd;
  pfh->dev = st.st_dev;
  pfh->ino = st.st_ino;
  return pfh;
fail:
  free(pfh);
  return 0;
}

int pidfile_write(struct pidfh *pfh) {
  char buf[16];
  int n;

  if (pfh == 0 || pfh->fd < 0) {
    errno = EINVAL;
    return -1;
  }
  n = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
  if (ftruncate(pfh->fd, 0) < 0 || pwrite(pfh->fd, buf, (size_t)n, 0) != n)
    return -1;
  return 0;
}

int pidfile_close(struct pidfh *pfh) {
  int r;

  if (pfh == 0)
    return 0;
  r = close(pfh->fd);
  free(pfh);
  return r;
}

int pidfile_remove(struct pidfh *pfh) {
  struct stat st;

  if (pfh == 0)
    return 0;
  if (stat(pfh->path, &st) == 0 && st.st_dev == pfh->dev && st.st_ino == pfh->ino)
    unlink(pfh->path);
  return pidfile_close(pfh);
}

int pidfile_fileno(const struct pidfh *pfh) {
  if (pfh == 0) {
    errno = EINVAL;
    return -1;
  }
  return pfh->fd;
}
