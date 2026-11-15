#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <pwd.h>
#include <grp.h>
#include <fnu/sys.h>
#include <fnu/crypt.h>
#include "ssh.h"
#include <utmp.h>
#include <err.h>
#include <util.h>
#include <limits.h>

#define HOSTKEY "/etc/ssh/ssh_host_ed25519_key"
#define WINDOW (1024 * 1024)
#define MAXPACKET 32768
#define MAXAUTH 6

struct session {
  struct ssh *s;
  struct passwd who;
  uint32_t rchan, lchan;
  uint32_t rwin, lwin;
  uint32_t rmax;
  int open, sent_eof, sent_close, got_close;
  int master, in, out, err;
  int child;
  int pty;
  char term[32];
  struct termios modes;
  struct winsize ws;
  int have_modes;
  char command[1024];
  char line[UT_LINESIZE + 1];
  struct sbuf data; // the packet a read becomes, kept
};

static void hostkey(struct ssh *s) {
  if (ssh_key_read(HOSTKEY, s->hostkey_pub, s->hostkey_sec) == 0)
    return;
  mkdir("/etc/ssh", 0777);
  if (ssh_key_generate(s->hostkey_pub, s->hostkey_sec) < 0 ||
      ssh_key_write(HOSTKEY, s->hostkey_pub, s->hostkey_sec, "root@openproninx") < 0) {
    dprintf(2, "sshd: cannot make a host key\n");
    exit(1);
  }
  {
    char fp[80];
    ssh_fingerprint(s->hostkey_pub, fp, sizeof(fp));
    dprintf(2, "sshd: new host key %s\n", fp);
  }
}

// AUTHENTICATION

static int authorized(const struct passwd *who, const unsigned char pub[32]) {
  char path[256], *text, *line;
  uint32_t len;
  int fd, n, ok = 0;

  snprintf(path, sizeof(path), "%s/.ssh/authorized_keys",
           who->pw_dir[0] ? who->pw_dir : "/");
  if ((fd = open(path, O_RDONLY)) < 0)
    return 0;
  if ((text = malloc(8192)) == 0) {
    close(fd);
    return 0;
  }
  len = 0;
  while ((n = (int)read(fd, text + len, 8191 - len)) > 0)
    len += (uint32_t)n;
  close(fd);
  text[len] = 0;
  for (line = text; line && *line && !ok;) {
    char *next = strchr(line, '\n'), *p = line;
    unsigned char raw[128], key[32];
    struct sbuf b;

    if (next)
      *next++ = 0;
    while (*p == ' ' || *p == '\t')
      p++;
    if (strncmp(p, "ssh-ed25519 ", 12) == 0 &&
        (n = ssh_b64_decode(p + 12, raw, sizeof(raw))) > 0) {
      sb_init(&b);
      b.b = raw;
      b.len = (uint32_t)n;
      if (ssh_get_pubkey(&b, key) == 0 && timingsafe_bcmp(key, pub, 32) == 0)
        ok = 1;
    }
    line = next;
  }
  free(text);
  return ok;
}

static int auth_failure(struct ssh *s, int partial) {
  struct sbuf p;
  int r;

  sb_init(&p);
  sb_put_u8(&p, SSH_MSG_USERAUTH_FAILURE);
  sb_put_cstring(&p, "publickey,password");
  sb_put_u8(&p, partial);
  r = ssh_send(s, &p);
  sb_free(&p);
  return r;
}

