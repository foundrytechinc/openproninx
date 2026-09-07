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

.comm stack, KSTACKSIZE

.section .note.GNU-stack,"",@progbits
