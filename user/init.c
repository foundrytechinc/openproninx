#include "inc/dir.h"
#include "inc/signal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <limits.h>
#include <fnu/sys.h>
#include <fnu/crypt.h>
#include <pwd.h>
#include <err.h>
#include <util.h>

#define SUPERVISOR "/usr/bin/fnusvc"
#define SHELL "/usr/bin/sh"
#define CONSOLE "/dev/console"
#define DEVDIR "/dev"
#define INIT_CONF "/etc/init.conf"

#define DEATH_WATCH 2000
#define STALL_SECONDS 5

// dying this soon is failing
#define RESPAWN_SPACING 5000
#define RESPAWN_LIMIT 3

typedef enum {
  s_none,
  s_multi_user, // the supervisor is up
  s_reread, // it reads its files again
  s_boot_shell, // the boot stage is done: a console shell first
  s_clean, // it is gone
  s_single_user, // a console shell only
  s_catatonia, // stop starting things
  s_death, // stop everything
  s_reboot,
  s_halt,
  s_poweroff
} state_t;

static volatile int requested = s_none;

static pid_t supervisor;
static uint64 supervisor_started;
static int supervisor_fast;
static int boot_note = -1;
static int boot_shell_done;

// init.conf
static int console_secure;
static uint64 shutdown_timeout = 90000;
static int start_single, start_boot;

static void read_init_conf(void) {
  char line[256], *eq, *v, *end;
  FILE *fp;

  if ((fp = fopen(INIT_CONF, "r")) == 0)
    return;
  while (fgets(line, sizeof(line), fp) != 0) {
    line[strcspn(line, "#\r\n")] = 0;
    if ((eq = strchr(line, '=')) == 0)
      continue;
    *eq = 0;
    for (end = eq; end > line && (end[-1] == ' ' || end[-1] == '\t');)
      *--end = 0;
    for (v = eq + 1; *v == ' ' || *v == '\t'; v++)
      ;
    for (end = v + strlen(v); end > v && (end[-1] == ' ' || end[-1] == '\t');)
      *--end = 0;
    if (strcmp(line, "console") == 0)
      console_secure = strcmp(v, "secure") == 0;
    else if (strcmp(line, "shutdown_timeout") == 0)
      shutdown_timeout = (uint64)strtoul(v, 0, 10) * 1000;
    else if (strcmp(line, "start") == 0) {
      start_single = strcmp(v, "single") == 0;
      start_boot = strcmp(v, "boot") == 0;
    }
  }
  fclose(fp);
}
static volatile int disaster_sig;

static uint64 now(void) {
  struct fnu_info sys;

  return fnu_info(&sys) == 0 ? sys.uptime * 1000 / FNU_HZ : 0;
}

static const int act_sigs[] = {SIGHUP,  SIGINT,  SIGTERM,
                               SIGUSR1, SIGUSR2, SIGTSTP};
static const int fatal_sigs[] = {SIGILL, SIGTRAP, SIGABRT, SIGEMT, SIGFPE,
                                 SIGBUS, SIGSEGV, SIGSYS,  SIGXCPU, SIGXFSZ};

static void on_transition(int sig, siginfo_t *si __unused, void *ctx __unused) {
  switch (sig) {
  case SIGHUP: requested = s_reread; break;
  case SIGINT: requested = s_reboot; break;
  case SIGTERM: requested = s_death; break;
  case SIGUSR1: requested = s_halt; break;
  case SIGUSR2: requested = s_poweroff; break;
  case SIGTSTP: requested = s_catatonia; break;
  }
}

static void on_disaster(int sig, siginfo_t *si, void *ctx __unused) {
  char msg[80];
  int n;

  disaster_sig = sig;
  n = snprintf(msg, sizeof(msg),
               "\nfnu-init: fatal signal %d at %p -- rebooting\n", sig,
               si->si_addr);
  if (n >= (int)sizeof(msg))
    n = sizeof(msg) - 1;
  if (n > 0)
    write(STDERR_FILENO, msg, (size_t)n);
  fnu_halt(FNU_HALT_REBOOT | FNU_HALT_FORCE);
  for (;;)
    sleep(STALL_SECONDS);
}

static const sigset_t nothing;

static void catch_signals(void) {
  struct sigaction act;
  uint32_t handled = 0;
  uint i;

  act.sa_flags = SA_SIGINFO;

  act.sa_mask = 0;
  for (i = 0; i < nitems(act_sigs); i++)
    act.sa_mask |= sigmask(act_sigs[i]);
  act.sa_sigaction = on_transition;
  for (i = 0; i < nitems(act_sigs); i++) {
    sigaction(act_sigs[i], &act, 0);
    handled |= sigmask(act_sigs[i]);
  }

  act.sa_sigaction = on_disaster;
  act.sa_mask = 0;
  for (i = 0; i < nitems(fatal_sigs); i++) {
    sigaction(fatal_sigs[i], &act, 0);
    handled |= sigmask(fatal_sigs[i]);
  }

  handled = ~handled;
  sigprocmask(SIG_SETMASK, &handled, 0);
}