static int userauth(struct ssh *s, struct passwd *who) {
  struct sbuf p;
  char user[LOGIN_NAME_MAX], service[32], method[32];
  int type, tries = 0, known;
  static const char *nobody =
      "$2b$10$liZTtiuUbZgkft3xcdUiDem8dFBFU0/.5ntYNJ5ScDSB7wQqmjRN2";

  sb_init(&p);
  if ((type = ssh_recv(s, &p)) != SSH_MSG_SERVICE_REQUEST)
    goto fail;
  sb_get_u8(&p);
  if (sb_get_cstring(&p, service, sizeof(service)) < 0 ||
      strcmp(service, "ssh-userauth") != 0)
    goto fail;
  sb_reset(&p);
  sb_put_u8(&p, SSH_MSG_SERVICE_ACCEPT);
  sb_put_cstring(&p, "ssh-userauth");
  if (ssh_send(s, &p) < 0)
    goto fail;

  for (;;) {
    if ((type = ssh_recv(s, &p)) != SSH_MSG_USERAUTH_REQUEST)
      goto fail;
    sb_get_u8(&p);
    if (sb_get_cstring(&p, user, sizeof(user)) < 0 ||
        sb_get_cstring(&p, service, sizeof(service)) < 0 ||
        sb_get_cstring(&p, method, sizeof(method)) < 0 ||
        strcmp(service, "ssh-connection") != 0)
      goto fail;
    {
      struct passwd *found = getpwnam(user);

      known = found != 0;
      if (known)
        *who = *found;
    }
    if (known && who->pw_passwd[0] == '*')
      known = 0;
    ssh_log(s, "userauth %s for %s%s", method, user, known ? "" : " (no such account)");

    if (strcmp(method, "password") == 0) {
      char password[CRYPT_PASSWORD_MAX];
      const unsigned char *pw;
      uint32_t n;
      int ok;

      sb_get_u8(&p);
      if ((pw = sb_get_string(&p, &n)) == 0 || n >= sizeof(password))
        goto fail;
      memcpy(password, pw, n);
      password[n] = 0;
      ok = crypt_checkpass(password, known ? who->pw_passwd : nobody) == 0 && known;
      explicit_bzero(password, sizeof(password));
      if (ok) {
        syslog(LOG_AUTH | LOG_INFO, "Accepted password for %s from %s", user, s->peer);
        sb_free(&p);
        return ssh_send_u8(s, SSH_MSG_USERAUTH_SUCCESS);
      }
      syslog(LOG_AUTH | LOG_NOTICE, "Failed password for %s%s from %s", known ? "" : "invalid user ",
             user, s->peer);
      sleep(1);
    } else if (strcmp(method, "publickey") == 0) {
      const unsigned char *blob, *sigp;
      uint32_t bn, sn;
      unsigned char pub[32], sig[64];
      char alg[32];
      struct sbuf b, signed_data;
      int has_sig, ok = 0;

      has_sig = sb_get_u8(&p);
      if (sb_get_cstring(&p, alg, sizeof(alg)) < 0 ||
          (blob = sb_get_string(&p, &bn)) == 0)
        goto fail;
      sb_init(&b);
      b.b = (unsigned char *)blob;
      b.len = bn;
      if (strcmp(alg, "ssh-ed25519") == 0 && ssh_get_pubkey(&b, pub) == 0 &&
          known && authorized(who, pub))
        ok = 1;
      if (ok && !has_sig) {
        sb_reset(&p);
        sb_put_u8(&p, SSH_MSG_USERAUTH_PK_OK);
        sb_put_cstring(&p, alg);
        sb_put_string(&p, blob, bn);
        if (ssh_send(s, &p) < 0)
          goto fail;
        continue;
      }
      if (ok) {
        if ((sigp = sb_get_string(&p, &sn)) == 0)
          goto fail;
        sb_init(&b);
        b.b = (unsigned char *)sigp;
        b.len = sn;
        sb_init(&signed_data);
        sb_put_string(&signed_data, s->session_id, 32);
        sb_put_u8(&signed_data, SSH_MSG_USERAUTH_REQUEST);
        sb_put_cstring(&signed_data, user);
        sb_put_cstring(&signed_data, service);
        sb_put_cstring(&signed_data, "publickey");
        sb_put_u8(&signed_data, 1);
        sb_put_cstring(&signed_data, alg);
        sb_put_string(&signed_data, blob, bn);
        if (ssh_get_sig(&b, sig) == 0 &&
            crypto_sign_ed25519_verify_detached(sig, signed_data.b,
                                                signed_data.len, pub) == 0) {
          sb_free(&signed_data);
          sb_free(&p);
          return ssh_send_u8(s, SSH_MSG_USERAUTH_SUCCESS);
        }
        sb_free(&signed_data);
      }
    }
    if (++tries >= MAXAUTH)
      goto fail;
    if (auth_failure(s, 0) < 0)
      goto fail;
  }
fail:
  sb_free(&p);
  return -1;
}

// THE SESSION

static int send_window_adjust(struct session *se, uint32_t n) {
  struct sbuf p;
  int r;

  sb_init(&p);
  sb_put_u8(&p, SSH_MSG_CHANNEL_WINDOW_ADJUST);
  sb_put_u32(&p, se->rchan);
  sb_put_u32(&p, n);
  r = ssh_send(se->s, &p);
  sb_free(&p);
  return r;
}

