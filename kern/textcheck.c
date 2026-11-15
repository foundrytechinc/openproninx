// KERNEL TEXT
// text and rodata in 64 KB pieces: a stray write shows up here
#include "defs.h"
#include "memlayout.h"
#include "ulogfmt.h"
#include "inc/abi.h"

#define PIECE (64 * 1024)
#define PIECES 256

// the padding between segments is junk
extern char etext[], srodata[], erodata[];

static struct {
  const uchar *at;
  uint64_t len;
  uint32_t sum;
} piece[PIECES];
static int pieces;

static void cut(const char *lo, const char *hi) {
  const char *p;

  for (p = lo; p < hi && pieces < PIECES; p += PIECE) {
    piece[pieces].at = (const uchar *)p;
    piece[pieces].len = (uint64_t)(hi - p) < PIECE ? (uint64_t)(hi - p) : PIECE;
    pieces++;
  }
}

static uint32_t piece_sum(int i) { return ulog_crc(0, piece[i].at, piece[i].len); }

void kernel_text_init(void) {
  int i;

  cut((const char *)KERNLINK, etext);
  cut(srodata, erodata);
  for (i = 0; i < pieces; i++)
    piece[i].sum = piece_sum(i);
}

// the text and read-only data as loaded, one sum
uint32_t kernel_text_crc(void) {
  return ulog_crc(ulog_crc(0, (const uchar *)KERNLINK, (uint64_t)(etext - (char *)KERNLINK)),
                  (const uchar *)srodata, (uint64_t)(erodata - srodata));
}

// pieces that changed since boot; how many, the first
int kernel_text_check(const char *when, uint64_t *first) {
  int i, changed = 0;

  for (i = 0; i < pieces; i++)
    if (piece_sum(i) != piece[i].sum) {
      if (changed == 0 && first != 0)
        *first = (uint64_t)(uintptr_t)piece[i].at;
      if (changed++ < 8)
        printf("kernel: text or read-only data at 0x%lx..0x%lx changed %s\n",
               (unsigned long)(uintptr_t)piece[i].at,
               (unsigned long)((uintptr_t)piece[i].at + piece[i].len), when);
    }
  return changed;
}

static int64_t checked(void *arg) {
  (void)arg;
  return kernel_text_check("since boot", 0);
}

// one piece's sum now, vs the kernel file
static int piece_asked;

static int64_t piece_now(void *arg) {
  (void)arg;
  return piece_asked >= 0 && piece_asked < pieces ? (int64_t)piece_sum(piece_asked) : -1;
}

// 8 bytes of text at an offset, vs the file
static int peek_at;

static int64_t peek(void *arg) {
  uint64_t v;

  (void)arg;
  if (peek_at < 0 || (uint64_t)peek_at + 8 > (uint64_t)(erodata - (char *)KERNLINK))
    return -1;
  memmove(&v, (const uchar *)KERNLINK + peek_at, 8);
  return (int64_t)v;
}

void kernel_text_sysctl(void) {
  sysctl_add_int("debug.kernel_text_at", &peek_at, FNU_CTLF_WRITE, 0, 0x7fffffff);
  sysctl_add_quad_fn("debug.kernel_text_word", peek, 0);
  sysctl_add_quad_fn("debug.kernel_text", checked, 0);
  sysctl_add_int("debug.kernel_text_piece", &piece_asked, FNU_CTLF_WRITE, 0, PIECES - 1);
  sysctl_add_quad_fn("debug.kernel_text_piece_sum", piece_now, 0);
}
