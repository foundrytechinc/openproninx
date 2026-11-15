#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>

#define STREAM_PATH "/var/tmp/unixtest.stream"
#define DGRAM_A "/var/tmp/unixtest.a"
#define DGRAM_B "/var/tmp/unixtest.b"

static int bad;

static void check(int ok, const char *what) {
  if (ok)
    printf("unixtest: %s ok\n", what);
  else
    printf("unixtest: %s FAILED (%s)\n", what, strerror(errno));
  if (!ok)
    bad = 1;
}

static socklen_t name_of(struct sockaddr_un *sun, const char *path) {
  memset(sun, 0, sizeof(*sun));
  sun->sun_family = AF_UNIX;
  strncpy(sun->sun_path, path, sizeof(sun->sun_path) - 1);
  return (socklen_t)SUN_LEN(sun);
}

static void pair(void) {
  int sv[2];
  char buf[16];
  struct stat st;

  check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
  check(write(sv[0], "ping", 4) == 4 && read(sv[1], buf, sizeof(buf)) == 4 &&
            memcmp(buf, "ping", 4) == 0,
        "one way");
  check(write(sv[1], "pong", 4) == 4 && read(sv[0], buf, sizeof(buf)) == 4 &&
            memcmp(buf, "pong", 4) == 0,
        "and the other");
  check(fstat(sv[0], &st) == 0 && S_ISSOCK(st.st_mode), "fstat says a socket");
  check(shutdown(sv[0], SHUT_WR) == 0 && read(sv[1], buf, sizeof(buf)) == 0,
        "a shut write end reads as the end");
  close(sv[1]);
  signal(SIGPIPE, SIG_IGN);
  check(write(sv[0], "x", 1) < 0 && errno == EPIPE, "no reader is EPIPE");
  close(sv[0]);
}

static void stream(void) {
  struct sockaddr_un sun, got;
  struct sockpeercred cred;
  socklen_t len, clen;
  struct pollfd pfd;
  struct stat st;
  int l, c, s, status;
  char buf[32];
  uid_t uid;
  gid_t gid;
  pid_t pid;

  unlink(STREAM_PATH);
  check((l = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0, "a stream socket");
  len = name_of(&sun, STREAM_PATH);
  check(bind(l, (struct sockaddr *)&sun, len) == 0, "bound to a name");
  check(stat(STREAM_PATH, &st) == 0 && S_ISSOCK(st.st_mode), "the name is a socket");
  check(bind(socket(AF_UNIX, SOCK_STREAM, 0), (struct sockaddr *)&sun, len) < 0 &&
            errno == EADDRINUSE,
        "a second bind is EADDRINUSE");
  check(listen(l, 4) == 0, "listening");
  fcntl(l, F_SETFL, O_NONBLOCK);
  check(accept(l, 0, 0) < 0 && errno == EAGAIN, "nothing to accept is EAGAIN");
  fcntl(l, F_SETFL, 0);

  if ((pid = fork()) == 0) {
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(c, (struct sockaddr *)&sun, len) < 0)
      _exit(2);
    if (write(c, "hello", 5) != 5 || read(c, buf, sizeof(buf)) != 3)
      _exit(3);
    _exit(memcmp(buf, "bye", 3) == 0 ? 0 : 4);
  }
  pfd.fd = l;
  pfd.events = POLLIN;
  check(poll(&pfd, 1, 5000) == 1 && (pfd.revents & POLLIN), "a connection polls readable");
  clen = sizeof(got);
  check((s = accept(l, (struct sockaddr *)&got, &clen)) >= 0, "accepted");
  check(read(s, buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0, "its data");
  len = sizeof(cred);
  check(getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.pid == pid &&
            cred.uid == getuid(),
        "SO_PEERCRED is the client");
  check(getpeereid(s, &uid, &gid) == 0 && uid == geteuid(), "getpeereid");
  clen = sizeof(got);
  check(getsockname(s, (struct sockaddr *)&got, &clen) == 0 &&
            strcmp(got.sun_path, STREAM_PATH) == 0,
        "the accepted end is named for the listener");
  check(write(s, "bye", 3) == 3, "an answer");
  check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "the client heard it");
  close(s);
  close(l);
  c = socket(AF_UNIX, SOCK_STREAM, 0);
  check(connect(c, (struct sockaddr *)&sun, name_of(&sun, STREAM_PATH)) < 0 &&
            errno == ECONNREFUSED,
        "nobody bound is ECONNREFUSED");
  check(connect(c, (struct sockaddr *)&sun, name_of(&sun, "/etc/passwd")) < 0 &&
            errno == ENOTSOCK,
        "a file is ENOTSOCK");
  close(c);
  unlink(STREAM_PATH);
}

