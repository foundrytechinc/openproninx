.text
.globl _start
_start:
  # exec leaves a fake return address, then argc, argv and envp.
  # see kern/exec.c
  mov 0x8(%rsp),%rdi   # argc
  mov 0x10(%rsp),%rsi  # argv
  mov 0x18(%rsp),%rdx  # envp
  # the ABI wants the stack sixteen-byte aligned at every call: an SSE
  # store to a local faults otherwise
  and $-16,%rsp
  call ___start
