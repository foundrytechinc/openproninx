// usertest: groupadd, useradd, usermod, userdel and what they leave in the files
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int failures;

static void ok(const char *what, int good) {
  printf("usertest: %-48s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

static int run(const char *cmd) {
  char line[512];

  snprintf(line, sizeof(line), "%s > /dev/null 2>&1", cmd);
  return system(line);
}

static int member(const char *group, const char *user) {
  struct group *g = getgrnam(group);
  int i;

  for (i = 0; g != 0 && g->gr_mem[i] != 0; i++)
    if (strcmp(g->gr_mem[i], user) == 0)
      return 1;
  return 0;
}

int main(void) {
  struct passwd *pw;
  struct group *g;
  struct stat st;

  if (getuid() != 0) {
    printf("usertest: needs root, skipped\n");
    return 0;
  }
  run("userdel -r _utv");
  run("userdel -r _utu");
  run("groupdel _utg");

  ok("groupadd", run("groupadd _utg") == 0 && getgrnam("_utg") != 0);
  ok("useradd", run("useradd -m -G _utg -c 'User Test' _utu") == 0);
  pw = getpwnam("_utu");
  ok("the account is there", pw != 0);
  if (pw == 0)
    return 1;
  ok("its home is under /home", strcmp(pw->pw_dir, "/home/_utu") == 0);
  g = getgrgid(pw->pw_gid);
  ok("it has a group of its own", g != 0 && strcmp(g->gr_name, "_utu") == 0);
  ok("and is in the group asked for", member("_utg", "_utu"));
  ok("the home is made, its own, mode 0700",
     stat("/home/_utu", &st) == 0 && st.st_uid == pw->pw_uid && (st.st_mode & 07777) == 0700);
  ok("the skeleton went in as dot files", stat("/home/_utu/.profile", &st) == 0 &&
                                              st.st_uid == pw->pw_uid);
  ok("it is locked until passwd", strcmp(getpwnam("_utu")->pw_passwd, "*") == 0);
  ok("a second one by the same name is refused", run("useradd _utu") != 0);

  run("usermod -p '$2b$10$abcdefghijklmnopqrstuuabcdefghijklmnopqrstuvwxyz01234' _utu");
  run("usermod -L _utu");
  ok("usermod -L locks it", getpwnam("_utu")->pw_passwd[0] == '*');
  run("usermod -U _utu");
  ok("usermod -U unlocks it", strncmp(getpwnam("_utu")->pw_passwd, "$2b$", 4) == 0);
  ok("usermod -s", run("usermod -s /usr/bin/nologin _utu") == 0 &&
                       strcmp(getpwnam("_utu")->pw_shell, "/usr/bin/nologin") == 0);
  ok("chfn", run("chfn -f 'Another Name' _utu") == 0 &&
                 strcmp(getpwnam("_utu")->pw_gecos, "Another Name") == 0);
  ok("chsh", run("chsh -s /usr/bin/sh _utu") == 0 &&
                 strcmp(getpwnam("_utu")->pw_shell, "/usr/bin/sh") == 0);
  ok("a name with : is refused", run("chfn -f 'a:b' _utu") != 0);
  ok("usermod -l renames it", run("usermod -l _utv _utu") == 0 && getpwnam("_utv") != 0 &&
                                  getpwnam("_utu") == 0);
  ok("its memberships follow the name", member("_utg", "_utv") && !member("_utg", "_utu"));
  ok("and so does its own group", getgrnam("_utv") != 0 && getgrnam("_utu") == 0);
  ok("usermod -G sets the groups", run("usermod -G wheel _utv") == 0 && member("wheel", "_utv") &&
                                       !member("_utg", "_utv"));
  ok("usermod -a -G adds to them", run("usermod -a -G _utg _utv") == 0 &&
                                       member("wheel", "_utv") && member("_utg", "_utv"));

  ok("userdel -r", run("userdel -r _utv") == 0 && getpwnam("_utv") == 0);
  ok("the home went with it", stat("/home/_utu", &st) < 0);
  ok("so did its group and memberships", getgrnam("_utv") == 0 && !member("wheel", "_utv") &&
                                             !member("_utg", "_utv"));
  ok("groupdel", run("groupdel _utg") == 0 && getgrnam("_utg") == 0);
  ok("root may not be removed", run("userdel root") != 0 && getpwnam("root") != 0);
  ok("no lock file is left behind", access("/etc/ptmp", F_OK) < 0 && access("/etc/gtmp", F_OK) < 0);
  printf("usertest: %s\n", failures ? "FAILED" : "ok");
  return failures != 0;
}
