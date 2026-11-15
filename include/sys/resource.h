#ifndef _SYS_RESOURCE_H_
#define _SYS_RESOURCE_H_

#include <sys/types.h>
#include <sys/time.h>

// libc checks these match the system's
#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_RSS 5
#define RLIMIT_MEMLOCK 6
#define RLIMIT_NPROC 7
#define RLIMIT_NOFILE 8
#define RLIM_NLIMITS 9
#define RLIM_INFINITY (~(uint64_t)0)

typedef uint64_t rlim_t;

struct rlimit {
  rlim_t rlim_cur;
  rlim_t rlim_max;
};

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)

struct rusage {
  struct timeval ru_utime;
  struct timeval ru_stime;
  long ru_maxrss;
  long ru_nsignals;
  long ru_nvcsw;
  long ru_nivcsw;
};

#define PRIO_MIN (-20)
#define PRIO_MAX 20

#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2

int getrlimit(int resource, struct rlimit *rlp);
int setrlimit(int resource, const struct rlimit *rlp);
int getrusage(int who, struct rusage *rusage);
int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);

#endif