static int send_data(struct session *se, int type, const void *buf, uint32_t n) {
  struct sbuf *p = &se->data;
  int r;

  sb_reset(p);
  sb_put_u8(p, type);
  sb_put_u32(p, se->rchan);
  if (type == SSH_MSG_CHANNEL_EXTENDED_DATA)
    sb_put_u32(p, 1);
  sb_put_string(p, buf, n);
  r = ssh_send(se->s, p);
  se->rwin -= n;
  return r;
}

static int send_chan_u8(struct session *se, int type) {
  struct sbuf p;
  int r;

  sb_init(&p);
  sb_put_u8(&p, type);
  sb_put_u32(&p, se->rchan);
  r = ssh_send(se->s, &p);
  sb_free(&p);
  return r;
}

static int send_exit_status(struct session *se, int status) {
  struct sbuf p;
  int r;

  sb_init(&p);
  sb_put_u8(&p, SSH_MSG_CHANNEL_REQUEST);
  sb_put_u32(&p, se->rchan);
  sb_put_cstring(&p, "exit-status");
  sb_put_u8(&p, 0);
  sb_put_u32(&p, (uint32_t)status);
  r = ssh_send(se->s, &p);
  sb_free(&p);
  return r;
}

static void child_env(struct session *se) {
  environ = 0;
  setenv("USER", se->who.pw_name, 1);
  setenv("LOGNAME", se->who.pw_name, 1);
  setenv("HOME", se->who.pw_dir[0] ? se->who.pw_dir : "/", 1);
  setenv("SHELL", se->who.pw_shell[0] ? se->who.pw_shell : "/usr/bin/sh", 1);
  setenv("PATH", "/usr/bin:/bin", 1);
  setenv("TERM", se->term[0] ? se->term : "vt100", 1);
  setenv("SSH_CONNECTION", se->s->peer, 1);
}

static int start_child(struct session *se) {
  int slave = -1, pin[2], pout[2], perr[2];
  char *shell = se->who.pw_shell[0] ? se->who.pw_shell : "/usr/bin/sh";
  char name0[64];
  char *argv[4];

  if (se->pty) {
    char host[64], *name;

    if (openpty(&se->master, &slave, 0, se->have_modes ? &se->modes : 0,
                &se->ws) < 0)
      return -1;
    if ((name = ttyname(slave)) != 0) {
      strlcpy(se->line, strncmp(name, "/dev/", 5) == 0 ? name + 5 : name,
              sizeof(se->line));
      strlcpy(host, se->s->peer, sizeof(host));
      host[strcspn(host, " ")] = 0;
      logwtmp(se->line, se->who.pw_name, host);
    }
  } else {
    if (pipe(pin) < 0 || pipe(pout) < 0 || pipe(perr) < 0)
      return -1;
  }

  if ((se->child = fork()) < 0)
    return -1;
  if (se->child == 0) {
    close(se->s->fd);
    if (se->pty) {
      close(se->master);
      if (login_tty(slave) < 0)
        exit(1);
    } else {
      dup2(pin[0], 0);
      dup2(pout[1], 1);
      dup2(perr[1], 2);
      close(pin[0]);
      close(pin[1]);
      close(pout[0]);
      close(pout[1]);
      close(perr[0]);
      close(perr[1]);
      setsid();
    }
    if (setlogin(se->who.pw_name) < 0)
      warn("setlogin");
    if (initgroups(se->who.pw_name, se->who.pw_gid) < 0 || setgid(se->who.pw_gid) < 0 ||
        setuid(se->who.pw_uid) < 0) {
      warn("%s", se->who.pw_name);
      exit(1);
    }
    child_env(se);
    if (se->who.pw_dir[0] && chdir(se->who.pw_dir) < 0)
      chdir("/");
    if (se->command[0]) {
      argv[0] = shell;
      argv[1] = "-c";
      argv[2] = se->command;
      argv[3] = 0;
    } else {
      snprintf(name0, sizeof(name0), "-%s", strrchr(shell, '/') ? strrchr(shell, '/') + 1 : shell);
      argv[0] = name0;
      argv[1] = 0;
    }
    execv(shell, argv);
    warn("%s", shell);
    exit(127);
  }
  if (se->pty) {
    close(slave);
    se->in = se->out = se->master;
    se->err = -1;
  } else {
    close(pin[0]);
    close(pout[1]);
    close(perr[1]);
    se->in = pin[1];
    se->out = pout[0];
    se->err = perr[0];
  }
  return 0;
}

