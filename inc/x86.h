#ifndef PRONINX_INC_X86_H
#define PRONINX_INC_X86_H

static inline void stosb(void *addr, int data, int cnt) {
  __asm__ volatile("cld; rep stosb"
                   : "=D"(addr), "=c"(cnt)
                   : "0"(addr), "1"(cnt), "a"(data)
                   : "memory", "cc");
}

#endif /* ifndef PRONINX_INC_X86_H */
