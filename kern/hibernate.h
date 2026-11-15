// HIBERNATE
#ifndef PRONINX_HIBERNATE_H
#define PRONINX_HIBERNATE_H

#include "inc/types.h"

#define HIB_CHUNK (4ULL << 20)      // HIBERNATE_CHUNK_SIZE
#define HIB_CHUNKTABLE (2ULL << 20) // HIBERNATE_CHUNK_TABLE_SIZE
#define HIB_PIGLET (4 * HIB_CHUNK)
#define HIB_MIN_MEMORY (16 * HIB_PIGLET) // the piglet a sixteenth at most
#define HIB_SIG_BYTES 4096
#define HIB_RANGES 240

#define HIB_TAIL_SECTORS ((HIB_CHUNKTABLE + HIB_SIG_BYTES) / 512)

struct hib_range {
  uint64_t base, end;
};

struct hib_sig {
  char magic[8];
  uint32_t version;
  uint32_t nranges;
  uint64_t image_offset;
  uint64_t image_sectors;
  uint64_t chunktable_offset;
  uint64_t nchunks;
  uint64_t piglet_pa;
  uint64_t freemap;
  uint32_t kern_crc;
  uint32_t efi_crc;
  uint32_t text_changed; // pieces changed since that kernel booted
  uint32_t pad;
  uint64_t text_first;
  int64_t asleep_wall; // a traced sleep: the time as the clock took step 0
  struct hib_range ranges[HIB_RANGES];
  uint32_t crc;
};

struct hib_chunk {
  uint64_t base, end;
  uint64_t offset;
  uint64_t csize;
  uint32_t flags;
  uint32_t pad;
};

#define HIB_NOSWAP (-2) // the image is larger than the free run of fnu-swap
#define HIB_NOMEM (-3)  // or than the memory set aside for it
#define HIB_WRITE (-4)  // the disk would not take it

uint64_t hibernate_piglet(void);
int hibernate_prepare(void);
int hibernate_reserve(void);
int hibernate_shrink(void);
int hibernate_suspend(void);
int hibernate_write(void);
void hibernate_explain(int err);
void hibernate_made(void);
void hibernate_resumed(void);
void hibernate_unprepare(void);
void hibernate_resume(uint swapdev);

#endif
