// POSIX over the system's own process and identity calls
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <fnu/sys.h>
#include "posix.h"
#include "thread_private.h"

// with threads, malloc's locks and the rest are taken across it
pid_t fork(void) {
  pid_t pid;

  if (!__isthreaded)
    return (pid_t)__posix(fnu_fork());
  _thread_fork_prepare();
  pid = (pid_t)fnu_fork();
  if (pid == 0)
    _thread_fork_child();
  else
    _thread_fork_parent();
  return (pid_t)__posix(pid);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
  return (int)__posix(fnu_exec(path, argv, envp));
}

static pid_t ids_of(pid_t pid, int which) {
  struct fnu_procids ids;

  if (__posix(fnu_procids(pid, &ids)) < 0)
    return -1;
  return which == 0 ? ids.pid : which == 1 ? ids.ppid : which == 2 ? ids.pgid : ids.sid;
}

pid_t getpid(void) { return ids_of(0, 0); }
pid_t getppid(void) { return ids_of(0, 1); }
pid_t getpgid(pid_t pid) { return ids_of(pid, 2); }
pid_t getsid(pid_t pid) { return ids_of(pid, 3); }
pid_t setsid(void) { return (pid_t)__posix(fnu_session_new()); }

int setpgid(pid_t pid, pid_t pgrp) {
  return (int)__posix(fnu_pgroup_set(pid, pgrp));
}

// the identity whole: one call answers all of it
static struct fnu_ident *ident(struct fnu_ident *id) {
  return __posix(fnu_ident_get(id)) < 0 ? 0 : id;
}

uid_t getuid(void) {
  struct fnu_ident id;

  return ident(&id) ? id.uid : (uid_t)-1;
}

uid_t geteuid(void) {
  struct fnu_ident id;

  return ident(&id) ? id.euid : (uid_t)-1;
}

gid_t getgid(void) {
  struct fnu_ident id;

  return ident(&id) ? id.gid : (gid_t)-1;
}

gid_t getegid(void) {
  struct fnu_ident id;

  return ident(&id) ? id.egid : (gid_t)-1;
}

int issetugid(void) {
  struct fnu_ident id;

  return ident(&id) ? (id.changed != 0) : 1;
}

int setuid(uid_t uid) {
  struct fnu_ident id;

  memset(&id, 0, sizeof(id));
  id.uid = uid;
  return (int)__posix(fnu_ident_set(&id, FNU_IDENT_UID));
}

int setgid(gid_t gid) {
  struct fnu_ident id;

  memset(&id, 0, sizeof(id));
  id.gid = gid;
  return (int)__posix(fnu_ident_set(&id, FNU_IDENT_GID));
}

// the three ids at once; -1 leaves one as it is
int setresuid(uid_t r, uid_t e, uid_t s) {
  struct fnu_ident id;
  int what = 0;

  memset(&id, 0, sizeof(id));
  if (r != (uid_t)-1) {
    id.uid = r;
    what |= FNU_IDENT_RUID;
  }
  if (e != (uid_t)-1) {
    id.euid = e;
    what |= FNU_IDENT_EUID;
  }
  if (s != (uid_t)-1) {
    id.svuid = s;
    what |= FNU_IDENT_SVUID;
  }
  return what == 0 ? 0 : (int)__posix(fnu_ident_set(&id, what));
}

int setresgid(gid_t r, gid_t e, gid_t s) {
  struct fnu_ident id;
  int what = 0;

  memset(&id, 0, sizeof(id));
  if (r != (gid_t)-1) {
    id.gid = r;
    what |= FNU_IDENT_RGID;
  }
  if (e != (gid_t)-1) {
    id.egid = e;
    what |= FNU_IDENT_EGID;
  }
  if (s != (gid_t)-1) {
    id.svgid = s;
    what |= FNU_IDENT_SVGID;
  }
  return what == 0 ? 0 : (int)__posix(fnu_ident_set(&id, what));
}

int getresuid(uid_t *r, uid_t *e, uid_t *s) {
  struct fnu_ident id;

  if (!ident(&id))
    return -1;
  *r = id.uid;
  *e = id.euid;
  *s = id.svuid;
  return 0;
}

int getresgid(gid_t *r, gid_t *e, gid_t *s) {
  struct fnu_ident id;

  if (!ident(&id))
    return -1;
  *r = id.gid;
  *e = id.egid;
  *s = id.svgid;
  return 0;
}

int seteuid(uid_t e) { return setresuid((uid_t)-1, e, (uid_t)-1); }

int setegid(gid_t e) { return setresgid((gid_t)-1, e, (gid_t)-1); }

// ???
int setreuid(uid_t r, uid_t e) {
  struct fnu_ident id;
  uid_t s = (uid_t)-1;

  if (!ident(&id))
    return -1;
  if (r != (uid_t)-1 && (r != id.uid || id.svuid != (e != (uid_t)-1 ? e : id.euid)))
    s = r;
  return setresuid(r, e, s);
}

int setregid(gid_t r, gid_t e) {
  struct fnu_ident id;
  gid_t s = (gid_t)-1;

  if (!ident(&id))
    return -1;
  if (r != (gid_t)-1 && (r != id.gid || id.svgid != (e != (gid_t)-1 ? e : id.egid)))
    s = r;
  return setresgid(r, e, s);
}

int setgroups(int n, const gid_t *list) {
  struct fnu_ident id;

  if (n < 0 || n > FNU_GROUPS_MAX) {
    errno = EINVAL;
    return -1;
  }
  memset(&id, 0, sizeof(id));
  id.ngroups = (uint32_t)n;
  memcpy(id.groups, list, (size_t)n * sizeof(id.groups[0]));
  return (int)__posix(fnu_ident_set(&id, FNU_IDENT_GROUPS));
}