static void dgram(void) {
  struct sockaddr_un a, b, from;
  struct timeval tv = {0, 200000};
  socklen_t alen, blen, flen;
  int sa, sb;
  char buf[32];

  unlink(DGRAM_A);
  unlink(DGRAM_B);
  sa = socket(AF_UNIX, SOCK_DGRAM, 0);
  sb = socket(AF_UNIX, SOCK_DGRAM, 0);
  alen = name_of(&a, DGRAM_A);
  blen = name_of(&b, DGRAM_B);
  check(bind(sa, (struct sockaddr *)&a, alen) == 0 &&
            bind(sb, (struct sockaddr *)&b, blen) == 0,
        "two datagram sockets bound");
  check(sendto(sa, "one", 3, 0, (struct sockaddr *)&b, blen) == 3, "sendto a name");
  flen = sizeof(from);
  check(recvfrom(sb, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen) == 3 &&
            memcmp(buf, "one", 3) == 0 && strcmp(from.sun_path, DGRAM_A) == 0,
        "recvfrom says who sent it");
  check(connect(sb, (struct sockaddr *)&a, alen) == 0 && send(sb, "two", 3, 0) == 3 &&
            recv(sa, buf, sizeof(buf), 0) == 3 && memcmp(buf, "two", 3) == 0,
        "a connected datagram socket");
  check(setsockopt(sa, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
            recv(sa, buf, sizeof(buf), 0) < 0 && errno == EAGAIN,
        "SO_RCVTIMEO runs out");
  close(sa);
  close(sb);
  unlink(DGRAM_A);
  unlink(DGRAM_B);
}

static int send_fd(int s, int fd, int flags) {
  union {
    struct cmsghdr hdr;
    char buf[CMSG_SPACE(sizeof(int))];
  } cm;
  struct msghdr msg;
  struct iovec iov;
  char c = 'f';

  memset(&msg, 0, sizeof(msg));
  memset(&cm, 0, sizeof(cm));
  iov.iov_base = &c;
  iov.iov_len = 1;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = &cm;
  msg.msg_controllen = sizeof(cm.buf);
  cm.hdr.cmsg_len = CMSG_LEN(sizeof(int));
  cm.hdr.cmsg_level = SOL_SOCKET;
  cm.hdr.cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(&cm.hdr), &fd, sizeof(int));
  return (int)sendmsg(s, &msg, flags);
}

