// top: -d -s -b
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fnu/sys.h>
#include "inc/product.h"
#include <err.h>

static const char *state_to_str(proc_state_t state) {
  switch (state) {
  case FNU_PS_UNUSED:
    return "unused";
  case FNU_PS_SLEEPING:
    return "sleep";
  case FNU_PS_RUNNABLE:
    return "runble";
  case FNU_PS_RUNNING:
    return "run";
  case FNU_PS_ZOMBIE:
    return "zombie";
  case FNU_PS_STOPPED:
    return "stop";
  default:
    return "???";
  }
}

static int batch;

static char *fixed(uint32 value, char *out) {
  unsigned whole = value / FNU_FSCALE;
  unsigned frac = ((value % FNU_FSCALE) * 100) / FNU_FSCALE;

  snprintf(out, 8, "%u.%02u", whole, frac);
  return out;
}

static int by_pid(const void *a, const void *b) {
  const struct fnu_procinfo *x = a, *y = b;

  return x->pid < y->pid ? -1 : x->pid > y->pid;
}

static int draw(void) {
  struct fnu_info sys_info;
  struct fnu_procinfo *pinfo;
  char lav[3][8];
  int nprocs, i;

  if ((nprocs = fnu_info(&sys_info)) < 0) {
    warnx("sysinfo: %s", fnu_errstr(nprocs));
    return -1;
  }
  if ((nprocs = fnu_processes(&pinfo)) < 0) {
    warnx("procinfo: %s", fnu_errstr(nprocs));
    return -1;
  }
  qsort(pinfo, (size_t)nprocs, sizeof(*pinfo), by_pid);
  if (!batch)
    printf("\033[2J\033[H");
  printf("%s TOP\n", FNU_PRODUCT_NAME);
  printf("Uptime: %llu ticks (%llus) | Procs: %u | RAM: %llu/%llu KB free\n",
         sys_info.uptime, sys_info.uptime / FNU_HZ, sys_info.nprocs,
         sys_info.free_ram / 1024, sys_info.total_ram / 1024);
  printf("Load average: %s %s %s\n\n", fixed(sys_info.loadavg[0], lav[0]),
         fixed(sys_info.loadavg[1], lav[1]), fixed(sys_info.loadavg[2], lav[2]));
  printf("%-8s %-12s %-10s %4s %4s %6s %-10s\n", "PID", "NAME", "STATE", "PRI",
         "NI", "%CPU", "MEM");
  printf("------------------------------------------------------------------\n");
  for (i = 0; i < nprocs; i++) {
    if (pinfo[i].state == FNU_PS_UNUSED)
      continue;
    char share[8];

    printf("%-8llu %-12s %-10s %4lu %4ld %6s %-10llu KB\n", (uint64)pinfo[i].pid,
           pinfo[i].name, state_to_str(pinfo[i].state),
           (unsigned long)pinfo[i].priority, (long)pinfo[i].nice,
           fixed(pinfo[i].pctcpu * 100, share),
           (uint64)pinfo[i].memory_size / 1024);
  }
  fnu_table_free(pinfo);
  return 0;
}

int main(int argc, char **argv) {
  int i = 1, count = -1, delay = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
    const char *p = argv[i] + 1;

    if (*p == 'b' || *p == '1') {
      batch = 1;
      if (*p == '1')
        count = 1;
      continue;
    }
    if (*p == 'd' || *p == 's') {
      const char *v = p[1] != 0 ? p + 1 : (i + 1 < argc ? argv[++i] : "");
      if (*p == 'd')
        count = atoi(v);
      else
        delay = atoi(v);
      continue;
    }
    dprintf(2, "usage: top [-b] [-d count] [-s delay]\n");
    exit(1);
  }
  if (delay < 1)
    delay = 1;
  for (;;) {
    if (draw() < 0)
      exit(1);
    if (count > 0 && --count == 0)
      break;
    sleep(delay);
  }
  exit(0);
}
