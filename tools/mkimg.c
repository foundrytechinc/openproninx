
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SECTOR 512
#define MB (1024 * 1024)

#define ESP_MIN_MB 64
#define ESP_SPARE_MB 8
#define BOOT_MB 1
#define SWAP_MB 256

#define ESP_START 2048
#define GPT_ENTRIES 128
#define GPT_ENTSZ 128
#define GPT_TBLSECS (GPT_ENTRIES * GPT_ENTSZ / SECTOR)

#define DOSPARTOFF 446
#define DOSMAGICOFFSET 510

static const char *progname = "mkimg";

static void die(const char *what) {
  fprintf(stderr, "%s: %s\n", progname, what);
  exit(1);
}

static void die2(const char *what, const char *who) {
  fprintf(stderr, "%s: %s: %s\n", progname, what, who);
  exit(1);
}

static void le16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v) {
  le16(p, v);
  le16(p + 2, v >> 16);
}

static void le64(uint8_t *p, uint64_t v) {
  le32(p, (uint32_t)v);
  le32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

static uint8_t *readfile(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  uint8_t *p;
  long n;

  if (f == 0)
    die2("cannot open", path);
  if (fseek(f, 0, SEEK_END) < 0)
    die2("cannot size", path);
  if ((n = ftell(f)) < 0)
    die2("cannot size", path);
  rewind(f);
  if ((p = malloc(n ? (size_t)n : 1)) == 0)
    die("out of memory");
  if (n > 0 && fread(p, 1, (size_t)n, f) != (size_t)n)
    die2("short read", path);
  fclose(f);
  *len = (size_t)n;
  return p;
}

// THE SUPERBLOCKS FINISHED
#define UFS2_SBLOCK 65536
#define UFS2_MAGIC 0x19540119u
#define SB_SBLKNO 8
#define SB_NCG 44
#define SB_FSIZE 52
#define SB_ID 144
#define SB_VOLNAME 680
#define MAXVOLLEN 32
#define SB_OLD_CPG 180
#define SB_FPG 188
#define SB_OLD_POSTBLFORMAT 1356
#define SB_OLD_NRPOS 1360
#define SB_MAGIC 1372

static const char *volname; // -L: older makefs has no label=

static void sb_finish(FILE *f, off_t at, const uint8_t *id, const char *path) {
  uint8_t one[4], name[MAXVOLLEN];

  le32(one, 1);
  if (volname != 0) {
    memset(name, 0, sizeof(name));
    strncpy((char *)name, volname, sizeof(name) - 1);
    if (fseeko(f, at + SB_VOLNAME, SEEK_SET) < 0 || fwrite(name, 1, sizeof(name), f) != sizeof(name))
      die2("cannot write", path);
  }
  if (fseeko(f, at + SB_ID, SEEK_SET) < 0 || fwrite(id, 1, 8, f) != 8 ||
      fseeko(f, at + SB_OLD_CPG, SEEK_SET) < 0 || fwrite(one, 1, 4, f) != 4 ||
      fseeko(f, at + SB_OLD_POSTBLFORMAT, SEEK_SET) < 0 ||
      fwrite(one, 1, 4, f) != 4 ||
      fseeko(f, at + SB_OLD_NRPOS, SEEK_SET) < 0 || fwrite(one, 1, 4, f) != 4)
    die2("cannot write", path);
}

static void ufs_newid(const char *path) {
  uint8_t sb[SB_MAGIC + 4], id[8], rnd[4];
  uint32_t ncg, fpg, sblkno, fsize, cg;
  FILE *f, *r;

  if ((f = fopen(path, "r+b")) == 0)
    die2("cannot open", path);
  if (fseeko(f, UFS2_SBLOCK, SEEK_SET) < 0 || fread(sb, 1, sizeof(sb), f) != sizeof(sb) ||
      rd32(sb + SB_MAGIC) != UFS2_MAGIC)
    die2("not UFS2", path);
  ncg = rd32(sb + SB_NCG);
  fpg = rd32(sb + SB_FPG);
  sblkno = rd32(sb + SB_SBLKNO);
  fsize = rd32(sb + SB_FSIZE);
  if ((r = fopen("/dev/urandom", "rb")) == 0 || fread(rnd, 1, 4, r) != 4)
    die("cannot read /dev/urandom");
  fclose(r);
  le32(id, (uint32_t)time(0));
  memcpy(id + 4, rnd, 4);
  sb_finish(f, UFS2_SBLOCK, id, path);
  for (cg = 0; cg < ncg; cg++) {
    off_t at = ((off_t)fpg * cg + sblkno) * fsize;

    if (fseeko(f, at + SB_MAGIC, SEEK_SET) < 0 || fread(rnd, 1, 4, f) != 4 ||
        rd32(rnd) != UFS2_MAGIC)
      die2("a cylinder group has no superblock", path);
    sb_finish(f, at, id, path);
  }
  if (fclose(f) != 0)
    die2("cannot finish", path);
}

// THE IMAGE FILE
static FILE *out;

static void put(uint64_t lba, const void *buf, size_t len) {
  if (fseeko(out, (off_t)(lba * SECTOR), SEEK_SET) < 0)
    die("seek");
  if (len > 0 && fwrite(buf, 1, len, out) != len)
    die("write");
}

// THE BOOT SECTOR
static uint32_t elf32_symbol(const uint8_t *e, size_t len, const char *name) {
  uint32_t shoff, shentsize, shnum, i, symtab = 0, strtab = 0;

  if (len < 52 || memcmp(e, "\177ELF\1\1", 6) != 0)
    die("mbr elf: not a 32-bit little-endian ELF file");
  shoff = rd32(e + 32);
  shentsize = rd16(e + 46);
  shnum = rd16(e + 48);
  if ((uint64_t)shoff + (uint64_t)shentsize * shnum > len)
    die("mbr elf: section headers outside the file");
  for (i = 0; i < shnum; i++) {
    const uint8_t *sh = e + shoff + i * shentsize;

    if (rd32(sh + 4) == 2) { // SHT_SYMTAB
      symtab = i;
      strtab = rd32(sh + 24); // sh_link
    }
  }
  if (symtab == 0)
    die("mbr elf: no symbol table");
  {
    const uint8_t *sh = e + shoff + symtab * shentsize;
    const uint8_t *st = e + shoff + strtab * shentsize;
    uint32_t off = rd32(sh + 16), size = rd32(sh + 20);
    uint32_t stoff = rd32(st + 16), stsize = rd32(st + 20);

    if ((uint64_t)off + size > len || (uint64_t)stoff + stsize > len)
      die("mbr elf: symbol table outside the file");
    for (i = 0; i + 16 <= size; i += 16) {
      const uint8_t *sym = e + off + i;
      uint32_t nm = rd32(sym);

      if (nm < stsize && strcmp((const char *)e + stoff + nm, name) == 0)
        return rd32(sym + 4);
    }
  }
  die2("mbr elf: symbol not found", name);
  return 0;
}

// GPT
static const uint32_t crc32_tab[] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d};

