#ifndef _PWD_H_
#define _PWD_H_

#include <sys/types.h>
#include "inc/cdefs.h"

#define _PATH_PASSWD "/etc/passwd"
#define _PATH_MASTERPASSWD "/etc/master.passwd"
#define _PATH_MASTERPASSWD_TMP "/etc/ptmp"
#define _PATH_PASSWD_TMP "/etc/ptmp.pub"

#define _PASSWORD_LEN 128
#define _PW_NAME_LEN 31

struct passwd {
  char *pw_name;
  char *pw_passwd;
  uid_t pw_uid;
  gid_t pw_gid;
  time_t pw_change;
  char *pw_class;
  char *pw_gecos;
  char *pw_dir;
  char *pw_shell;
  time_t pw_expire;
  int pw_fields;
};

#define _PWF_NAME 0x0001
#define _PWF_PASSWD 0x0002
#define _PWF_UID 0x0004
#define _PWF_GID 0x0008
#define _PWF_CHANGE 0x0010
#define _PWF_CLASS 0x0020
#define _PWF_GECOS 0x0040
#define _PWF_DIR 0x0080
#define _PWF_SHELL 0x0100
#define _PWF_EXPIRE 0x0200

__BEGIN_DECLS
struct passwd *getpwuid(uid_t);
struct passwd *getpwnam(const char *);
#define _PW_BUF_LEN 1024 /* what getpw*_r wants for a buffer */

int uid_from_user(const char *, uid_t *);
const char *user_from_uid(uid_t, int);

int getpwnam_r(const char *, struct passwd *, char *, size_t, struct passwd **);
int getpwuid_r(uid_t, struct passwd *, char *, size_t, struct passwd **);
struct passwd *getpwent(void);
int setpassent(int);
struct passwd *pw_dup(const struct passwd *);
void setpwent(void);
void endpwent(void);

int crypt_newhash(const char *password, const char *pref, char *hash,
                  size_t hashlen);
char *bcrypt_gensalt(uint8_t);
char *bcrypt(const char *, const char *);
int bcrypt_newhash(const char *, int, char *, size_t);
int bcrypt_checkpass(const char *, const char *);
int crypt_checkpass(const char *password, const char *hash);

// the system's own
__END_DECLS

#endif
