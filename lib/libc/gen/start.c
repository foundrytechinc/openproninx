#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tib.h>

char *__progname = (char *)"";

int main(int, char **, char **);

void ___start(int argc, char **argv, char **envp) {
  char *slash;

  // errno and thread stuff live in the thread block
  _libc_tib_init();
  environ = envp;
  if (argc > 0 && argv[0] != 0) {
    __progname = argv[0];
    if ((slash = strrchr(argv[0], '/')) != 0)
      __progname = slash + 1;
  }
  exit(main(argc, argv, envp));
}
