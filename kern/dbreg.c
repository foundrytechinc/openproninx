#include "defs.h"
#include "inc/types.h"

void dbregs_load(const uint64_t *dr) {
  __asm __volatile("mov %0, %%dr0" : : "r"(dr[0]));
  __asm __volatile("mov %0, %%dr1" : : "r"(dr[1]));
  __asm __volatile("mov %0, %%dr2" : : "r"(dr[2]));
  __asm __volatile("mov %0, %%dr3" : : "r"(dr[3]));
  __asm __volatile("mov %0, %%dr7" : : "r"(dr[7]));
}

void dbregs_clear(void) {
  uint64_t zero = 0;
  __asm __volatile("mov %0, %%dr7" : : "r"(zero));
}

uint64_t dbregs_status(void) {
  uint64_t dr6, zero = 0;
  __asm __volatile("mov %%dr6, %0" : "=r"(dr6));
  __asm __volatile("mov %0, %%dr6" : : "r"(zero));
  return dr6;
}
