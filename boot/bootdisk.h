#ifndef PRONINX_BOOT_BOOTDISK_H
#define PRONINX_BOOT_BOOTDISK_H

#include "inc/types.h"

#define BOOTDISK_MAXBLK 4096

struct bootdisk {
  int (*read)(void *ctx, uint64 lba, uint32 count, void *dst);
  void *ctx;
  uint32 shift;
  uint64 blocks;
};

struct gpt_part {
  uint64 start, count;
  uint8 guid[16];
};

int gpt_scan(const struct bootdisk *d, const uint8 type[16], const char *name,
             struct gpt_part *out);

struct bootenv {
  char *buf;
  uint32 size, len;
};

void env_init(struct bootenv *e, char *buf, uint32 size);
int env_set(struct bootenv *e, const char *name, const char *value);
const char *env_get(struct bootenv *e, const char *name);
int env_true(struct bootenv *e, const char *name);
void autoboot(struct bootenv *e, int (*key)(void), void (*nap)(void));
void guid_text(const uint8 raw[16], char out[37]);

int root_env(const struct bootdisk *d, struct bootenv *e);

#endif
