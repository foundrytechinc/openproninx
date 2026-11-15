.text
.globl _start
_start:
  # exec leaves a fake return address, then argc, argv and envp. A program that
  # speaks only the system's own calls starts here, with no library behind it
  mov 0x8(%rsp),%rdi   # argc
  mov 0x10(%rsp),%rsi  # argv
  mov 0x18(%rsp),%rdx  # envp
  and $-16,%rsp
  call fnu_main
  # fnu_main returning is the same as asking to leave
  xor %edi,%edi
  call fnu_exit