static uint32_t crc32(const void *buf, size_t sz) {
  const uint8_t *p = buf;
  uint32_t crc = ~0U;

  while (sz--)
    crc = crc32_tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return crc ^ ~0U;
}

static void uuid_enc(uint8_t *p, const char *text) {
  uint32_t f[5] = {0, 0, 0, 0, 0};
  const char *s = text;
  uint64_t node = 0;
  int i;

  for (i = 0; i < 5; i++) {
    uint64_t v = 0;
    while (*s && *s != '-') {
      int c = *s++;
      v = v * 16 + (uint64_t)(c >= 'a' ? c - 'a' + 10
                              : c >= 'A' ? c - 'A' + 10 : c - '0');
    }
    if (i < 4)
      f[i] = (uint32_t)v;
    else
      node = v;
    if (*s == '-')
      s++;
  }
  le32(p, f[0]);
  le16(p + 4, f[1]);
  le16(p + 6, f[2]);
  p[8] = (uint8_t)(f[3] >> 8);
  p[9] = (uint8_t)f[3];
  for (i = 0; i < 6; i++)
    p[10 + i] = (uint8_t)(node >> (40 - 8 * i));
}

static void uuid_random(uint8_t *p) {
  static FILE *rnd;

  if (rnd == 0 && (rnd = fopen("/dev/urandom", "rb")) == 0)
    die("cannot open /dev/urandom");
  if (fread(p, 1, 16, rnd) != 16)
    die("short read from /dev/urandom");
  p[7] = (uint8_t)((p[7] & 0x0f) | 0x40);
  p[8] = (uint8_t)((p[8] & 0x3f) | 0x80);
}

