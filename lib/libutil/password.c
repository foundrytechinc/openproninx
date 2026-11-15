// a password: the echo goes off before the prompt
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

static volatile sig_atomic_t caught[NSIG];

static const int watched[] = {SIGALRM, SIGHUP, SIGINT, SIGPIPE, SIGQUIT,
                              SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU};
#define NWATCHED (int)(sizeof(watched) / sizeof(watched[0]))

static void note(int sig) { caught[sig] = 1; }

// an ignored signal stays ignored
static void catch(int sig, const struct sigaction *sa, struct sigaction *old) {
  sigaction(sig, sa, old);
  if (old->sa_handler == SIG_IGN && sa->sa_handler != SIG_IGN)
    sigaction(sig, old, 0);
}

int read_password(const char *prompt, char *buf, int max) {
  struct sigaction sa, saved[NWATCHED];
  struct termios term, old;
  int in, out, i, n, again, error = 0, ttou;
  char c;

  if (max <= 0)
    return -1;
  for (;;) {
    memset((void *)caught, 0, sizeof(caught));
    if ((in = out = open(_PATH_TTY, O_RDWR | O_CLOEXEC)) < 0) {
      in = STDIN_FILENO;
      out = STDERR_FILENO;
    }
    // before the handlers: in the background this stops us with SIGTTOU
    if (tcgetattr(in, &old) == 0) {
      term = old;
      term.c_lflag &= ~(ECHO | ECHONL);
      tcsetattr(in, TCSAFLUSH, &term);
    } else {
      memset(&term, 0, sizeof(term));
      term.c_lflag |= ECHO;
      old = term;
    }
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = note;
    for (i = 0; i < NWATCHED; i++)
      catch(watched[i], &sa, &saved[i]);

    write(out, prompt, strlen(prompt));
    for (i = 0; (n = (int)read(in, &c, 1)) == 1 && c != '\n' && c != '\r';)
      if (i < max - 1)
        buf[i++] = c;
    buf[i] = 0;
    if (n < 0)
      error = errno;
    if (!(term.c_lflag & ECHO))
      write(out, "\n", 1);

    if (memcmp(&term, &old, sizeof(term)) != 0) {
      ttou = caught[SIGTTOU];
      while (tcsetattr(in, TCSAFLUSH, &old) < 0 && errno == EINTR && !caught[SIGTTOU])
        ;
      caught[SIGTTOU] = ttou;
    }
    for (i = 0; i < NWATCHED; i++)
      sigaction(watched[i], &saved[i], 0);
    if (in != STDIN_FILENO)
      close(in);

    // the signal goes through now the terminal is back; a stop asks again
    for (again = 0, i = 1; i < NSIG; i++)
      if (caught[i]) {
        kill(getpid(), i);
        if (i == SIGTSTP || i == SIGTTIN || i == SIGTTOU)
          again = 1;
      }
    if (!again)
      break;
    error = 0;
  }
  if (n < 0) {
    explicit_bzero(buf, (size_t)max);
    errno = error;
    return -1;
  }
  return 0;
}
