# MSR access that may fault: the fault comes back here as a failure
#
#   int msr_read_try(uint32_t msr, uint64_t *val, uintptr_t *onfault);
#   int msr_write_try(uint32_t msr, uint64_t val, uintptr_t *onfault);

.globl msr_read_try
msr_read_try:
  movq %rdx, %r8
  leaq 1f(%rip), %rax
  movq %rax, (%r8)
  movl %edi, %ecx
  rdmsr
  movq $0, (%r8)
  shlq $32, %rdx
  movl %eax, %eax
  orq %rdx, %rax
  movq %rax, (%rsi)
  xorl %eax, %eax
  ret
1:
  movq $0, (%r8)
  movl $-1, %eax
  ret

.globl msr_write_try
msr_write_try:
  movq %rdx, %r8
  leaq 1f(%rip), %rax
  movq %rax, (%r8)
  movl %edi, %ecx
  movl %esi, %eax
  movq %rsi, %rdx
  shrq $32, %rdx
  wrmsr
  movq $0, (%r8)
  xorl %eax, %eax
  ret
1:
  movq $0, (%r8)
  movl $-1, %eax
  ret