static int handle_request(struct session *se, struct sbuf *p) {
  char kind[32];
  int want_reply, ok = 0;

  if (sb_get_cstring(p, kind, sizeof(kind)) < 0)
    return -1;
  want_reply = sb_get_u8(p);
  ssh_log(se->s, "channel request %s", kind);
  if (strcmp(kind, "pty-req") == 0) {
    const unsigned char *modes;
    uint32_t n;

    if (sb_get_cstring(p, se->term, sizeof(se->term)) < 0)
      se->term[0] = 0;
    se->ws.ws_col = (uint16)sb_get_u32(p);
    se->ws.ws_row = (uint16)sb_get_u32(p);
    se->ws.ws_xpixel = (uint16)sb_get_u32(p);
    se->ws.ws_ypixel = (uint16)sb_get_u32(p);
    if ((modes = sb_get_string(p, &n)) != 0) {
      int fd = open("/dev/console", O_RDONLY);
      if (fd >= 0) {
        ioctl(fd, TIOCGETA, (uint64_t)&se->modes);
        close(fd);
        ssh_get_ttymodes(modes, n, &se->modes);
        se->have_modes = 1;
      }
    }
    se->pty = 1;
    ok = 1;
  } else if (strcmp(kind, "env") == 0) {
    ok = 1;
  } else if (strcmp(kind, "shell") == 0 || strcmp(kind, "exec") == 0) {
    if (se->child == 0) {
      if (kind[0] == 'e' && sb_get_cstring(p, se->command, sizeof(se->command)) < 0)
        return -1;
      ok = start_child(se) == 0;
    }
  } else if (strcmp(kind, "window-change") == 0) {
    se->ws.ws_col = (uint16)sb_get_u32(p);
    se->ws.ws_row = (uint16)sb_get_u32(p);
    se->ws.ws_xpixel = (uint16)sb_get_u32(p);
    se->ws.ws_ypixel = (uint16)sb_get_u32(p);
    if (se->master >= 0)
      ioctl(se->master, TIOCSWINSZ, (uint64_t)&se->ws);
    ok = 1;
  } else if (strcmp(kind, "signal") == 0) {
    char name[16];
    if (sb_get_cstring(p, name, sizeof(name)) == 0 && se->child > 0) {
      int sig = strcmp(name, "INT") == 0 ? SIGINT : strcmp(name, "TERM") == 0 ? SIGTERM
                : strcmp(name, "KILL") == 0 ? SIGKILL : strcmp(name, "HUP") == 0 ? SIGHUP : 0;
      if (sig)
        kill(-se->child, sig);
    }
    ok = 1;
  }
  if (want_reply)
    return send_chan_u8(se, ok ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE);
  return 0;
}

static int handle_packet(struct session *se, int type, struct sbuf *p) {
  uint32_t n;
  const unsigned char *d;

  if (type >= SSH_MSG_CHANNEL_WINDOW_ADJUST && type <= SSH_MSG_CHANNEL_FAILURE &&
      sb_get_u32(p) != se->lchan)
    return -1;
  switch (type) {
  case SSH_MSG_CHANNEL_DATA:
    if ((d = sb_get_string(p, &n)) == 0 || n > se->lwin)
      return -1;
    se->lwin -= n;
    if (se->in >= 0 && n > 0 && ssh_writen(se->in, d, n) < 0) {
      close(se->in);
      se->in = -1;
    }
    if (se->lwin < WINDOW / 2) {
      if (send_window_adjust(se, WINDOW - se->lwin) < 0)
        return -1;
      se->lwin = WINDOW;
    }
    return 0;
  case SSH_MSG_CHANNEL_EXTENDED_DATA:
    return 0;
  case SSH_MSG_CHANNEL_WINDOW_ADJUST:
    se->rwin += sb_get_u32(p);
    return 0;
  case SSH_MSG_CHANNEL_REQUEST:
    return handle_request(se, p);
  case SSH_MSG_CHANNEL_EOF:
    if (se->in >= 0) {
      if (se->pty) {
        unsigned char eof = se->have_modes ? se->modes.c_cc[VEOF] : 4;
        write(se->in, &eof, 1);
      } else {
        close(se->in);
        se->in = -1;
      }
    }
    return 0;
  case SSH_MSG_CHANNEL_CLOSE:
    se->got_close = 1;
    if (!se->sent_close) {
      se->sent_close = 1;
      send_chan_u8(se, SSH_MSG_CHANNEL_CLOSE);
    }
    return 0;
  case SSH_MSG_GLOBAL_REQUEST: {
    char kind[64];
    int want;
    if (sb_get_cstring(p, kind, sizeof(kind)) < 0)
      return -1;
    want = sb_get_u8(p);
    if (want)
      return ssh_send_u8(se->s, SSH_MSG_REQUEST_FAILURE);
    return 0;
  }
  case SSH_MSG_CHANNEL_OPEN: {
    struct sbuf r;
    char kind[32];
    uint32_t sender;

    if (sb_get_cstring(p, kind, sizeof(kind)) < 0)
      return -1;
    sender = sb_get_u32(p);
    sb_init(&r);
    sb_put_u8(&r, SSH_MSG_CHANNEL_OPEN_FAILURE);
    sb_put_u32(&r, sender);
    sb_put_u32(&r, 4);
    sb_put_cstring(&r, "one session per connection");
    sb_put_cstring(&r, "");
    n = (uint32_t)ssh_send(se->s, &r);
    sb_free(&r);
    return (int)n;
  }
  default:
    return 0;
  }
}

