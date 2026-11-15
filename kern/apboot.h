#ifndef PRONINX_APBOOT_H
#define PRONINX_APBOOT_H

// AP start page: a jump, then the header the kernel fills
#define AP_HDR_STEP 0x08   // u16: how far the AP got
#define AP_HDR_TABLES 0x0c // u32: the low tables, for paging on in 32 bit
#define AP_HDR_STACK 0x10  // u64
#define AP_HDR_ENTRY 0x18  // u64: mpentry
#define AP_HDR_CR3 0x20    // u64: the kernel's tables
#define AP_HDR_PROBE 0x28  // u16: nonzero, answer and halt
#define AP_HDR_COUNT 0x2a  // u16: how many answered
#define AP_HDR_IDS 0x2c    // u8[AP_PROBE_MAX]: their initial APIC ids
#define AP_PROBE_MAX 20
#define AP_HDR_END 0x40

#endif