struct part {
  const char *type;
  const char *name;
  uint64_t start, sectors;
  uint64_t attr;
  const char *uuid; // or a random one
};

static void uuid_print(void) {
  uint8_t u[16];

  uuid_random(u);
  printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
         u[3], u[2], u[1], u[0], u[5], u[4], u[7], u[6], u[8], u[9], u[10], u[11],
         u[12], u[13], u[14], u[15]);
}

// 8-4-4-4-12 hex digits
static int uuid_valid(const char *s) {
  int i;

  for (i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-')
        return 0;
    } else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
                 (s[i] >= 'A' && s[i] <= 'F'))) {
      return 0;
    }
  }
  return s[36] == 0;
}

#define GPT_ENT_ATTR_LEGACY_BOOT (1ULL << 2)

static void gpt_write(uint64_t total, const struct part *parts, int nparts,
                      const uint8_t *bootcode, int active) {
  uint8_t pmbr[SECTOR], hdr[SECTOR], *tbl;
  uint32_t crc;
  int i, j;

  memset(pmbr, 0, sizeof(pmbr));
  memcpy(pmbr, bootcode, DOSPARTOFF - 6);
  pmbr[DOSPARTOFF] = active ? 0x80 : 0;
  pmbr[DOSPARTOFF + 2] = 2;
  pmbr[DOSPARTOFF + 4] = 0xee;
  pmbr[DOSPARTOFF + 5] = 0xff;
  pmbr[DOSPARTOFF + 6] = 0xff;
  pmbr[DOSPARTOFF + 7] = 0xff;
  le32(pmbr + DOSPARTOFF + 8, 1);
  le32(pmbr + DOSPARTOFF + 12,
       total - 1 > 0xffffffffULL ? 0xffffffffU : (uint32_t)(total - 1));
  le16(pmbr + DOSMAGICOFFSET, 0xaa55);
  put(0, pmbr, sizeof(pmbr));

  if ((tbl = calloc(GPT_TBLSECS, SECTOR)) == 0)
    die("out of memory");
  for (i = 0; i < nparts; i++) {
    uint8_t *ent = tbl + i * GPT_ENTSZ;

    uuid_enc(ent, parts[i].type);
    if (parts[i].uuid != 0)
      uuid_enc(ent + 16, parts[i].uuid);
    else
      uuid_random(ent + 16);
    le64(ent + 32, parts[i].start);
    le64(ent + 40, parts[i].start + parts[i].sectors - 1);
    le64(ent + 48, parts[i].attr);
    for (j = 0; parts[i].name[j] != 0 && j < 36; j++)
      le16(ent + 56 + 2 * j, (uint8_t)parts[i].name[j]);
  }
  put(2, tbl, GPT_TBLSECS * SECTOR);
  put(total - 1 - GPT_TBLSECS, tbl, GPT_TBLSECS * SECTOR);

  memset(hdr, 0, sizeof(hdr));
  memcpy(hdr, "EFI PART", 8);
  le32(hdr + 8, 0x00010000);
  le32(hdr + 12, 92);
  le64(hdr + 40, 2 + GPT_TBLSECS);
  le64(hdr + 48, total - 2 - GPT_TBLSECS);
  uuid_random(hdr + 56);
  le32(hdr + 80, GPT_ENTRIES);
  le32(hdr + 84, GPT_ENTSZ);
  le32(hdr + 88, crc32(tbl, GPT_TBLSECS * SECTOR));

  le64(hdr + 24, 1);
  le64(hdr + 32, total - 1);
  le64(hdr + 72, 2);
  le32(hdr + 16, 0);
  crc = crc32(hdr, 92);
  le32(hdr + 16, crc);
  put(1, hdr, sizeof(hdr));

  le64(hdr + 24, total - 1);
  le64(hdr + 32, 1);
  le64(hdr + 72, total - 1 - GPT_TBLSECS);
  le32(hdr + 16, 0);
  crc = crc32(hdr, 92);
  le32(hdr + 16, crc);
  put(total - 1, hdr, sizeof(hdr));
  free(tbl);
}

