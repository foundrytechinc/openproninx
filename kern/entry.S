#include "memlayout.h"
#include "param.h"

# kernel entry, from the boot block. runs near 0x100000.
.globl _start
_start = V2P_WO(entry)

entry:
  movq $KERNBASE,%rdx

  leaq stack(%rip),%rax
  add $KSTACKSIZE,%rax
  addq %rdx,%rax
  mov %rax,%rsp

  leaq entry_high_address(%rip),%rax
  addq %rdx,%rax
  jmp *%rax

entry_high_address:
  jmp kmain

# an AP, from the start code: %rcx its tables, %rsp its stack
.globl mpentry
mpentry:
  movq %rcx,%cr3
  call mp_startup

.comm stack, KSTACKSIZE
