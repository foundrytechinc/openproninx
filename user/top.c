#include "user/user.h"
#include "inc/product.h"

char*
state_to_str(proc_state_t state)
{
  switch(state){
    case UNUSED:   return "unused";
    case SLEEPING: return "sleep";
    case RUNNABLE: return "runble";
    case RUNNING:  return "run";
    case ZOMBIE:   return "zombie";
    default:       return "???";
  }
}

int
main(void)
{
  struct info sys_info;
  struct procinfo pinfo[64];
  int nprocs, i;

  if(info(&sys_info) < 0){
    printf("top: cannot get system info\n");
    exit();
  }

  nprocs = procinfo(pinfo, 64);
  if(nprocs < 0){
    printf("top: cannot get process info\n");
    exit();
  }

  printf("\033[2J\033[H"); // Clear screen
  printf("%s TOP\n", FNU_PRODUCT_NAME);
  printf("Uptime: %d ticks | Procs: %d | RAM: %d/%d KB free\n\n", 
         sys_info.uptime, sys_info.nprocs, sys_info.free_ram/1024, sys_info.total_ram/1024);

  printf("%-8s %-12s %-10s %-10s\n", "PID", "NAME", "STATE", "MEM");
  printf("--------------------------------------------\n");

  for(i = 0; i < nprocs; i++){
    if(pinfo[i].state == UNUSED) continue;
    printf("%-8d %-12s %-10s %-10d KB\n", 
           pinfo[i].pid, 
           pinfo[i].name, 
           state_to_str(pinfo[i].state),
           pinfo[i].memory_size / 1024);
  }

  exit();
}