static void wall(const char *what) {
  char line[160], path[MAXPATHLEN], scratch[512];
  int dir, fd, n, off;

  strlcpy(line, "\n\n*** FINAL system shutdown message ***\n"
                   "THE SYSTEM IS GOING DOWN FOR ",
             sizeof(line));
  strlcpy(line + strlen(line), what, sizeof(line) - strlen(line));
  strlcpy(line + strlen(line), " NOW\n\n", sizeof(line) - strlen(line));

  if ((dir = open(DEVDIR, O_RDONLY)) < 0)
    return;
  while ((n = getdirentries(dir, scratch, sizeof(scratch), 0)) > 0) {
    for (off = 0; off < n;) {
      struct dirent *de = (struct dirent *)(scratch + off);

      if (de->d_reclen < _GENERIC_DIRLEN(0) || de->d_reclen > n - off)
        break;
      off += de->d_reclen;
      if (de->d_fileno == 0 || strncmp(de->d_name, "tty", 3) != 0)
        continue;
      strlcpy(path, DEVDIR "/", sizeof(path));
      strlcpy(path + strlen(path), de->d_name,
                 sizeof(path) - strlen(path));
      if ((fd = open(path, O_RDWR)) < 0)
        continue;
      write(fd, line, strlen(line));
      close(fd);
    }
  }
  close(dir);
}

// SIGCHLD and the transitions as events
static int events = -1;

// the kernel's numbers for act_sigs, and SIGCHLD
static const int native_sigs[] = {FNU_SIG_HANGUP, FNU_SIG_INTERRUPT, FNU_SIG_TERMINATE,
                                  FNU_SIG_USER1,  FNU_SIG_USER2,     FNU_SIG_TTYSTOP,
                                  FNU_SIG_CHILD};

static int watch_signals(void) {
  struct fnu_event ev[nitems(native_sigs)];
  uint i;

  if ((events = fnu_eventq(FNU_FD_CLOSEEXEC)) < 0)
    return -1;
  memset(ev, 0, sizeof(ev));
  for (i = 0; i < nitems(ev); i++) {
    ev[i].ident = (uint64)native_sigs[i];
    ev[i].filter = FNU_EVF_SIGNAL;
    ev[i].flags = FNU_EV_ADD | FNU_EV_CLEAR;
  }
  if (fnu_events(events, ev, (int)nitems(ev), 0, 0, 0) < 0) {
    close(events);
    events = -1;
    return -1;
  }
  return 0;
}

// until an event, or the deadline; 0 for none
static void await(uint64 deadline) {
  struct fnu_event ev[8];
  struct fnu_timespec ts, *tp = 0;
  uint64 t;

  if (events < 0 && watch_signals() < 0) {
    warnx("no event queue");
    sleep(1);
    return;
  }
  if (deadline != 0) {
    if ((t = now()) >= deadline)
      return;
    ts.sec = (int64_t)((deadline - t) / 1000);
    ts.nsec = (int32_t)((deadline - t) % 1000 * 1000000);
    ts.pad = 0;
    tp = &ts;
  }
  fnu_events(events, 0, 0, ev, (int)nitems(ev), tp);
}

static int reap(uint64 watch) {
  uint64 deadline = now() + watch;
  int n = 0;
  pid_t pid;

  for (;;) {
    while ((pid = waitpid(-1, 0, WNOHANG)) > 0)
      n++;
    if (pid < 0 || now() >= deadline)
      return n;
    await(deadline);
  }
}

static int wait_for(pid_t who, uint64 watch) {
  uint64 deadline = now() + watch;
  pid_t pid;

  for (;;) {
    pid = waitpid(who, 0, WNOHANG);
    if (pid == who || pid < 0)
      return 0;
    if (now() >= deadline)
      return -1;
    await(deadline);
  }
}

static void stop_everything(void) {
  static const int ladder[] = {SIGHUP, SIGTERM, SIGKILL};
  uint i;
  int n;

  for (i = 0; i < nitems(ladder); i++) {
    if (kill(-1, ladder[i]) < 0)
      return;
    n = reap(i + 1 < nitems(ladder) ? DEATH_WATCH : DEATH_WATCH / 2);
    printf("fnu-init: %d stopped on signal %d\n", n, ladder[i]);
  }
  // signal zero only asks
  if (kill(-1, 0) == 0)
    printf("fnu-init: some processes would not stop\n");
}

