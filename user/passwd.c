#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <fnu/sys.h>
#include <fnu/crypt.h>
#include <err.h>
#include <limits.h>
#include <util.h>

int main(int argc, char **argv) {
  char old[CRYPT_PASSWORD_MAX], new[CRYPT_PASSWORD_MAX], hash[_PASSWORD_LEN];
  char pref[16];
  struct passwd *account, *found;
  uid_t me = getuid();

  if (argc > 2) {
    dprintf(2, "usage: passwd [user]\n");
    exit(1);
  }

  if (argc == 2) {
    if ((found = getpwnam(argv[1])) == 0) {
      dprintf(2, "passwd: no account called %s\n", argv[1]);
      exit(1);
    }
  } else if ((found = getpwuid(me)) == 0) {
    dprintf(2, "passwd: no account owns uid %d\n", me);
    exit(1);
  }
  if ((account = pw_dup(found)) == 0)
    err(1, "pw_dup");

  if (me != 0 && me != account->pw_uid) {
    dprintf(2, "passwd: %s is not yours to change\n", account->pw_name);
    exit(1);
  }

  if (me != 0) {
    if (read_password("Old password: ", old, sizeof(old)) < 0 ||
        crypt_checkpass(old, account->pw_passwd) != 0) {
      memset(old, 0, sizeof(old));
      dprintf(2, "passwd: incorrect\n");
      exit(1);
    }
    memset(old, 0, sizeof(old));
  }

  printf("Changing password for %s.\n", account->pw_name);
  if (pwd_ask_new(new, sizeof(new)) < 0)
    exit(1);

  snprintf(pref, sizeof(pref), "bcrypt,%d", CRYPT_COST);
  if (crypt_newhash(new, pref, hash, sizeof(hash)) < 0) {
    memset(new, 0, sizeof(new));
    dprintf(2, "passwd: cannot hash it\n");
    exit(1);
  }
  memset(new, 0, sizeof(new));
  account->pw_passwd = hash;

  if (pwd_store(account) < 0) {
    warn("%s", _PATH_MASTERPASSWD);
    exit(1);
  }
  syslog(LOG_AUTH | LOG_INFO, "password for %s changed by uid %u", account->pw_name,
         (unsigned)me);
  printf("passwd: changed the password for %s\n", account->pw_name);
  exit(0);
}
