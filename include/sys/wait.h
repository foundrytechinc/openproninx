#ifndef _SYS_WAIT_H_
#define _SYS_WAIT_H_

#include <sys/types.h>

// status word packing
#define _WSTATUS(x) ((x) & 0177)
#define _WSTOPPED 0177
#define WIFSTOPPED(x) (_WSTATUS(x) == _WSTOPPED)
#define WSTOPSIG(x) (((x) >> 8) & 0xff)
#define WIFSIGNALED(x) (_WSTATUS(x) != _WSTOPPED && _WSTATUS(x) != 0 && (x) != 0x13)
#define WTERMSIG(x) (_WSTATUS(x))
#define WIFEXITED(x) (_WSTATUS(x) == 0)
#define WEXITSTATUS(x) (((x) >> 8) & 0xff)
#define WIFCONTINUED(x) ((x) == 0x13)
#define WCOREDUMP(x) ((x) & 0200)
#define W_EXITCODE(ret, sig) ((ret) << 8 | (sig))
#define W_STOPCODE(sig) ((sig) << 8 | _WSTOPPED)

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8
#define WSTOPPED WUNTRACED

pid_t wait(int *status);
pid_t waitpid(pid_t wpid, int *status, int options);
struct rusage;
pid_t wait3(int *status, int options, struct rusage *rusage);
pid_t wait4(pid_t pid, int *status, int options, struct rusage *rusage);

#endif
