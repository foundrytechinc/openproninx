#include "user.h"

int main(void) {
  char name[USER_NAME_MAX], password[65];
  char *shell[] = {"/usr/bin/sh", 0};
  pid_t pid;
  for (;;) {
    printf("OpenProninx login: ");
    gets(name, sizeof(name));
    if (strlen(name) && (name[strlen(name) - 1] == '\n' || name[strlen(name) - 1] == '\r'))
      name[strlen(name) - 1] = 0;
    if (name[0] == '\0') {
      sleep(1);
      continue;
    }
    printf("Password: ");
    if (read_password(password, sizeof(password)) < 0 || login(name, password) < 0) {
      printf("Login incorrect\n");
      memset(password, 0, sizeof(password));
      continue;
    }
    memset(password, 0, sizeof(password));
    pid = fork();
    if (pid < 0) {
      printf("login: cannot start shell\n");
      continue;
    }
    if (pid == 0) {
      exec("/usr/bin/sh", shell);
      shell[0] = "sh";
      exec("sh", shell);
      printf("login: cannot start shell\n");
      exit();
    }
    wait();
  }
}
