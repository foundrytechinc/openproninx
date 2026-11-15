#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fnu/sys.h>
#include "ssh.h"
#include <err.h>
#include <util.h>

#define WINDOW (1024 * 1024)
#define MAXPACKET 32768

static struct termios saved_term;
static int term_raw;
static char known_hosts[256];
static char hostname[128];
static int port = 22;

static void term_restore(void) {
  if (term_raw)
    ioctl(0, TIOCSETAW, (uint64_t)&saved_term);
  term_raw = 0;
}

static void die(const char *msg) {
  term_restore();
  dprintf(2, "ssh: %s\n", msg);
  exit(255);
}

// KNOWN HOSTS

static int known_host_line(char *line, const char *want, unsigned char pub[32]) {
  char *p = line, *host, *type, *key;

  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == '#' || *p == 0)
    return 0;
  host = p;
  while (*p && *p != ' ' && *p != '\t')
    p++;
  if (*p == 0)
    return 0;
  *p++ = 0;
  while (*p == ' ' || *p == '\t')
    p++;
  type = p;
  while (*p && *p != ' ' && *p != '\t')
    p++;
  if (*p == 0)
    return 0;
  *p++ = 0;
  while (*p == ' ' || *p == '\t')
    p++;
  key = p;
  if (strcmp(host, want) != 0 || strcmp(type, "ssh-ed25519") != 0)
    return 0;
  {
    unsigned char raw[128];
    struct sbuf b;
    int n = ssh_b64_decode(key, raw, sizeof(raw));

    if (n <= 0)
      return 0;
    sb_init(&b);
    b.b = raw;
    b.len = (uint32_t)n;
    return ssh_get_pubkey(&b, pub) == 0 ? 1 : 0;
  }
}

static void host_name_for_file(char *out, int size) {
  if (port == 22)
    strlcpy(out, hostname, size);
  else
    snprintf(out, (size_t)size, "[%s]:%d", hostname, port);
}

static int verify_hostkey(struct ssh *s, const unsigned char pub[32]) {
  char name[160], fp[80], *text = 0, *line, answer[16];
  unsigned char known[32];
  int fd, n, have = 0, len = 0;

  (void)s;
  host_name_for_file(name, sizeof(name));
  ssh_fingerprint(pub, fp, sizeof(fp));
  if ((fd = open(known_hosts, O_RDONLY)) >= 0) {
    if ((text = malloc(65536)) == 0)
      die("out of memory");
    while ((n = (int)read(fd, text + len, 65535 - len)) > 0)
      len += n;
    close(fd);
    text[len] = 0;
    for (line = text; line && *line;) {
      char *next = strchr(line, '\n');
      if (next)
        *next++ = 0;
      if (known_host_line(line, name, known)) {
        have = 1;
        break;
      }
      line = next;
    }
    free(text);
  }
  if (have) {
    if (timingsafe_bcmp(known, pub, 32) == 0)
      return 0;
    term_restore();
    dprintf(2, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
               "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\n"
               "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
               "The ED25519 host key for %s is now %s.\n"
               "Remove the old line from %s if this is expected.\n",
            name, fp, known_hosts);
    return -1;
  }
  if (!isatty(0)) {
    dprintf(2, "ssh: host key for %s (%s) is not known and there is no "
               "terminal to ask on\n", name, fp);
    return -1;
  }
  term_restore();
  printf("The authenticity of host '%s' can't be established.\n"
         "ED25519 key fingerprint is %s.\n"
         "Are you sure you want to continue connecting (yes/no)? ",
         name, fp);
  if (fgets(answer, sizeof(answer), stdin) == 0)
    return -1;
  if (strncmp(answer, "yes", 3) != 0)
    return -1;
  {
    struct sbuf blob;
    char b[128], linebuf[256], dir[256];

    sb_init(&blob);
    ssh_put_pubkey(&blob, pub);
    ssh_b64_encode(blob.b, blob.len, b, sizeof(b));
    sb_free(&blob);
    n = snprintf(linebuf, sizeof(linebuf), "%s ssh-ed25519 %s\n", name, b);
    strlcpy(dir, known_hosts, sizeof(dir));
    mkdir(dirname(dir), 0777);
    if ((fd = open(known_hosts, O_WRONLY | O_CREAT | O_APPEND, 0666)) >= 0) {
      ssh_writen(fd, linebuf, (uint32_t)n);
      close(fd);
      printf("Warning: Permanently added '%s' (ED25519) to the list of known hosts.\n", name);
    }
  }
  return 0;
}