static int recv_fd(int s, int flags, size_t room, int *msgflags) {
  union {
    struct cmsghdr hdr;
    char buf[CMSG_SPACE(4 * sizeof(int))];
  } cm;
  struct cmsghdr *c;
  struct msghdr msg;
  struct iovec iov;
  char byte;
  int fd = -1;

  memset(&msg, 0, sizeof(msg));
  iov.iov_base = &byte;
  iov.iov_len = 1;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = &cm;
  msg.msg_controllen = (socklen_t)room;
  if (recvmsg(s, &msg, flags) < 0)
    return -2;
  *msgflags = msg.msg_flags;
  for (c = CMSG_FIRSTHDR(&msg); c != 0; c = CMSG_NXTHDR(&msg, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
        c->cmsg_len >= CMSG_LEN(sizeof(int)))
      memcpy(&fd, CMSG_DATA(c), sizeof(int));
  return fd;
}

static int inflight(void) {
  int v = -1;
  size_t len = sizeof(v);

  if (sysctlbyname("net.local.inflight", &v, &len, 0, 0) < 0)
    return -1;
  return v;
}

static void rights(void) {
  int sv[2], dv[2], p[2], fd, mf, junk[2];
  char buf[16];

  check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 && pipe(p) == 0, "a pair and a pipe");
  check(send_fd(sv[0], p[0], 0) == 1, "a pipe's reading end sent");
  close(p[0]);
  check((fd = recv_fd(sv[1], 0, CMSG_SPACE(sizeof(int)), &mf)) >= 0 && mf == 0,
        "and received");
  check(write(p[1], "abc", 3) == 3 && read(fd, buf, sizeof(buf)) == 3 &&
            memcmp(buf, "abc", 3) == 0,
        "it reads what the pipe was given");
  check((fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0, "no close-on-exec unasked");
  close(fd);

  check(send_fd(sv[0], p[1], 0) == 1, "sent again");
  check((fd = recv_fd(sv[1], MSG_CMSG_CLOEXEC, CMSG_SPACE(sizeof(int)), &mf)) >= 0 &&
            (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0,
        "MSG_CMSG_CLOEXEC sets close-on-exec");
  close(fd);

  check(send_fd(sv[0], p[1], 0) == 1 &&
            recv_fd(sv[1], 0, sizeof(struct cmsghdr), &mf) == -1 && (mf & MSG_CTRUNC),
        "no room for it is MSG_CTRUNC");
  check(send_fd(sv[0], p[1], 0) == 1 && read(sv[1], buf, sizeof(buf)) == 1,
        "read(2) takes the byte and drops the descriptor");
  check(inflight() == 0, "nothing left in flight");
  close(p[1]);

  check(socketpair(AF_UNIX, SOCK_DGRAM, 0, dv) == 0 && pipe(p) == 0, "a datagram pair");
  check(send_fd(dv[0], p[1], 0) == 1, "a descriptor in a datagram");
  close(p[1]);
  check((fd = recv_fd(dv[1], 0, CMSG_SPACE(sizeof(int)), &mf)) >= 0 &&
            write(fd, "z", 1) == 1 && read(p[0], buf, 1) == 1 && buf[0] == 'z',
        "comes out with it");
  close(fd);
  close(p[0]);
  close(dv[0]);
  close(dv[1]);

  check(send_fd(sv[0], sv[0], 0) == 1 && send_fd(sv[1], sv[1], 0) == 1 &&
            inflight() == 2,
        "two sockets in flight in each other");
  close(sv[0]);
  close(sv[1]);
  socketpair(AF_UNIX, SOCK_STREAM, 0, junk);
  close(junk[0]);
  close(junk[1]);
  check(inflight() == 0, "the cycle is collected");
}

static void flags(void) {
  int sv[2], dv[2], status;
  char buf[32];
  pid_t pid;

  check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "a pair for the flags");
  check(recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN,
        "MSG_DONTWAIT on a blocking socket");
  check(write(sv[0], "peekaboo", 8) == 8 && recv(sv[1], buf, 4, MSG_PEEK) == 4 &&
            memcmp(buf, "peek", 4) == 0,
        "MSG_PEEK sees it");
  check(recv(sv[1], buf, sizeof(buf), 0) == 8 && memcmp(buf, "peekaboo", 8) == 0,
        "and leaves it");
  if ((pid = fork()) == 0) {
    write(sv[0], "half", 4);
    usleep(200000);
    write(sv[0], "whole", 5);
    _exit(0);
  }
  check(recv(sv[1], buf, 9, MSG_WAITALL) == 9 && memcmp(buf, "halfwhole", 9) == 0,
        "MSG_WAITALL waits for all of it");
  waitpid(pid, &status, 0);
  close(sv[1]);
  signal(SIGPIPE, SIG_DFL);
  if ((pid = fork()) == 0)
    _exit(send(sv[0], "x", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE ? 0 : 1);
  check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "MSG_NOSIGNAL is EPIPE and no signal");
  signal(SIGPIPE, SIG_IGN);
  close(sv[0]);

  check(socketpair(AF_UNIX, SOCK_DGRAM, 0, dv) == 0 && send(dv[0], "datagram", 8, 0) == 8,
        "a datagram for the flags");
  check(recv(dv[1], buf, 4, MSG_PEEK) == 4, "peeked at");
  {
    struct msghdr msg;
    struct iovec iov = {buf, 4};

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    check(recvmsg(dv[1], &msg, 0) == 4 && (msg.msg_flags & MSG_TRUNC) &&
              memcmp(buf, "data", 4) == 0,
          "cut short, MSG_TRUNC says so");
  }
  close(dv[0]);
  close(dv[1]);
}

int main(void) {
  pair();
  stream();
  dgram();
  rights();
  flags();
  printf("unixtest: %s\n", bad ? "FAILED" : "ok");
  return bad;
}