// FAT32
#define FAT_RESERVED 32
#define FAT_FSINFO 1
#define FAT_BACKUP 6
#define FAT_NFATS 2
#define FAT_SPC 1
#define FAT_EOC 0x0fffffffu

static struct {
  uint8_t *img;
  uint32_t sectors, fatsecs, clusters, next;
  uint32_t hidden;
} fat;

static uint8_t *fat_sector(uint32_t lsn) { return fat.img + (size_t)lsn * SECTOR; }

static uint32_t fat_get(uint32_t c) {
  return rd32(fat_sector(FAT_RESERVED) + (size_t)c * 4) & FAT_EOC;
}

static void fat_set(uint32_t c, uint32_t v) {
  int i;

  for (i = 0; i < FAT_NFATS; i++) {
    uint8_t *e = fat_sector(FAT_RESERVED + i * fat.fatsecs) + (size_t)c * 4;
    le32(e, (rd32(e) & 0xf0000000u) | (v & FAT_EOC));
  }
}

static uint8_t *fat_cluster(uint32_t c) {
  return fat_sector(FAT_RESERVED + FAT_NFATS * fat.fatsecs + (c - 2) * FAT_SPC);
}

static uint32_t fat_alloc(void) {
  uint32_t c = fat.next++;

  if (c >= fat.clusters + 2)
    die("esp: out of clusters");
  fat_set(c, FAT_EOC);
  return c;
}

