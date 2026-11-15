
#include <sys/types.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FILE_ "/tmp/eventtest.f"

static int failures;

static void ok(const char *what, int good) {
  printf("eventtest: %-48s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

static int wait1(int kq, struct kevent *ev, int ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

  return kevent(kq, 0, 0, ev, 1, &ts);
}

int main(void) {
  struct kevent ch, ev;
  int kq, p[2], fd, status;
  pid_t pid;

  if ((kq = kqueue()) < 0)
    err(1, "kqueue");

  if (pipe(p) < 0)
    err(1, "pipe");
  EV_SET(&ch, p[0], EVFILT_READ, EV_ADD, 0, 0, (void *)7);
  ok("a pipe is watched", kevent(kq, &ch, 1, 0, 0, 0) == 0);
  ok("nothing yet", wait1(kq, &ev, 0) == 0);
  write(p[1], "hello", 5);
  ok("five bytes are said", wait1(kq, &ev, 100) == 1 && ev.ident == (uintptr_t)p[0] &&
                                ev.filter == EVFILT_READ && ev.data == 5 &&
                                ev.udata == (void *)7);
  close(p[1]);
  ok("and the writer's end", wait1(kq, &ev, 100) == 1 && (ev.flags & EV_EOF));
  close(p[0]);
  ok("a closed descriptor's note is gone", wait1(kq, &ev, 0) == 0);

  EV_SET(&ch, 999, EVFILT_READ, EV_ADD, 0, 0, 0);
  ok("a bad descriptor is EV_ERROR, EBADF",
     kevent(kq, &ch, 1, &ev, 1, 0) == 1 && (ev.flags & EV_ERROR) && ev.data == EBADF);
  EV_SET(&ch, 1, EVFILT_USER, EV_ADD, 0, 0, 0);
  ok("a filter it has not is EINVAL",
     kevent(kq, &ch, 1, &ev, 1, 0) == 1 && (ev.flags & EV_ERROR) && ev.data == EINVAL);

  // a timer counts
  EV_SET(&ch, 42, EVFILT_TIMER, EV_ADD, 0, 50, 0);
  kevent(kq, &ch, 1, 0, 0, 0);
  ok("a 50 ms timer fires", wait1(kq, &ev, 1000) == 1 && ev.filter == EVFILT_TIMER &&
                               ev.ident == 42 && ev.data >= 1);
  EV_SET(&ch, 42, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
  ok("and is deleted", kevent(kq, &ch, 1, 0, 0, 0) == 0);
  ok("deleting it again is ENOENT", kevent(kq, &ch, 1, 0, 0, 0) < 0 && errno == ENOENT);

  signal(SIGUSR1, SIG_IGN);
  EV_SET(&ch, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, 0);
  kevent(kq, &ch, 1, 0, 0, 0);
  kill(getpid(), SIGUSR1);
  kill(getpid(), SIGUSR1);
  ok("two ignored SIGUSR1 are counted", wait1(kq, &ev, 500) == 1 &&
                                           ev.filter == EVFILT_SIGNAL &&
                                           ev.ident == SIGUSR1 && ev.data == 2);
  EV_SET(&ch, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, 0);
  kevent(kq, &ch, 1, 0, 0, 0);

  if ((pid = fork()) == 0) {
    usleep(100000);
    _exit(3);
  }
  EV_SET(&ch, pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, 0);
  ok("a child is watched", kevent(kq, &ch, 1, 0, 0, 0) == 0);
  ok("its exit comes with its status",
     wait1(kq, &ev, 2000) == 1 && ev.filter == EVFILT_PROC && (ev.fflags & NOTE_EXIT) &&
         WIFEXITED((int)ev.data) && WEXITSTATUS((int)ev.data) == 3);
  waitpid(pid, &status, 0);
  ok("a process that is not is ESRCH",
     kevent(kq, &ch, 1, &ev, 1, 0) == 1 && (ev.flags & EV_ERROR) && ev.data == ESRCH);

  if ((fd = open(FILE_, O_CREAT | O_TRUNC | O_RDWR, 0644)) < 0)
    err(1, "%s", FILE_);
  EV_SET(&ch, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE,
         0, 0);
  kevent(kq, &ch, 1, 0, 0, 0);
  write(fd, "abc", 3);
  ok("a write and a growth", wait1(kq, &ev, 100) == 1 && ev.filter == EVFILT_VNODE &&
                                (ev.fflags & (NOTE_WRITE | NOTE_EXTEND)) ==
                                    (NOTE_WRITE | NOTE_EXTEND));
  ok("seen once", wait1(kq, &ev, 0) == 0);
  unlink(FILE_);
  ok("and the name taken away", wait1(kq, &ev, 100) == 1 && (ev.fflags & NOTE_DELETE));

  EV_SET(&ch, fd, EVFILT_VNODE, EV_DELETE, 0, 0, 0);
  kevent(kq, &ch, 1, 0, 0, 0);
  lseek(fd, 0, SEEK_SET);
  EV_SET(&ch, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, 0);
  kevent(kq, &ch, 1, 0, 0, 0);
  ok("three bytes to read", wait1(kq, &ev, 100) == 1 && ev.data == 3);
  ok("not said twice", wait1(kq, &ev, 0) == 0);
  pwrite(fd, "de", 2, 3);
  ok("said again when it grew", wait1(kq, &ev, 100) == 1 && ev.data == 5);
  lseek(fd, 0, SEEK_END);
  if ((pid = fork()) == 0) {
    usleep(200000);
    _exit(pwrite(fd, "fgh", 3, 5) == 3 ? 0 : 1);
  }
  ok("another process's write wakes the wait",
     wait1(kq, &ev, 3000) == 1 && ev.filter == EVFILT_READ && ev.data == 3);
  waitpid(pid, &status, 0);

  // EV_DISPATCH
  EV_SET(&ch, 7, EVFILT_TIMER, EV_ADD | EV_DISPATCH, 0, 20, 0);
  kevent(kq, &ch, 1, 0, 0, 0);
  EV_SET(&ch, fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
  kevent(kq, &ch, 1, 0, 0, 0);
  ok("a dispatched timer fires", wait1(kq, &ev, 500) == 1 && ev.ident == 7);
  ok("then is off", wait1(kq, &ev, 100) == 0);
  EV_SET(&ch, 7, EVFILT_TIMER, EV_ENABLE, 0, 20, 0);
  kevent(kq, &ch, 1, 0, 0, 0);
  ok("until enabled again", wait1(kq, &ev, 500) == 1 && ev.ident == 7);
  close(fd);

  if ((pid = fork()) == 0)
    _exit(kevent(kq, 0, 0, 0, 0, 0) < 0 && errno == EBADF ? 0 : 1);
  ok("a child has no queue", waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                                 WEXITSTATUS(status) == 0);
  close(kq);

  printf(failures ? "eventtest: FAILED\n" : "eventtest: ok\n");
  return failures != 0;
}
