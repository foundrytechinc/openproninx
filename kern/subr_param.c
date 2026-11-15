#include "defs.h"
#include "mmu.h"
#include "param.h"
#include "inc/abi.h"

int maxusers, maxproc, maxprocperuid, maxfiles, maxfilesperproc;
int maxfilesrootres, maxthread, ncallout, maxvnodes;
int max_threads_per_proc = 1500;

static int tunable(const char *name, int dflt) {
  int n;

  return getenv_int(name, &n) && n >= 0 ? n : dflt;
}

void init_param2(void) {
  int mb = (int)(get_total_ram() >> 20);
  int64_t t;

  if ((maxusers = tunable("kern.maxusers", 0)) == 0 && (maxusers = mb / 8) < 32)
    maxusers = 32;
  maxproc = tunable("kern.maxproc", 20 + 16 * maxusers);
  if (maxproc < 32)
    maxproc = 32;
  if (maxproc > mb * 40)
    maxproc = mb * 40;
  if ((maxfiles = tunable("kern.maxfiles", maxproc * 16)) < 128)
    maxfiles = 128;
  maxfilesperproc = maxfiles / 4;
  maxfilesrootres = maxfiles / 20;
  if ((maxprocperuid = maxproc / 4) < 128)
    maxprocperuid = maxproc / 2;
  t = (int64_t)maxproc * max_threads_per_proc;
  maxthread = tunable("kern.maxthread", t < 1000000 ? (int)t : 1000000);
  maxvnodes = tunable("kern.maxvnodes",
                      maxproc + (int)(get_total_ram() / PGSIZE / 4));
  if ((ncallout = 16 + maxproc + maxfiles) > 5 * 60 * HZ)
    ncallout = 5 * 60 * HZ;
  ncallout = tunable("kern.ncallout", ncallout);
}

void param_sysctl(void) {
  const int S = FNU_CTLF_STDIO;

  sysctl_add_int("kern.maxusers", &maxusers, S, 0, 0);
  sysctl_add_int("kern.maxproc", &maxproc, S, 0, 0);
  sysctl_add_int("kern.maxprocperuid", &maxprocperuid, S | FNU_CTLF_WRITE, 1,
                 1 << 30);
  sysctl_add_int("kern.maxthread", &maxthread, S, 0, 0);
  sysctl_add_int("kern.maxfiles", &maxfiles, S | FNU_CTLF_WRITE, 128, 1 << 30);
  sysctl_add_int("kern.maxfilesperproc", &maxfilesperproc, S | FNU_CTLF_WRITE,
                 32, 1 << 30);
  sysctl_add_int("kern.maxfilesrootres", &maxfilesrootres, S | FNU_CTLF_WRITE,
                 0, 1 << 30);
  sysctl_add_int("kern.threads.max_threads_per_proc", &max_threads_per_proc,
                 S | FNU_CTLF_WRITE, 1, 1 << 30);
  sysctl_add_int("kern.ncallout", &ncallout, S, 0, 0);
  sysctl_add_int("kern.maxvnodes", &maxvnodes, S, 0, 0);
}
