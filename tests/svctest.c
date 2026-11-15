// svctest: steps, services, the restart policy and services(8), on the live supervisor
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STATUS "/var/run/fnusvc.status"
#define CONTROL "/var/run/fnusvc.ctl"

static int failures;

static void ok(const char *what, int good) {
  printf("svctest: %-48s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

// field 1 is the state, 2 the pid
static int field(const char *name, int which, char *out, size_t size) {
  char line[512], *p, *f[4];
  FILE *fp;
  int k, found = 0;

  if ((fp = fopen(STATUS, "r")) == 0)
    return 0;
  while (!found && fgets(line, sizeof(line), fp) != 0) {
    for (k = 0, p = line; k < 4; k++) {
      f[k] = p;
      if (p != 0 && (p = strchr(p, '\t')) != 0)
        *p++ = 0;
    }
    if (f[3] != 0 && strcmp(f[0], name) == 0) {
      strlcpy(out, f[which], size);
      found = 1;
    }
  }
  fclose(fp);
  return found;
}

static int becomes(const char *name, const char *state, int tenths) {
  struct timespec pause = {0, 100000000};
  char now[32];

  while (tenths-- > 0) {
    if (field(name, 1, now, sizeof(now)) && strcmp(now, state) == 0)
      return 1;
    nanosleep(&pause, 0);
  }
  return 0;
}

static int vanishes(const char *name, int tenths) {
  struct timespec pause = {0, 100000000};
  char now[32];

  while (tenths-- > 0) {
    if (!field(name, 1, now, sizeof(now)))
      return 1;
    nanosleep(&pause, 0);
  }
  return 0;
}

static void put(const char *name, const char *text) {
  char path[256];
  FILE *fp;

  snprintf(path, sizeof(path), "/etc/rc.d/%s", name);
  if ((fp = fopen(path, "w")) != 0) {
    fputs(text, fp);
    fclose(fp);
  }
}

// what the command printed has this in it
static int says(const char *cmd, const char *want) {
  char out[512];
  size_t n;
  FILE *fp;

  if ((fp = popen(cmd, "r")) == 0)
    return 0;
  n = fread(out, 1, sizeof(out) - 1, fp);
  out[n] = 0;
  pclose(fp);
  return strstr(out, want) != 0;
}

static int run(const char *cmd) {
  char line[512];

  snprintf(line, sizeof(line), "%s > /dev/null 2>&1", cmd);
  return system(line);
}

int main(void) {
  char pid1[32] = "", pid2[32] = "";

  if (getuid() != 0 || access(CONTROL, F_OK) < 0) {
    printf("svctest: needs root and a running supervisor, skipped\n");
    return 0;
  }
  unlink("/tmp/svctest.out");
  put("_svstep", "desc=a test step\nstep=echo \"one two\" > /tmp/svctest.out\n");
  put("_svrun", "desc=a test service\nrun=/usr/bin/sleep 1000\n");
  put("_svcrash", "run=/usr/bin/sh -c \"exit 7\"\nlimit=2/60\n");
  put("_svblock", "requires=_svnone\nrun=/usr/bin/sleep 1000\n");
  run("services enable _svstep");
  run("services enable _svrun");
  run("services enable _svcrash");
  run("services enable _svblock");

  ok("a step runs to done", becomes("_svstep", "done", 50));
  ok("its quoted line went through the shell", access("/tmp/svctest.out", F_OK) == 0);
  ok("a service runs", becomes("_svrun", "running", 50));
  ok("a missing requirement blocks", becomes("_svblock", "blocked", 50));
  ok("failing twice in a minute is maintenance", becomes("_svcrash", "maintenance", 80));

  field("_svrun", 2, pid1, sizeof(pid1));
  run("services stop _svrun");
  ok("stop", becomes("_svrun", "stopped", 50));
  run("services start _svrun");
  ok("start", becomes("_svrun", "running", 50));
  run("services restart _svrun");
  ok("restart", becomes("_svrun", "running", 50));
  field("_svrun", 2, pid2, sizeof(pid2));
  ok("restart gives a new process", pid1[0] != 0 && strcmp(pid1, pid2) != 0);
  ok("restart answers with the state it came to",
     says("services restart _svrun", "_svrun: running"));
  run("services stop _svrun");
  put("_svlate", "requires=_svrun\nrun=/usr/bin/sleep 1000\n");
  run("services enable _svlate");
  ok("a stopped requirement blocks", becomes("_svlate", "blocked", 50));
  run("services start _svrun");
  ok("and the blocked one starts once it is up", becomes("_svlate", "running", 50));
  put("_svnone", "run=/usr/bin/sleep 1000\n");
  run("services enable _svnone");
  ok("enabling what it lacked starts it", becomes("_svblock", "running", 50));
  run("services clear _svcrash");
  ok("clear tries it again", becomes("_svcrash", "maintenance", 80));
  ok("its log has the restart decisions", run("grep -q 'restarting too quickly' /var/log/svc/_svcrash.log") == 0);

  run("services disable _svstep");
  run("services disable _svrun");
  run("services disable _svcrash");
  run("services disable _svblock");
  run("services disable _svlate");
  run("services disable _svnone");
  ok("disable takes them out of rc.conf", run("grep -q _sv /etc/rc.conf") != 0);
  ok("and the supervisor stops and forgets them",
     vanishes("_svrun", 100) && vanishes("_svstep", 10) && vanishes("_svcrash", 10) &&
         vanishes("_svblock", 10) && vanishes("_svlate", 10) &&
         vanishes("_svnone", 10));
  unlink("/etc/rc.d/_svstep");
  unlink("/etc/rc.d/_svrun");
  unlink("/etc/rc.d/_svcrash");
  unlink("/etc/rc.d/_svblock");
  unlink("/etc/rc.d/_svlate");
  unlink("/etc/rc.d/_svnone");
  unlink("/tmp/svctest.out");
  run("rm -f /var/log/svc/_sv*");
  printf("svctest: %s\n", failures ? "FAILED" : "ok");
  return failures != 0;
}