static int session_open(struct session *se) {
  struct sbuf p;
  char kind[32];
  int type;

  sb_init(&p);
  for (;;) {
    if ((type = ssh_recv(se->s, &p)) < 0)
      break;
    sb_get_u8(&p);
    if (type != SSH_MSG_CHANNEL_OPEN) {
      if (type == SSH_MSG_GLOBAL_REQUEST && handle_packet(se, type, &p) < 0)
        break;
      continue;
    }
    if (sb_get_cstring(&p, kind, sizeof(kind)) < 0)
      break;
    se->rchan = sb_get_u32(&p);
    se->rwin = sb_get_u32(&p);
    se->rmax = sb_get_u32(&p);
    if (se->rmax > MAXPACKET)
      se->rmax = MAXPACKET;
    sb_reset(&p);
    if (strcmp(kind, "session") != 0) {
      sb_put_u8(&p, SSH_MSG_CHANNEL_OPEN_FAILURE);
      sb_put_u32(&p, se->rchan);
      sb_put_u32(&p, 3);
      sb_put_cstring(&p, "only a session");
      sb_put_cstring(&p, "");
      if (ssh_send(se->s, &p) < 0)
        break;
      continue;
    }
    se->lchan = 0;
    se->lwin = WINDOW;
    sb_put_u8(&p, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    sb_put_u32(&p, se->rchan);
    sb_put_u32(&p, se->lchan);
    sb_put_u32(&p, se->lwin);
    sb_put_u32(&p, MAXPACKET);
    if (ssh_send(se->s, &p) < 0)
      break;
    se->open = 1;
    sb_free(&p);
    return 0;
  }
  sb_free(&p);
  return -1;
}

static void session_run(struct session *se) {
  static char buf[MAXPACKET];
  struct sbuf p;
  int n, type, status = 0, reaped = 0;

  sb_init(&p);
  while (!se->got_close || !se->sent_close) {
    fd_set rd;
    struct timeval zero = {0, 0};
    uint32_t can = se->rwin < sizeof(buf) ? se->rwin : sizeof(buf);
    int outdone = se->out < 0 && se->err < 0, pend = ssh_pending(se->s);

    FD_ZERO(&rd);
    FD_SET(se->s->fd, &rd);
    if (se->out >= 0 && can > 0)
      FD_SET(se->out, &rd);
    if (se->err >= 0 && can > 0)
      FD_SET(se->err, &rd);
    if (select(FD_SETSIZE, &rd, 0, 0, pend ? &zero : 0) < 0)
      break;
    if (pend)
      FD_SET(se->s->fd, &rd);
    if (FD_ISSET(se->s->fd, &rd)) {
      if ((type = ssh_recv(se->s, &p)) < 0)
        break;
      sb_get_u8(&p);
      if (handle_packet(se, type, &p) < 0)
        break;
    }
    if (se->out >= 0 && FD_ISSET(se->out, &rd)) {
      n = (int)read(se->out, buf, can > se->rmax ? se->rmax : can);
      if (n > 0) {
        if (send_data(se, SSH_MSG_CHANNEL_DATA, buf, (uint32_t)n) < 0)
          break;
      } else {
        close(se->out);
        if (se->in == se->out)
          se->in = -1;
        se->out = -1;
      }
    }
    if (se->err >= 0 && FD_ISSET(se->err, &rd)) {
      n = (int)read(se->err, buf, can > se->rmax ? se->rmax : can);
      if (n > 0) {
        if (send_data(se, SSH_MSG_CHANNEL_EXTENDED_DATA, buf, (uint32_t)n) < 0)
          break;
      } else {
        close(se->err);
        se->err = -1;
      }
    }
    if (!outdone && se->out < 0 && se->err < 0 && !se->sent_eof) {
      int ei;

      if (se->child > 0 && waitpid(se->child, &ei, 0) == se->child) {
        status = WIFSIGNALED(ei) ? 128 + WTERMSIG(ei) : WEXITSTATUS(ei);
        reaped = 1;
      }
      send_exit_status(se, status);
      se->sent_eof = 1;
      if (send_chan_u8(se, SSH_MSG_CHANNEL_EOF) < 0)
        break;
      if (!se->sent_close) {
        se->sent_close = 1;
        if (send_chan_u8(se, SSH_MSG_CHANNEL_CLOSE) < 0)
          break;
      }
    }
  }
  sb_free(&p);
  sb_free(&se->data);
  if (se->in >= 0)
    close(se->in);
  if (se->out >= 0 && se->out != se->in)
    close(se->out);
  if (se->err >= 0)
    close(se->err);
  if (se->child > 0 && !reaped) {
    kill(-se->child, SIGHUP);
    waitpid(se->child, 0, 0);
  }
  if (se->line[0] != 0)
    logwtmp(se->line, "", "");
}

static void serve(int fd, const char *peer) {
  struct ssh s;
  struct session se;

  ssh_init(&s, fd, 1);
  strlcpy(s.peer, (char *)peer, sizeof(s.peer));
  hostkey(&s);
  memset(&se, 0, sizeof(se));
  se.s = &s;
  se.master = se.in = se.out = se.err = -1;
  if (ssh_versions(&s) < 0 || ssh_kex(&s) < 0) {
    ssh_log(&s, "key exchange failed with %s", peer);
    return;
  }
  if (userauth(&s, &se.who) < 0) {
    ssh_log(&s, "authentication failed from %s", peer);
    syslog(LOG_AUTH | LOG_NOTICE, "too many authentication failures from %s", peer);
    ssh_disconnect(&s, SSH_DISCONNECT_BY_APPLICATION, "too many authentication failures");
    return;
  }
  dprintf(2, "sshd: %s logged in from %s\n", se.who.pw_name, peer);
  if (session_open(&se) == 0)
    session_run(&se);
  ssh_disconnect(&s, SSH_DISCONNECT_BY_APPLICATION, "session ended");
}

int main(int argc, char **argv) {
  struct sockaddr_in me, from;
  socklen_t fromlen = sizeof(struct sockaddr_in);
  int ei;
  int port = 22, s, c, i, once = 0;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
      port = atoi(argv[++i]);
    else if (strcmp(argv[i], "-d") == 0) {
      setenv("SSH_DEBUG", "1", 1);
      once = 1;
    } else {
      dprintf(2, "usage: sshd [-d] [-p port]\n");
      exit(1);
    }
  }
  if (getuid() != 0) {
    dprintf(2, "sshd: must be root\n");
    exit(1);
  }
  {
    struct ssh probe;
    ssh_init(&probe, -1, 1);
    hostkey(&probe);
  }
  memset(&me, 0, sizeof(me));
  me.sin_len = sizeof(me);
  me.sin_family = AF_INET;
  me.sin_port = htons(port);
  me.sin_addr.s_addr = INADDR_ANY;
  if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0 || bind(s, (struct sockaddr *)&me, sizeof(me)) < 0 ||
      listen(s, 8) < 0) {
    warn("port %d", port);
    exit(1);
  }
  dprintf(2, "sshd: listening on port %d\n", port);
  for (;;) {
    char peer[64];

    while (waitpid(-1, &ei, WNOHANG) > 0)
      ;
    if ((c = accept(s, (struct sockaddr *)&from, &fromlen)) < 0)
      continue;
    snprintf(peer, sizeof(peer), "%s %d %d", inet_ntoa(from.sin_addr),
             ntohs(from.sin_port), port);
    if (once) {
      close(s);
      serve(c, peer);
      exit(0);
    }
    if (fork() == 0) {
      close(s);
      serve(c, peer);
      exit(0);
    }
    close(c);
  }
}
