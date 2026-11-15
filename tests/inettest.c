#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define TCP_PORT 7701
#define UDP_PORT 7702

static int bad;

static void check(int ok, const char *what) {
  if (ok)
    printf("inettest: %s ok\n", what);
  else
    printf("inettest: %s FAILED (%s)\n", what, strerror(errno));
  if (!ok)
    bad = 1;
}

static socklen_t loopback(struct sockaddr_in *sin, int port) {
  memset(sin, 0, sizeof(*sin));
  sin->sin_len = sizeof(*sin);
  sin->sin_family = AF_INET;
  sin->sin_port = htons(port);
  sin->sin_addr.s_addr = inet_addr("127.0.0.1");
  return sizeof(*sin);
}

static void stream(void) {
  struct sockaddr_in sin;
  socklen_t len;
  int l, c, s, i, status, one = 1;
  char buf[32];
  ssize_t n;
  pid_t pid;

  l = socket(AF_INET, SOCK_STREAM, 0);
  setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  len = loopback(&sin, TCP_PORT);
  check(bind(l, (struct sockaddr *)&sin, len) == 0 && listen(l, 4) == 0,
        "a listener on the loopback");
  c = socket(AF_INET, SOCK_STREAM, 0);
  check(connect(c, (struct sockaddr *)&sin, len) == 0, "connected");
  check((s = accept(l, 0, 0)) >= 0, "accepted");
  check(recv(s, buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN,
        "MSG_DONTWAIT with nothing there");
  check(send(c, "peekaboo", 8, 0) == 8, "sent");
  usleep(100000);
  check(recv(s, buf, 4, MSG_PEEK) == 4 && memcmp(buf, "peek", 4) == 0,
        "MSG_PEEK sees it");
  check(recv(s, buf, 8, MSG_WAITALL) == 8 && memcmp(buf, "peekaboo", 8) == 0,
        "and leaves it");
  if ((pid = fork()) == 0) {
    send(c, "half", 4, 0);
    usleep(200000);
    send(c, "whole", 5, 0);
    _exit(0);
  }
  check(recv(s, buf, 9, MSG_WAITALL) == 9 && memcmp(buf, "halfwhole", 9) == 0,
        "MSG_WAITALL waits for all of it");
  waitpid(pid, &status, 0);

  close(s);
  usleep(100000);
  signal(SIGPIPE, SIG_DFL);
  if ((pid = fork()) == 0) {
    for (i = 0; i < 50; i++) {
      if ((n = send(c, "x", 1, MSG_NOSIGNAL)) < 0)
        _exit(errno == EPIPE || errno == ECONNRESET ? 0 : 1);
      usleep(20000);
    }
    _exit(2);
  }
  check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "MSG_NOSIGNAL to a closed peer is an error, not a signal");
  signal(SIGPIPE, SIG_IGN);
  close(c);
  close(l);
}

static void dgram(void) {
  struct sockaddr_in sin, far;
  struct msghdr msg;
  struct iovec iov;
  socklen_t len;
  int a, b;
  char buf[32];

  a = socket(AF_INET, SOCK_DGRAM, 0);
  b = socket(AF_INET, SOCK_DGRAM, 0);
  len = loopback(&sin, UDP_PORT);
  check(bind(b, (struct sockaddr *)&sin, len) == 0, "a datagram socket bound");
  check(sendto(a, "datagram", 8, 0, (struct sockaddr *)&sin, len) == 8, "sent to it");
  check(recv(b, buf, 4, MSG_PEEK) == 4 && memcmp(buf, "data", 4) == 0, "peeked at");
  memset(&msg, 0, sizeof(msg));
  iov.iov_base = buf;
  iov.iov_len = 4;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  check(recvmsg(b, &msg, 0) == 4 && (msg.msg_flags & MSG_TRUNC) &&
            memcmp(buf, "data", 4) == 0,
        "cut short, MSG_TRUNC says so");
  check(recv(b, buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN,
        "and then nothing");
  check(sendto(a, "near", 4, MSG_DONTROUTE, (struct sockaddr *)&sin, len) == 4 &&
            recv(b, buf, sizeof(buf), 0) == 4,
        "MSG_DONTROUTE to the loopback goes");
  len = loopback(&far, UDP_PORT);
  far.sin_addr.s_addr = inet_addr("192.0.2.1");
  check(sendto(a, "far", 3, MSG_DONTROUTE, (struct sockaddr *)&far, len) < 0,
        "and to a host on no wire of ours does not");
  close(a);
  close(b);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  stream();
  dgram();
  printf("inettest: %s\n", bad ? "FAILED" : "ok");
  return bad;
}