// the supervisor's word that the boot stage is done
static int note_w = -1;

static int notify_pipe(void) {
  struct fnu_event ev;
  int fd[2];

  if (pipe2(fd, O_CLOEXEC | O_NONBLOCK) < 0)
    return -1;
  note_w = fd[1];
  memset(&ev, 0, sizeof(ev));
  ev.ident = (uint64)fd[0];
  ev.filter = FNU_EVF_READ;
  ev.flags = FNU_EV_ADD;
  if (events >= 0)
    fnu_events(events, &ev, 1, 0, 0, 0);
  return fd[0];
}

static int note_read(void) {
  char buf[16];
  ssize_t n = read(boot_note, buf, sizeof(buf));

  if (n == 0 || (n < 0 && errno != EAGAIN)) {
    close(boot_note);
    boot_note = -1;
    return 0;
  }
  if (n < 0)
    return 0;
  close(boot_note);
  boot_note = -1;
  return 1;
}

static pid_t start(const char *path) {
  char *argv[2];
  pid_t pid;

  argv[0] = (char *)path;
  argv[1] = 0;
  if ((pid = fork()) < 0)
    return -1;
  if (pid != 0)
    return pid;

  // exec keeps the mask
  sigprocmask(SIG_SETMASK, &nothing, 0);
  if (boot_note >= 0 && note_w >= 0) {
    if (note_w == 3)
      fcntl(3, F_SETFD, 0);
    else
      dup2(note_w, 3);
    setenv("FNUSVC_NOTIFY", "3", 1);
  }
  execv(argv[0], argv);
  warn("%s", path);
  exit(1);
}

static int root_writable(void) {
  struct fnu_fsstat *table;
  int n, i, rw = 1;

  if ((n = fnu_volumes(&table)) < 0)
    return 1;
  for (i = 0; i < n; i++)
    if (strcmp(table[i].on, "/") == 0)
      rw = !(table[i].flags & FNU_MNT_RDONLY);
  fnu_table_free(table);
  return rw;
}

// sulogin. ^D и дальше
static int single_auth(void) {
  char password[CRYPT_PASSWORD_MAX];
  struct passwd *pw = getpwnam("root");
  int ok;

  if (console_secure || pw == 0 || pw->pw_passwd[0] == 0)
    return 1;
  for (;;) {
    if (read_password("Enter root password, or ^D to go on to multi user: ",
                      password, sizeof(password)) < 0 || password[0] == 0) {
      printf("\n");
      return 0;
    }
    ok = crypt_checkpass(password, pw->pw_passwd) == 0;
    explicit_bzero(password, sizeof(password));
    if (ok)
      return 1;
    printf("Login incorrect\n");
    sleep(1);
  }
}

// a root shell on the console
static int console_shell(void) {
  pid_t pid, dead;

  if ((pid = fork()) < 0)
    return -1;
  if (pid == 0) {
    struct passwd *pw;
    char *argv[2];

    argv[0] = (char *)SHELL;
    argv[1] = 0;
    sigprocmask(SIG_SETMASK, &nothing, 0);
    if (login_tty(0) < 0)
      warn("console");
    if (!single_auth())
      _exit(0);
    setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
    setenv("HOME", (pw = getpwnam("root")) != 0 && pw->pw_dir[0] != 0 ? pw->pw_dir : "/", 1);
    printf("fnu-init: the root is %s; mount -uw / makes it writable\n",
           root_writable() ? "writable" : "read-only");
    execv(argv[0], argv);
    warn("%s", SHELL);
    exit(1);
  }
  for (;;) {
    while ((dead = waitpid(-1, 0, WNOHANG)) > 0) {
      if (dead == pid)
        return 0;
      if (dead == supervisor)
        supervisor = 0;
    }
    if (requested != s_none || dead < 0)
      return 0;
    await(0);
  }
}

static state_t f_single_user(void) {
  printf("fnu-init: single user mode\n");
  if (console_shell() < 0) {
    sleep(STALL_SECONDS);
    return s_single_user;
  }
  return requested != s_none ? s_none : s_multi_user;
}

// services get shutdown_timeout, in order
static void stop_supervisor(void) {
  if (supervisor <= 0)
    return;
  printf("fnu-init: stopping the services\n");
  kill(supervisor, SIGTERM);
  if (wait_for(supervisor, shutdown_timeout) < 0) {
    printf("fnu-init: the services did not stop in %llu s\n",
           (unsigned long long)(shutdown_timeout / 1000));
    kill(supervisor, SIGKILL);
    wait_for(supervisor, DEATH_WATCH / 2);
  }
  supervisor = 0;
}