// USERAUTH

static int userauth(struct ssh *s, const char *user, const char *keyfile) {
  struct sbuf p;
  unsigned char pub[32], sec[64];
  char password[128], prompt[320];
  int type, tries, have_key;

  sb_init(&p);
  sb_put_u8(&p, SSH_MSG_SERVICE_REQUEST);
  sb_put_cstring(&p, "ssh-userauth");
  if (ssh_send(s, &p) < 0 || ssh_recv(s, &p) != SSH_MSG_SERVICE_ACCEPT)
    goto fail;

  have_key = ssh_key_read(keyfile, pub, sec) == 0;
  if (have_key) {
    struct sbuf blob, signed_data;
    unsigned char sig[64];
    unsigned long long siglen;

    sb_init(&blob);
    ssh_put_pubkey(&blob, pub);
    sb_init(&signed_data);
    sb_put_string(&signed_data, s->session_id, 32);
    sb_put_u8(&signed_data, SSH_MSG_USERAUTH_REQUEST);
    sb_put_cstring(&signed_data, user);
    sb_put_cstring(&signed_data, "ssh-connection");
    sb_put_cstring(&signed_data, "publickey");
    sb_put_u8(&signed_data, 1);
    sb_put_cstring(&signed_data, "ssh-ed25519");
    sb_put_sbuf(&signed_data, &blob);
    crypto_sign_ed25519_detached(sig, &siglen, signed_data.b, signed_data.len, sec);
    explicit_bzero(sec, sizeof(sec));

    sb_reset(&p);
    sb_put_u8(&p, SSH_MSG_USERAUTH_REQUEST);
    sb_put_cstring(&p, user);
    sb_put_cstring(&p, "ssh-connection");
    sb_put_cstring(&p, "publickey");
    sb_put_u8(&p, 1);
    sb_put_cstring(&p, "ssh-ed25519");
    sb_put_sbuf(&p, &blob);
    {
      struct sbuf sb;
      sb_init(&sb);
      ssh_put_sig(&sb, sig);
      sb_put_sbuf(&p, &sb);
      sb_free(&sb);
    }
    sb_free(&blob);
    sb_free(&signed_data);
    if (ssh_send(s, &p) < 0)
      goto fail;
    for (;;) {
      type = ssh_recv(s, &p);
      if (type == SSH_MSG_USERAUTH_SUCCESS)
        goto ok;
      if (type == SSH_MSG_USERAUTH_BANNER)
        continue;
      if (type == SSH_MSG_USERAUTH_FAILURE)
        break;
      goto fail;
    }
    ssh_log(s, "the key in %s was not accepted", keyfile);
  }

  for (tries = 0; tries < 3; tries++) {
    term_restore();
    snprintf(prompt, sizeof(prompt), "%s@%s's password: ", user, hostname);
    if (read_password(prompt, password, sizeof(password)) < 0)
      goto fail;
    sb_reset(&p);
    sb_put_u8(&p, SSH_MSG_USERAUTH_REQUEST);
    sb_put_cstring(&p, user);
    sb_put_cstring(&p, "ssh-connection");
    sb_put_cstring(&p, "password");
    sb_put_u8(&p, 0);
    sb_put_cstring(&p, password);
    explicit_bzero(password, sizeof(password));
    if (ssh_send(s, &p) < 0)
      goto fail;
    for (;;) {
      type = ssh_recv(s, &p);
      if (type == SSH_MSG_USERAUTH_SUCCESS)
        goto ok;
      if (type == SSH_MSG_USERAUTH_BANNER) {
        uint32_t n;
        const unsigned char *b;
        sb_get_u8(&p);
        if ((b = sb_get_string(&p, &n)) != 0)
          write(2, b, n);
        continue;
      }
      if (type == SSH_MSG_USERAUTH_FAILURE)
        break;
      goto fail;
    }
    printf("Permission denied, please try again.\n");
  }
fail:
  sb_free(&p);
  return -1;
ok:
  sb_free(&p);
  return 0;
}

