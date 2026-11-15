#ifndef PRONINX_WAKE_H
#define PRONINX_WAKE_H

// SLEEP: two lowmem pages: jump, header, code
// page two is the real-mode stack
#define WAKE_HDR_FLAGS 0x08  // u8: what the wake code does first
#define WAKE_HDR_TABLES 0x0c // u32: the low tables
#define WAKE_HDR_EFER 0x10   // u64
#define WAKE_HDR_ENTRY 0x18  // u64: the kernel's resume entry
#define WAKE_HDR_RTC 0x20    // two clock images, pm_trace's first two steps
#define WAKE_RTC_SLOT 24     // RTC_IMAGE_MAX
#define WAKE_HDR_CLOCK_REGS 0x50 // the clock registers to take, 0xff ended
#define WAKE_HDR_CLOCK_TAKEN 0x5b // u8: they were taken
#define WAKE_HDR_CLOCK 0x5c      // what they held as the sleep ended
#define WAKE_HDR_END 0x68
#define WAKE_PAGES 2
#define WAKE_STACK 0x1f00 // from the start, the page after

#define WAKE_TRACE 0x01 // steps into the rtc
#define WAKE_VIDEO 0x02 // post the video bios again (reset_video)

// pm_trace steps the wake code writes itself
#define PMT_WAKECODE 1
#define PMT_VIDEO 2

#define CTX_RBX 0x00
#define CTX_RBP 0x08
#define CTX_R12 0x10
#define CTX_R13 0x18
#define CTX_R14 0x20
#define CTX_R15 0x28
#define CTX_RSP 0x30
#define CTX_RIP 0x38
#define CTX_CR0 0x40
#define CTX_CR3 0x48
#define CTX_CR4 0x50
#define CTX_GDT 0x58 // 10 bytes
#define CTX_IDT 0x68 // 10 bytes
#define CTX_FLAGS 0x78
#define CTX_SIZE 0x80

#endif