static state_t f_multi_user(void) {
  static int booted;
  pid_t dead;

  if (supervisor <= 0) {
    if (!booted) {
      logwtmp("~", "reboot", "");
      booted = 1;
    }
    supervisor_started = now();
    if (start_boot && !boot_shell_done)
      boot_note = notify_pipe();
    supervisor = start(SUPERVISOR);
  if (note_w >= 0) {
    close(note_w);
    note_w = -1;
  }
  if (supervisor < 0) {
      warn("fork");
      sleep(STALL_SECONDS);
      return s_multi_user;
    }
  }

  for (;;) {
    while ((dead = waitpid(-1, 0, WNOHANG)) > 0)
      if (dead == supervisor) {
        supervisor = 0;
        return s_clean;
      }
    if (requested != s_none)
      return s_none;
    if (dead < 0) {
      supervisor = 0;
      return s_clean;
    }
    if (boot_note >= 0 && note_read())
      return s_boot_shell;
    await(0);
  }
}

// single-user milestone: boot stage up, shell, then multi
static state_t f_boot_shell(void) {
  int fd;

  printf("fnu-init: the boot stage is done; leave this shell for multi user\n");
  console_shell();
  boot_shell_done = 1;
  if (requested != s_none)
    return s_none;
  if ((fd = open("/var/run/fnusvc.ctl", O_WRONLY | O_NONBLOCK)) >= 0) {
    write(fd, "multi\n", 6);
    close(fd);
  }
  return s_multi_user;
}

static state_t f_reread(void) {
  if (supervisor > 0)
    kill(supervisor, SIGHUP);
  return s_multi_user;
}

// repeating too quickly
static state_t f_clean(void) {
  printf("fnu-init: the supervisor exited\n");
  if (now() - supervisor_started < RESPAWN_SPACING)
    supervisor_fast++;
  else
    supervisor_fast = 0;
  if (supervisor_fast >= RESPAWN_LIMIT) {
    supervisor_fast = 0;
    printf("fnu-init: the supervisor keeps failing\n");
    return s_single_user;
  }
  return s_multi_user;
}

// catatonia
static state_t f_catatonia(void) {
  pid_t dead;

  for (;;) {
    while ((dead = waitpid(-1, 0, WNOHANG)) > 0)
      if (dead == supervisor)
        supervisor = 0;
    if (requested != s_none)
      return s_none;
    await(0);
  }
}

static state_t f_death(void) {
  stop_supervisor();
  stop_everything();
  supervisor = 0;
  return s_single_user;
}

static state_t f_down(int howto, const char *what) {
  wall(what);
  stop_supervisor();
  printf("fnu-init: asking every process to stop\n");
  stop_everything();
  logwtmp("~", "shutdown", "");
  printf("fnu-init: handing the machine to the kernel\n");
  fnu_halt(howto);
  printf("fnu-init: the kernel would not go down\n");
  supervisor = 0;
  return s_single_user;
}

static state_t f_reboot(void) { return f_down(FNU_HALT_REBOOT, "REBOOT"); }
static state_t f_halt(void) { return f_down(FNU_HALT_STOP, "HALT"); }
static state_t f_poweroff(void) { return f_down(FNU_HALT_POWEROFF, "POWEROFF"); }

static state_t run(state_t s) {
  switch (s) {
  case s_multi_user: return f_multi_user();
  case s_reread: return f_reread();
  case s_boot_shell: return f_boot_shell();
  case s_clean: return f_clean();
  case s_single_user: return f_single_user();
  case s_catatonia: return f_catatonia();
  case s_death: return f_death();
  case s_reboot: return f_reboot();
  case s_halt: return f_halt();
  case s_poweroff: return f_poweroff();
  default: return s_single_user;
  }
}

static void transition(state_t s) {
  for (;;) {
    if (requested != s_none) {
      s = requested;
      requested = s_none;
    }
    s = run(s);
    if (s == s_none)
      s = s_single_user;
  }
}

static void console_setup(void) {
  int fd;

  while ((fd = open(CONSOLE, O_RDWR)) < 0)
    sleep(STALL_SECONDS);
  if (fd != 0) {
    if (dup2(fd, 0) < 0)
      for (;;)
        sleep(STALL_SECONDS);
    close(fd);
  }
  if (dup(0) < 0 || dup(0) < 0)
    for (;;)
      sleep(STALL_SECONDS);
}

int main(int argc, char **argv) {
  int single = argc > 1 && strcmp(argv[1], "-s") == 0;

  if (getuid() != 0)
    errx(1, "%s", strerror(EPERM));
  if (getpid() != 1)
    errx(1, "already running");
  console_setup();
  read_init_conf();
  catch_signals();
  watch_signals();
  transition(single || start_single ? s_single_user : s_multi_user);
  return 0;
}
