#ifndef _UTIL_H_
#define _UTIL_H_

// libutil: helpers shared by programs, neither libc nor system calls
#include <sys/types.h>
#include <stdio.h>

#define HN_DECIMAL 0x01
#define HN_NOSPACE 0x02
#define HN_B 0x04
#define HN_DIVISOR_1000 0x08
#define HN_IEC_PREFIXES 0x10
#define HN_GETSCALE 0x10
#define HN_AUTOSCALE 0x20

int humanize_number(char *, size_t, int64_t, const char *, int, int);
int login_tty(int);
void logwtmp(const char *, const char *, const char *);

#define FMT_SCALED_STRSIZE 7 // minus sign, 4 digits, suffix, null byte
int fmt_scaled(long long, char *);
int scan_scaled(char *, long long *);
struct termios;
struct winsize;
int getptmfd(void);
int openpty(int *, int *, char *, const struct termios *,
            const struct winsize *);
int fdopenpty(int, int *, int *, char *, const struct termios *,
              const struct winsize *);
pid_t forkpty(int *, char *, const struct termios *, const struct winsize *);
pid_t fdforkpty(int, int *, char *, const struct termios *,
                const struct winsize *);

struct passwd;
struct group;
int pwd_store(const struct passwd *entry);
int pwd_replace(const char *name, const struct passwd *entry);
int pwd_remove(const char *name);
int grp_store(const struct group *entry);
int grp_replace(const char *name, const struct group *entry);
int grp_remove(const char *name);
int grp_set_member(const char *user, const char *const *groups);
int grp_rename_member(const char *user, const char *rename);

struct users_conf {
  char home[256];
  char skel[256];
  char shell[256];
  char group[64];
  mode_t mode;
  unsigned uid_lo, uid_hi;
  unsigned gid_lo, gid_hi;
};
int users_conf(struct users_conf *c);
uid_t users_free_uid(const struct users_conf *c, uid_t want);
gid_t users_free_gid(const struct users_conf *c, gid_t want);
int users_skel(const char *skel, const char *home, uid_t uid, gid_t gid);
int users_mkhome(const struct users_conf *c, const char *home, const char *skel,
                 uid_t uid, gid_t gid);
int users_rmtree(const char *path);
char **users_grouplist(const char *s);
int users_name_ok(const char *name);
int pwd_ask_new(char *out, int size);
int read_password(const char *prompt, char *buf, int max);
void pathjoin(char *out, int size, const char *dir, const char *name);
void timecolumn(int64_t secs, int64_t now, char *out);

// pidfile(3): a locked file in /var/run, one daemon of a kind
struct pidfh;
struct pidfh *pidfile_open(const char *path, mode_t mode, pid_t *otherpid);
int pidfile_write(struct pidfh *pfh);
int pidfile_close(struct pidfh *pfh);
int pidfile_remove(struct pidfh *pfh);
int pidfile_fileno(const struct pidfh *pfh);

#endif