static void fat_format(uint32_t sectors, uint32_t hidden, const char *label,
                       uint32_t volid, const struct tm *tm) {
  uint8_t *bs, *fs;
  uint32_t x, x1, cls;
  int i;

  fat.sectors = sectors;
  fat.hidden = hidden;
  if ((fat.img = calloc(sectors, SECTOR)) == 0)
    die("out of memory");

  x1 = FAT_RESERVED;
  x = (uint32_t)((uint64_t)(sectors - x1) * SECTOR * 8 /
                 (FAT_SPC * SECTOR * 8 + 32 / 4 * FAT_NFATS));
  fat.fatsecs = (uint32_t)(((uint64_t)(2 + x) * 4 + SECTOR - 1) / SECTOR);
  x1 += fat.fatsecs * FAT_NFATS;
  cls = (sectors - x1) / FAT_SPC;
  if ((uint64_t)fat.fatsecs * SECTOR / 4 - 2 < cls)
    cls = (uint32_t)((uint64_t)fat.fatsecs * SECTOR / 4 - 2);
  if (cls < 0xfff5)
    die("esp: too small for FAT32");
  fat.clusters = cls;
  fat.next = 2;

  bs = fat_sector(0);
  bs[0] = 0xeb;
  bs[1] = 0x58;
  bs[2] = 0x90;
  memcpy(bs + 3, "BSD4.4  ", 8);
  le16(bs + 11, SECTOR);
  bs[13] = FAT_SPC;
  le16(bs + 14, FAT_RESERVED);
  bs[16] = FAT_NFATS;
  le16(bs + 17, 0);
  le16(bs + 19, 0);
  bs[21] = hidden ? 0xf8 : 0xf0;
  le16(bs + 22, 0);
  le16(bs + 24, 63);
  le16(bs + 26, 255);
  le32(bs + 28, hidden);
  le32(bs + 32, sectors);
  le32(bs + 36, fat.fatsecs);
  le16(bs + 40, 0);
  le16(bs + 42, 0);
  le32(bs + 44, 2);
  le16(bs + 48, FAT_FSINFO);
  le16(bs + 50, FAT_BACKUP);
  bs[64] = 0x80;
  bs[66] = 0x29;
  le32(bs + 67, volid);
  memset(bs + 71, ' ', 11);
  memcpy(bs + 71, label, strlen(label) > 11 ? 11 : strlen(label));
  memcpy(bs + 82, "FAT32   ", 8);
  le16(bs + 510, 0xaa55);

  fs = fat_sector(FAT_FSINFO);
  le32(fs, 0x41615252);
  le32(fs + SECTOR - 28, 0x61417272);
  le32(fs + SECTOR - 24, 0xffffffff);
  le32(fs + SECTOR - 20, 0xffffffff);
  le16(fs + SECTOR - 2, 0xaa55);

  memcpy(fat_sector(FAT_BACKUP), bs, SECTOR);
  memcpy(fat_sector(FAT_BACKUP + FAT_FSINFO), fs, SECTOR);

  for (i = 0; i < FAT_NFATS; i++) {
    uint8_t *f = fat_sector(FAT_RESERVED + i * fat.fatsecs);
    le32(f, 0x0fffff00u | bs[21]);
    le32(f + 4, FAT_EOC);
  }
  fat_alloc();

  {
    uint8_t *de = fat_cluster(2);
    memset(de, ' ', 11);
    memcpy(de, label, strlen(label) > 11 ? 11 : strlen(label));
    de[11] = 0x08;
    le16(de + 22, (uint32_t)tm->tm_hour << 11 | (uint32_t)tm->tm_min << 5 |
                      (uint32_t)tm->tm_sec >> 1);
    le16(de + 24, (uint32_t)(tm->tm_year - 80) << 9 |
                      (uint32_t)(tm->tm_mon + 1) << 5 | (uint32_t)tm->tm_mday);
  }
}

static int fat_name83(const char *name, uint8_t *out, uint8_t *ntflags) {
  const char *dot = strrchr(name, '.');
  size_t base = dot ? (size_t)(dot - name) : strlen(name), ext = dot ? strlen(dot + 1) : 0;
  size_t i;
  int lowbase = 0, lowext = 0, upbase = 0, upext = 0;

  if (base == 0 || base > 8 || ext > 3)
    return -1;
  memset(out, ' ', 11);
  for (i = 0; i < base; i++) {
    int c = (unsigned char)name[i];
    if (c >= 'a' && c <= 'z')
      lowbase = 1, c -= 'a' - 'A';
    else if (c >= 'A' && c <= 'Z')
      upbase = 1;
    out[i] = (uint8_t)c;
  }
  for (i = 0; i < ext; i++) {
    int c = (unsigned char)dot[1 + i];
    if (c >= 'a' && c <= 'z')
      lowext = 1, c -= 'a' - 'A';
    else if (c >= 'A' && c <= 'Z')
      upext = 1;
    out[8 + i] = (uint8_t)c;
  }
  if ((lowbase && upbase) || (lowext && upext))
    return -1;
  *ntflags = (uint8_t)((lowbase ? 0x08 : 0) | (lowext ? 0x10 : 0));
  return 0;
}

static void fat_stamp(uint8_t *de, const struct tm *tm) {
  uint32_t t = (uint32_t)tm->tm_hour << 11 | (uint32_t)tm->tm_min << 5 |
               (uint32_t)tm->tm_sec >> 1;
  uint32_t d = (uint32_t)(tm->tm_year - 80) << 9 |
               (uint32_t)(tm->tm_mon + 1) << 5 | (uint32_t)tm->tm_mday;

  le16(de + 14, t);
  le16(de + 16, d);
  le16(de + 18, d);
  le16(de + 22, t);
  le16(de + 24, d);
}

