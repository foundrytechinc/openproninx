#ifndef FNU_PRONINX_ULOGFMT_H
#define FNU_PRONINX_ULOGFMT_H

#define ULOG_NAME ".journal"
#define ULOG_BLK 512
#define ULOG_AREA 1024
#define ULOG_VERSION 1
#define ULOG_LIVE 1

#define ULOG_H_VERSION 8
#define ULOG_H_CRC 12
#define ULOG_H_GEN 16
#define ULOG_H_FSID 24
#define ULOG_H_MTIME 32
#define ULOG_H_AREA 40
#define ULOG_H_TAIL 48
#define ULOG_H_TAILSEQ 56
#define ULOG_H_FLAGS 64
#define ULOG_H_INO 68

#define LR_MAGIC 0x524a5850u
#define LR_BLOCKS 1
#define LR_REVOKE 2
#define LR_ORPHANS 3
#define LR_COMMIT 4
#define LR_HDR 32
#define LR_ENTRIES ((ULOG_BLK - LR_HDR) / 16)
#define LR_INOS ((ULOG_BLK - LR_HDR) / 4)
#define LR_FIRST 1

#define LR_TYPE 4
#define LR_COUNT 6
#define LR_SEQ 8
#define LR_LEN 16
#define LR_FLAGS 20

static const unsigned char ulog_magic[8] = {'P', 'X', 'U', 'F', 'S', 'J', 'N', 'L'};

// CRC-32C
static inline unsigned int ulog_crc(unsigned int crc, const unsigned char *p,
                                    unsigned long n) {
  static unsigned int tab[256];
  unsigned int i, k, c;

  if (tab[1] == 0)
    for (i = 0; i < 256; i++) {
      for (c = i, k = 0; k < 8; k++)
        c = (c & 1) ? (c >> 1) ^ 0x82f63b78u : c >> 1;
      tab[i] = c;
    }
  crc = ~crc;
  while (n-- > 0)
    crc = tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return ~crc;
}

#endif
