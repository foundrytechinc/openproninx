#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <fnu/sys.h>
#include "ssh.h"
#include <err.h>
#include <util.h>

static void usage(void) {
  dprintf(2, "usage: ssh-keygen [-t ed25519] [-f file] [-C comment] [-l]\n");
  exit(1);
}

int main(int argc, char **argv) {
  unsigned char pub[32], sec[64];
  char file[256], comment[128], fp[80], dir[256];
  const char *user = getenv("USER");
  int i, list = 0;

  ssh_user_path(file, sizeof(file), "id_ed25519");
  snprintf(comment, sizeof(comment), "%s@openproninx", user ? user : "root");
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
      if (strcmp(argv[++i], "ed25519") != 0) {
        dprintf(2, "ssh-keygen: only ed25519 keys are made here\n");
        exit(1);
      }
    } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
      strlcpy(file, argv[++i], sizeof(file));
    else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc)
      strlcpy(comment, argv[++i], sizeof(comment));
    else if (strcmp(argv[i], "-N") == 0 && i + 1 < argc)
      i++;
    else if (strcmp(argv[i], "-l") == 0)
      list = 1;
    else
      usage();
  }

  if (list) {
    if (ssh_pubkey_read(file, pub) < 0 && ssh_key_read(file, pub, sec) < 0) {
      warnx("%s: not a readable key", file);
      exit(1);
    }
    ssh_fingerprint(pub, fp, sizeof(fp));
    printf("256 %s %s (ED25519)\n", fp, file);
    exit(0);
  }

  if (access(file, F_OK) == 0) {
    dprintf(2, "ssh-keygen: %s already exists\n", file);
    exit(1);
  }
  strlcpy(dir, file, sizeof(dir));
  mkdir(dirname(dir), 0777);
  if (ssh_key_generate(pub, sec) < 0 || ssh_key_write(file, pub, sec, comment) < 0) {
    warn("%s", file);
    exit(1);
  }
  explicit_bzero(sec, sizeof(sec));
  ssh_fingerprint(pub, fp, sizeof(fp));
  printf("Your identification has been saved in %s\n"
         "Your public key has been saved in %s.pub\n"
         "The key fingerprint is:\n%s %s\n",
         file, file, fp, comment);
  exit(0);
}