static uint8_t *fat_dirslot(uint32_t dir) {
  uint32_t c = dir, last = dir;
  uint32_t i;

  for (;;) {
    uint8_t *cl = fat_cluster(c);

    for (i = 0; i < FAT_SPC * SECTOR; i += 32)
      if (cl[i] == 0)
        return cl + i;
    last = c;
    c = fat_get(c);
    if (c >= 0x0ffffff8u)
      break;
  }
  c = fat_alloc();
  fat_set(last, c);
  return fat_cluster(c);
}

static uint32_t fat_mkdir(uint32_t parent, const char *name,
                          const struct tm *tm) {
  uint8_t *de = fat_dirslot(parent), *dot;
  uint32_t c = fat_alloc();
  uint8_t flags;

  if (fat_name83(name, de, &flags) < 0)
    die2("esp: not an 8.3 name", name);
  de[11] = 0x10;
  de[12] = flags;
  fat_stamp(de, tm);
  le16(de + 20, c >> 16);
  le16(de + 26, c & 0xffff);

  dot = fat_cluster(c);
  memset(dot, ' ', 11);
  dot[0] = '.';
  dot[11] = 0x10;
  fat_stamp(dot, tm);
  le16(dot + 20, c >> 16);
  le16(dot + 26, c & 0xffff);
  memset(dot + 32, ' ', 11);
  dot[32] = '.';
  dot[33] = '.';
  dot[32 + 11] = 0x10;
  fat_stamp(dot + 32, tm);
  if (parent != 2) {
    le16(dot + 32 + 20, parent >> 16);
    le16(dot + 32 + 26, parent & 0xffff);
  }
  return c;
}

static void fat_addfile(uint32_t dir, const char *name, const uint8_t *data,
                        size_t len, const struct tm *tm) {
  uint8_t *de = fat_dirslot(dir);
  uint32_t first = 0, prev = 0, c;
  size_t off;
  uint8_t flags;

  if (fat_name83(name, de, &flags) < 0)
    die2("esp: not an 8.3 name", name);
  de[11] = 0x20;
  de[12] = flags;
  fat_stamp(de, tm);
  for (off = 0; off < len; off += FAT_SPC * SECTOR) {
    size_t n = len - off < FAT_SPC * SECTOR ? len - off : FAT_SPC * SECTOR;

    c = fat_alloc();
    memcpy(fat_cluster(c), data + off, n);
    if (prev == 0)
      first = c;
    else
      fat_set(prev, c);
    prev = c;
  }
  le16(de + 20, first >> 16);
  le16(de + 26, first & 0xffff);
  le32(de + 28, (uint32_t)len);
}

// THE SPEC
static void spec_dir(const char *root, const char *rel) {
  char path[4096];
  struct dirent **names = 0;
  DIR *d;
  struct dirent *e;
  int n = 0, cap = 0, i;

  snprintf(path, sizeof(path), "%s%s%s", root, rel[0] ? "/" : "", rel);
  if ((d = opendir(path)) == 0)
    die2("cannot open", path);
  while ((e = readdir(d)) != 0) {
    struct dirent *copy;

    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 64;
      if ((names = realloc(names, cap * sizeof(*names))) == 0)
        die("out of memory");
    }
    if ((copy = malloc(sizeof(*copy) + strlen(e->d_name) + 1)) == 0)
      die("out of memory");
    memcpy(copy, e, sizeof(*copy));
    strcpy(copy->d_name, e->d_name);
    names[n++] = copy;
  }
  closedir(d);
  for (i = 1; i < n; i++) {
    struct dirent *k = names[i];
    int j = i - 1;

    while (j >= 0 && strcmp(names[j]->d_name, k->d_name) > 0) {
      names[j + 1] = names[j];
      j--;
    }
    names[j + 1] = k;
  }
  for (i = 0; i < n; i++) {
    char sub[4096], full[4096];
    struct stat st;

    snprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", names[i]->d_name);
    snprintf(full, sizeof(full), "%s/%s", root, sub);
    if (lstat(full, &st) < 0)
      die2("cannot stat", full);
    if (S_ISDIR(st.st_mode)) {
      printf("./%s type=dir mode=0%o\n", sub, (unsigned)(st.st_mode & 07777));
      spec_dir(root, sub);
    } else if (S_ISREG(st.st_mode)) {
      printf("./%s type=file mode=0%o\n", sub, (unsigned)(st.st_mode & 07777));
    }
    free(names[i]);
  }
  free(names);
}