// THE SESSION

static uint32_t rchan, rwin, rmax, lwin;
static int got_eof, got_close, exit_status = 255, sent_eof;

static int chan_u8(struct ssh *s, int type) {
  struct sbuf p;
  int r;

  sb_init(&p);
  sb_put_u8(&p, type);
  sb_put_u32(&p, rchan);
  r = ssh_send(s, &p);
  sb_free(&p);
  return r;
}

static int chan_request(struct ssh *s, const char *kind, struct sbuf *body, int reply) {
  struct sbuf p;
  int r;

  sb_init(&p);
  sb_put_u8(&p, SSH_MSG_CHANNEL_REQUEST);
  sb_put_u32(&p, rchan);
  sb_put_cstring(&p, kind);
  sb_put_u8(&p, reply);
  if (body)
    sb_put(&p, body->b, body->len);
  r = ssh_send(s, &p);
  sb_free(&p);
  return r;
}

static int handle_packet(struct ssh *s, int type, struct sbuf *p) {
  uint32_t n, kind;
  const unsigned char *d;

  switch (type) {
  case SSH_MSG_CHANNEL_DATA:
    sb_get_u32(p);
    if ((d = sb_get_string(p, &n)) == 0 || n > lwin)
      return -1;
    lwin -= n;
    ssh_writen(1, d, n);
    break;
  case SSH_MSG_CHANNEL_EXTENDED_DATA:
    sb_get_u32(p);
    kind = sb_get_u32(p);
    if ((d = sb_get_string(p, &n)) == 0 || n > lwin)
      return -1;
    lwin -= n;
    ssh_writen(kind == 1 ? 2 : 1, d, n);
    break;
  case SSH_MSG_CHANNEL_WINDOW_ADJUST:
    sb_get_u32(p);
    rwin += sb_get_u32(p);
    return 0;
  case SSH_MSG_CHANNEL_REQUEST: {
    char req[32];
    int want;

    sb_get_u32(p);
    if (sb_get_cstring(p, req, sizeof(req)) < 0)
      return -1;
    want = sb_get_u8(p);
    if (strcmp(req, "exit-status") == 0) {
      exit_status = (int)sb_get_u32(p);
      ssh_log(s, "exit status %d", exit_status);
    }
    else if (strcmp(req, "exit-signal") == 0)
      exit_status = 128;
    if (want)
      return chan_u8(s, SSH_MSG_CHANNEL_FAILURE);
    return 0;
  }
  case SSH_MSG_CHANNEL_EOF:
    got_eof = 1;
    return 0;
  case SSH_MSG_CHANNEL_CLOSE:
    got_close = 1;
    return 0;
  case SSH_MSG_GLOBAL_REQUEST: {
    char req[64];
    if (sb_get_cstring(p, req, sizeof(req)) < 0)
      return -1;
    if (sb_get_u8(p))
      return ssh_send_u8(s, SSH_MSG_REQUEST_FAILURE);
    return 0;
  }
  case SSH_MSG_CHANNEL_OPEN: {
    struct sbuf r;
    char what[32];
    uint32_t sender;

    if (sb_get_cstring(p, what, sizeof(what)) < 0)
      return -1;
    sender = sb_get_u32(p);
    sb_init(&r);
    sb_put_u8(&r, SSH_MSG_CHANNEL_OPEN_FAILURE);
    sb_put_u32(&r, sender);
    sb_put_u32(&r, 1);
    sb_put_cstring(&r, "no");
    sb_put_cstring(&r, "");
    n = (uint32_t)ssh_send(s, &r);
    sb_free(&r);
    return (int)n;
  }
  default:
    return 0;
  }
  if (lwin < WINDOW / 2) {
    struct sbuf a;
    int r;

    sb_init(&a);
    sb_put_u8(&a, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    sb_put_u32(&a, rchan);
    sb_put_u32(&a, WINDOW - lwin);
    r = ssh_send(s, &a);
    sb_free(&a);
    lwin = WINDOW;
    return r;
  }
  return 0;
}

static int request_reply(struct ssh *s, struct sbuf *p) {
  int type;

  for (;;) {
    if ((type = ssh_recv(s, p)) < 0)
      return -1;
    sb_get_u8(p);
    if (type == SSH_MSG_CHANNEL_SUCCESS || type == SSH_MSG_CHANNEL_FAILURE)
      return type;
    if (handle_packet(s, type, p) < 0)
      return -1;
  }
}

static int session(struct ssh *s, const char *command, int want_tty) {
  struct sbuf p;
  static char buf[MAXPACKET];
  int type, n, escape = 1, at_line_start = 1;

  sb_init(&p);
  sb_put_u8(&p, SSH_MSG_CHANNEL_OPEN);
  sb_put_cstring(&p, "session");
  sb_put_u32(&p, 0);
  sb_put_u32(&p, WINDOW);
  sb_put_u32(&p, MAXPACKET);
  lwin = WINDOW;
  if (ssh_send(s, &p) < 0)
    die("cannot open a session");
  for (;;) {
    type = ssh_recv(s, &p);
    if (type == SSH_MSG_CHANNEL_OPEN_CONFIRMATION)
      break;
    if (type == SSH_MSG_CHANNEL_OPEN_FAILURE || type < 0)
      die("the server refused a session");
    sb_get_u8(&p);
    if (handle_packet(s, type, &p) < 0)
      die("protocol error");
  }
  sb_get_u8(&p);
  sb_get_u32(&p);
  rchan = sb_get_u32(&p);
  rwin = sb_get_u32(&p);
  rmax = sb_get_u32(&p);
  if (rmax > MAXPACKET)
    rmax = MAXPACKET;

  if (want_tty) {
    struct sbuf body;
    struct winsize ws;
    const char *term = getenv("TERM");

    memset(&ws, 0, sizeof(ws));
    ioctl(0, TIOCGWINSZ, (uint64_t)&ws);
    sb_init(&body);
    sb_put_cstring(&body, term ? term : "vt100");
    sb_put_u32(&body, ws.ws_col);
    sb_put_u32(&body, ws.ws_row);
    sb_put_u32(&body, ws.ws_xpixel);
    sb_put_u32(&body, ws.ws_ypixel);
    ssh_put_ttymodes(&body, &saved_term);
    if (chan_request(s, "pty-req", &body, 1) < 0)
      die("cannot ask for a terminal");
    sb_free(&body);
    if (request_reply(s, &p) != SSH_MSG_CHANNEL_SUCCESS)
      die("the server would not give a terminal");
  }
  if (command != 0) {
    struct sbuf body;
    sb_init(&body);
    sb_put_cstring(&body, command);
    if (chan_request(s, "exec", &body, 1) < 0)
      die("cannot send the command");
    sb_free(&body);
  } else if (chan_request(s, "shell", 0, 1) < 0)
    die("cannot ask for a shell");
  if (request_reply(s, &p) != SSH_MSG_CHANNEL_SUCCESS)
    die("the server refused to run it");

  if (want_tty) {
    struct termios raw = saved_term;

    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(ICRNL | IXON | INLCR | IGNCR | ISTRIP);
    raw.c_oflag &= ~OPOST;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    ioctl(0, TIOCSETAW, (uint64_t)&raw);
    term_raw = 1;
  }

  while (!got_close) {
    fd_set rd;
    struct timeval zero = {0, 0};
    int nfds = s->fd + 1, pend = ssh_pending(s);

    FD_ZERO(&rd);
    FD_SET(s->fd, &rd);
    int stdin_open = !sent_eof;

    if (stdin_open && rwin > 0)
      FD_SET(0, &rd);
    if (select(nfds, &rd, 0, 0, pend ? &zero : 0) < 0)
      break;
    if (pend)
      FD_SET(s->fd, &rd);
    if (FD_ISSET(s->fd, &rd)) {
      if ((type = ssh_recv(s, &p)) < 0)
        break;
      sb_get_u8(&p);
      if (handle_packet(s, type, &p) < 0)
        break;
    }
    if (FD_ISSET(0, &rd) && stdin_open) {
      uint32_t can = rwin < sizeof(buf) ? rwin : sizeof(buf);
      if (can > rmax)
        can = rmax;
      n = (int)read(0, buf, can);
      if (n <= 0) {
        sent_eof = 1;
        chan_u8(s, SSH_MSG_CHANNEL_EOF);
        continue;
      }
      if (want_tty && escape) {
        int i, out = 0;
        for (i = 0; i < n; i++) {
          if (at_line_start && buf[i] == '~' && i + 1 < n && buf[i + 1] == '.') {
            term_restore();
            dprintf(2, "\nConnection to %s closed.\n", hostname);
            exit(exit_status);
          }
          at_line_start = buf[i] == '\r' || buf[i] == '\n';
          buf[out++] = buf[i];
        }
        n = out;
      }
      sb_reset(&p);
      sb_put_u8(&p, SSH_MSG_CHANNEL_DATA);
      sb_put_u32(&p, rchan);
      sb_put_string(&p, buf, (uint32_t)n);
      if (ssh_send(s, &p) < 0)
        break;
      rwin -= (uint32_t)n;
    }
  }
  chan_u8(s, SSH_MSG_CHANNEL_CLOSE);
  sb_free(&p);
  term_restore();
  return exit_status;
}

static void usage(void) {
  dprintf(2, "usage: ssh [-p port] [-i keyfile] [-l user] [-t] [-T] [user@]host [command ...]\n");
  exit(255);
}

int main(int argc, char **argv) {
  struct ssh s;
  struct sockaddr_in to;
  struct in_addr addr;
  char command[1024], keyfile[256], user[64], *at;
  const char *login = getenv("USER");
  int i, fd, want_tty = -1, r;

  keyfile[0] = 0;
  user[0] = 0;
  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
      port = atoi(argv[++i]);
    else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
      strlcpy(keyfile, argv[++i], sizeof(keyfile));
    else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
      strlcpy(user, argv[++i], sizeof(user));
    else if (strcmp(argv[i], "-t") == 0)
      want_tty = 1;
    else if (strcmp(argv[i], "-T") == 0)
      want_tty = 0;
    else if (strcmp(argv[i], "-v") == 0)
      setenv("SSH_DEBUG", "1", 1);
    else
      usage();
  }
  if (i >= argc)
    usage();
  strlcpy(hostname, argv[i++], sizeof(hostname));
  if ((at = strchr(hostname, '@')) != 0) {
    *at = 0;
    strlcpy(user, hostname, sizeof(user));
    memmove(hostname, at + 1, strlen(at + 1) + 1);
  }
  if (user[0] == 0)
    strlcpy(user, login ? (char *)login : "root", sizeof(user));
  command[0] = 0;
  for (; i < argc; i++) {
    if (command[0])
      strlcpy(command + strlen(command), " ", (int)(sizeof(command) - strlen(command)));
    strlcpy(command + strlen(command), argv[i], (int)(sizeof(command) - strlen(command)));
  }
  if (want_tty < 0)
    want_tty = command[0] == 0 && isatty(0);
  if (want_tty && ioctl(0, TIOCGETA, (uint64_t)&saved_term) < 0)
    want_tty = 0;
  if (!want_tty)
    memset(&saved_term, 0, sizeof(saved_term));
  if (keyfile[0] == 0)
    ssh_user_path(keyfile, sizeof(keyfile), "id_ed25519");
  ssh_user_path(known_hosts, sizeof(known_hosts), "known_hosts");

  if ((r = fnu_resolve(hostname, (uint8 *)&addr.s_addr, 5000)) < 0)
    errx(1, "%s: %s", hostname, fnu_errstr(r));
  memset(&to, 0, sizeof(to));
  to.sin_len = sizeof(to);
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = addr.s_addr;
  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0 || connect(fd, (struct sockaddr *)&to, sizeof(to)) < 0) {
    warn("connect to host %s port %d", hostname, port);
    exit(255);
  }

  ssh_init(&s, fd, 0);
  s.verify_hostkey = verify_hostkey;
  if (ssh_versions(&s) < 0)
    die("the other end does not speak SSH-2");
  if (ssh_kex(&s) < 0)
    die("key exchange failed");
  if (userauth(&s, user, keyfile) < 0)
    die("permission denied");
  r = session(&s, command[0] ? command : 0, want_tty);
  ssh_disconnect(&s, SSH_DISCONNECT_BY_APPLICATION, "done");
  close(fd);
  exit(r);
}