int getgroups(int n, gid_t *list) {
  struct fnu_ident id;

  if (n < 0 || ident(&id) == 0) {
    if (n < 0)
      errno = EINVAL;
    return -1;
  }
  if (n == 0)
    return (int)id.ngroups;
  if (n < (int)id.ngroups) {
    errno = EINVAL;
    return -1;
  }
  memcpy(list, id.groups, id.ngroups * sizeof(id.groups[0]));
  return (int)id.ngroups;
}

int setlogin(const char *name) {
  struct fnu_ident id;

  memset(&id, 0, sizeof(id));
  if (strlcpy(id.login, name, sizeof(id.login)) >= sizeof(id.login)) {
    errno = EINVAL;
    return -1;
  }
  return (int)__posix(fnu_ident_set(&id, FNU_IDENT_LOGIN));
}

int getlogin_r(char *name, size_t len) {
  struct fnu_ident id;
  int saved = errno, r = 0;

  if (ident(&id) == 0)
    r = errno;
  else if (id.login[0] == 0)
    r = ENOENT;
  else if (strlcpy(name, id.login, len) >= len)
    r = ERANGE;
  errno = saved;
  return r;
}

// pledge is native; only the errors are POSIX
int pledge(const char *promises, const char *execpromises) {
  return (int)__posix(fnu_pledge(promises, execpromises));
}

int unveil(const char *path, const char *permissions) {
  return (int)__posix(fnu_unveil(path, permissions));
}

static int whom_of(int which) {
  switch (which) {
  case PRIO_PROCESS: return FNU_NICE_PROCESS;
  case PRIO_PGRP: return FNU_NICE_GROUP;
  case PRIO_USER: return FNU_NICE_USER;
  default: return -1;
  }
}

// -1 is a niceness too: errno says which it was
int getpriority(int which, id_t who) {
  int v = 0, whom = whom_of(which);

  if (whom < 0 || who > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  return __posix(fnu_niceness(whom, (int)who, FNU_NICE_GET, &v)) < 0 ? -1 : v;
}

int setpriority(int which, id_t who, int prio) {
  int whom = whom_of(which);

  if (whom < 0 || who > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  return (int)__posix(fnu_niceness(whom, (int)who, FNU_NICE_SET, &prio));
}

int nice(int incr) {
  int v = incr;

  return __posix(fnu_niceness(FNU_NICE_PROCESS, 0, FNU_NICE_ADD, &v)) < 0 ? -1 : v;
}

int getentropy(void *buf, size_t len) {
  if (len > 256) {
    errno = EINVAL;
    return -1;
  }
  return (int)__posix(fnu_random(buf, len));
}

// kern.hostname; a name too long for the room is an error
int gethostname(char *name, size_t len) {
  size_t got = len;

  if (sysctlbyname("kern.hostname", name, &got, 0, 0) < 0) {
    if (errno == ENOMEM)
      errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

int sethostname(const char *name, size_t len) {
  return sysctlbyname("kern.hostname", 0, 0, name, len);
}

int __sig_to_native(int);

// a word can look like an errno, hence PT_IO
int64_t ptrace(int request, pid_t pid, uintptr_t addr, int64_t data) {
  struct fnu_ptrace_io io;
  int64_t word;
  long r;

  // a signal to deliver is said in the system's number
  if ((request == PT_CONTINUE || request == PT_STEP || request == PT_SYSCALL ||
       request == PT_DETACH) && data != 0 &&
      (data = __sig_to_native((int)data)) == 0) {
    errno = EINVAL;
    return -1;
  }
  if (request != PT_READ_I && request != PT_READ_D)
    return __posix(fnu_ptrace(request, pid, addr, data));
  io.op = PT_IO_READ;
  io.len = sizeof(word);
  io.addr = addr;
  io.buf = (uint64_t)(uintptr_t)&word;
  if ((r = __posix(fnu_ptrace(PT_IO, pid, (uintptr_t)&io, 0))) < 0)
    return -1;
  if (r != sizeof(word)) {
    errno = EFAULT;
    return -1;
  }
  return word;
}

// prot and map kinds are the system's numbers
_Static_assert(PROT_NONE == FNU_MEM_NONE && PROT_READ == FNU_MEM_READ &&
                   PROT_WRITE == FNU_MEM_WRITE && PROT_EXEC == FNU_MEM_EXEC,
               "protections");
_Static_assert(MAP_SHARED == FNU_MEM_SHARED && MAP_PRIVATE == FNU_MEM_PRIVATE &&
                   MAP_FIXED == FNU_MEM_FIXED && MAP_ANON == FNU_MEM_ANON,
               "mapping kinds");
_Static_assert(MS_ASYNC == FNU_MEMSYNC_START && MS_SYNC == FNU_MEMSYNC_WAIT,
               "msync");

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
  long r = fnu_map(addr, len, prot, flags, fd, offset);

  if (r < 0) {
    errno = __errno_of((int)-r);
    return MAP_FAILED;
  }
  return (void *)r;
}

int mprotect(void *addr, size_t len, int prot) {
  return (int)__posix(fnu_protect(addr, len, prot));
}

int munmap(void *addr, size_t len) {
  return (int)__posix(fnu_unmap(addr, len));
}

int msync(void *addr, size_t len, int flags) {
  return (int)__posix(fnu_memsync(addr, len, flags));
}