static void usage(void) {
  fprintf(stderr,
          "usage: %s [-a] -o OUT -m MBR -e MBR.elf -s STAGE2 -k KERNEL "
          "-b BOOTX64.EFI [-r RAMDISK] [-u ROOT.ufs] [-R ROOT-UUID] [-w SWAP-MB]\n"
          "       %s -S DIR\n       %s -G\n       %s -i UFS [-L label]\n",
          progname, progname, progname, progname);
  exit(1);
}

int main(int argc, char **argv) {
  const char *outpath = 0, *mbrpath = 0, *elfpath = 0, *stage2path = 0;
  const char *kernelpath = 0, *efipath = 0, *ramdiskpath = 0, *ufspath = 0;
  const char *specdir = 0, *rootuuid = 0, *newid = 0;
  int active = 0, newuuid = 0, c;
  char *end;
  uint8_t *mbr, *elf, *stage2, *kernel, *efi, *ramdisk = 0, *ufs = 0;
  size_t mbrlen, elflen, stage2len, kernellen, efilen, ramdisklen = 0, ufslen = 0;
  uint64_t esp_start = ESP_START, esp_secs, esp_mb;
  uint64_t boot_start, boot_secs = (uint64_t)BOOT_MB * MB / SECTOR;
  uint64_t root_start, root_secs;
  uint64_t swap_start, swap_secs = (uint64_t)SWAP_MB * MB / SECTOR;
  uint64_t total, stage2_secs;
  uint32_t nsects_addr, off;
  time_t now;
  struct tm *tm;
  uint8_t zero[SECTOR];
  struct part parts[4] = {
      {"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI System", 0, 0, 0, 0},
      {"7B589DA3-BDC2-4F6C-8028-FC43D46C5953", "fnu-boot", 0, 0,
       GPT_ENT_ATTR_LEGACY_BOOT, 0},
      {"516E7CB6-6ECF-11D6-8FF8-00022D09712B", "fnu-root", 0, 0, 0, 0},
      {"516E7CB5-6ECF-11D6-8FF8-00022D09712B", "fnu-swap", 0, 0, 0, 0},
  };
  uint32_t efidir, bootdir, fnudir;

  while ((c = getopt(argc, argv, "ao:m:e:s:k:b:r:u:S:R:Gi:L:w:")) != -1) {
    switch (c) {
    case 'a': active = 1; break;
    case 'o': outpath = optarg; break;
    case 'm': mbrpath = optarg; break;
    case 'e': elfpath = optarg; break;
    case 's': stage2path = optarg; break;
    case 'k': kernelpath = optarg; break;
    case 'b': efipath = optarg; break;
    case 'r': ramdiskpath = optarg; break;
    case 'u': ufspath = optarg; break;
    case 'S': specdir = optarg; break;
    case 'R': rootuuid = optarg; break;
    case 'G': newuuid = 1; break;
    case 'i': newid = optarg; break;
    case 'L': volname = optarg; break;
    case 'w':
      swap_secs = strtoull(optarg, &end, 10) * MB / SECTOR;
      if (*optarg == 0 || *end != 0 || swap_secs == 0)
        die2("not a size in MiB:", optarg);
      break;
    default: usage();
    }
  }
  if (newuuid) {
    uuid_print();
    return 0;
  }
  if (newid != 0) {
    ufs_newid(newid);
    return 0;
  }
  if (rootuuid != 0 && !uuid_valid(rootuuid))
    die2("not a uuid:", rootuuid);
  parts[2].uuid = rootuuid;
  if (specdir != 0) {
    printf("/set type=file uid=0 gid=0\n. type=dir mode=0755\n");
    spec_dir(specdir, "");
    return 0;
  }
  if (outpath == 0 || mbrpath == 0 || elfpath == 0 || stage2path == 0 ||
      kernelpath == 0 || efipath == 0)
    usage();

  mbr = readfile(mbrpath, &mbrlen);
  elf = readfile(elfpath, &elflen);
  stage2 = readfile(stage2path, &stage2len);
  kernel = readfile(kernelpath, &kernellen);
  efi = readfile(efipath, &efilen);
  if (ramdiskpath != 0)
    ramdisk = readfile(ramdiskpath, &ramdisklen);
  if (ufspath != 0)
    ufs = readfile(ufspath, &ufslen);
  if (mbrlen < DOSPARTOFF - 6)
    die("mbr: shorter than its boot code");
  stage2_secs = (stage2len + SECTOR - 1) / SECTOR;
  if (stage2_secs > boot_secs)
    die("stage2 is larger than the fnu-boot partition");
  root_secs = ((ufs != 0 ? ufslen : ramdisklen) + MB - 1) / MB * MB / SECTOR;
  if (root_secs == 0)
    die("no root: neither -u nor -r");
  esp_mb = (efilen + kernellen + ramdisklen + MB - 1) / MB + ESP_SPARE_MB;
  esp_mb = (esp_mb + 7) / 8 * 8;
  if (esp_mb < ESP_MIN_MB)
    esp_mb = ESP_MIN_MB;
  esp_secs = esp_mb * MB / SECTOR;

  nsects_addr = elf32_symbol(elf, elflen, "nsects");
  off = nsects_addr - 0x600;
  if (off >= DOSPARTOFF - 8)
    die("mbr: nsects at an unexpected offset");
  le16(mbr + off, (uint32_t)stage2_secs);

  boot_start = esp_start + esp_secs;
  root_start = boot_start + boot_secs;
  swap_start = root_start + root_secs;
  total = swap_start + swap_secs + 2048;
  parts[0].start = esp_start, parts[0].sectors = esp_secs;
  parts[1].start = boot_start, parts[1].sectors = boot_secs;
  parts[2].start = root_start, parts[2].sectors = root_secs;
  parts[3].start = swap_start, parts[3].sectors = swap_secs;

  if ((out = fopen(outpath, "wb")) == 0)
    die2("cannot create", outpath);
  memset(zero, 0, sizeof(zero));
  put(total - 1, zero, sizeof(zero));

  gpt_write(total, parts, 4, mbr, active);

  now = time(0);
  tm = gmtime(&now);
  fat_format((uint32_t)esp_secs, (uint32_t)esp_start, "FNUESP",
             (uint32_t)now, tm);
  efidir = fat_mkdir(2, "EFI", tm);
  bootdir = fat_mkdir(efidir, "BOOT", tm);
  fnudir = fat_mkdir(2, "fnu", tm);
  fat_addfile(bootdir, "BOOTX64.EFI", efi, efilen, tm);
  fat_addfile(fnudir, "kernel", kernel, kernellen, tm);
  if (ramdisk != 0 && ramdisklen > 0)
    fat_addfile(fnudir, "ramdisk", ramdisk, ramdisklen, tm);
  put(esp_start, fat.img, (size_t)esp_secs * SECTOR);

  put(boot_start, stage2, stage2len);
  if (ufs != 0)
    put(root_start, ufs, ufslen);
  if (fclose(out) != 0)
    die2("cannot finish", outpath);

  printf("%s: esp@%llu fnu-boot@%llu (%llu sectors) root@%llu (%s)\n",
         outpath, (unsigned long long)esp_start, (unsigned long long)boot_start,
         (unsigned long long)stage2_secs, (unsigned long long)root_start,
         ufs != 0 ? "ufs2" : "empty");
  return 0;
}
