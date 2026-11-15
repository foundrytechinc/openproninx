// exec(3): vector forms over execve(2), PATH walked like execvp
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

int execv(const char *path, char *const argv[]) {
  return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
  char full[PATH_MAX];
  const char *path, *p;
  size_t n, len = strlen(file);
  int seen_eacces = 0;

  if (strchr(file, '/') != 0)
    return execve(file, argv, environ);
  if ((path = getenv("PATH")) == 0)
    path = "/usr/bin:/bin";
  for (p = path;; p += n + 1) {
    n = strcspn(p, ":");
    if (n == 0) {
      if (len + 1 > sizeof(full))
        goto toolong;
      memcpy(full, file, len + 1);
    } else {
      if (n + 1 + len + 1 > sizeof(full))
        goto toolong;
      memcpy(full, p, n);
      full[n] = '/';
      memcpy(full + n + 1, file, len + 1);
    }
    execve(full, argv, environ);
    if (errno == EACCES)
      seen_eacces = 1;
    else if (errno != ENOENT && errno != ENOTDIR)
      return -1;
  toolong:
    if (p[n] == 0)
      break;
  }
  errno = seen_eacces ? EACCES : ENOENT;
  return -1;
}

#define EXECL_MAX 64

static int gather(const char *arg, va_list ap, char **argv) {
  int i = 0;

  argv[i++] = (char *)arg;
  while (i < EXECL_MAX && (argv[i++] = va_arg(ap, char *)) != 0)
    ;
  if (i == EXECL_MAX) {
    errno = E2BIG;
    return -1;
  }
  return 0;
}

int execl(const char *path, const char *arg, ...) {
  char *argv[EXECL_MAX];
  va_list ap;
  int r;

  va_start(ap, arg);
  r = gather(arg, ap, argv);
  va_end(ap);
  return r < 0 ? -1 : execv(path, argv);
}

int execlp(const char *file, const char *arg, ...) {
  char *argv[EXECL_MAX];
  va_list ap;
  int r;

  va_start(ap, arg);
  r = gather(arg, ap, argv);
  va_end(ap);
  return r < 0 ? -1 : execvp(file, argv);
}
