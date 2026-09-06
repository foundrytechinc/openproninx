/*
 * FNU/OpenProninx fnudf -- Report disk and memory usage (df-style).
 *
 * Part of the FNU (Foundry is Not Unix) userland.  Queries SYS_df to
 * obtain per-volume usage for FNU Root, FNU Data, and RAM, then prints
 * a human-readable summary table mirroring `df -h`.
 */
#include "user/user.h"
#include "inc/product.h"

#define NENTRIES DF_ENTRIES_MAX
#define LENLABEL  20
#define LENMOUNT  18
#define LENSIZE   10
#define LENPCT    6

/* Human-friendly byte formatting -- returns one of B/KiB/MiB/GiB.
 * Writes into `out` (caller must provide at least 16 chars). */
static char *human(uint64_t v, char *out) {
  static const char units[] = {'B', 'K', 'M', 'G', 'T'};
  uint64_t whole, frac;
  int u = 0;
  whole = v;
  frac  = 0;
  while (whole >= 1024 && u < 4) {
    frac  = whole % 1024;
    whole /= 1024;
    u++;
  }
  /* Output as "NNN.Nxu" or "NNNxu" depending on magnitude. */
  if (u == 0) {
    int i = 15, n;
    out[i] = 0;
    if (whole == 0) { out[--i] = '0'; }
    while (whole > 0) { out[--i] = '0' + (whole % 10); whole /= 10; }
    n = 15 - i;
    safestrcpy(out, out + i, 16 - n);
    out[n++] = units[0];
    out[n]   = 0;
  } else {
    int d0, d1, n = 0;
    d0 = (int)((frac * 100ULL) / 1024ULL / 10);
    d1 = (int)((frac * 100ULL) / 1024ULL % 10);
    /* whole part */
    if (whole == 0) { out[n++] = '0'; }
    else {
      char tmp[16]; int k = 0;
      while (whole > 0) { tmp[k++] = '0' + (whole % 10); whole /= 10; }
      while (k > 0) out[n++] = tmp[--k];
    }
    out[n++] = '.';
    out[n++] = '0' + d0;
    out[n++] = '0' + d1;
    out[n++] = units[u];
    out[n]   = 0;
  }
  return out;
}

static void put_pad(const char *s, int width, int left) {
  int n = strlen(s);
  if (!left) while (width-- > n) putchar(' ');
  while (n-- > 0) putchar(*s++);
  if (left)  while (width-- > (int)strlen("")) { /* pad right to min width? nop */ break; }
}

static void put_bar(int pct) {
  int i, blocks = pct * 20 / 100;
  putchar('[');
  for (i = 0; i < 20; i++)
    putchar(i < blocks ? '#' : ' ');
  printf("] %3d%%", pct);
}

static int cmp_label(const void *a, const void *b) {
  const struct df_stat *x = a, *y = b;
  /* RAM comes last; volumes come sorted by mount then label. */
  int ra = (strncmp(x->label, "RAM", 3) == 0);
  int rb = (strncmp(y->label, "RAM", 3) == 0);
  if (ra != rb) return ra - rb;
  return strcmp(x->label, y->label);
}

int main(int argc, char **argv) {
  struct df_stat entries[NENTRIES];
  struct df_stat tmp[NENTRIES];
  int n, i, valid = 0;
  (void)argc; (void)argv;

  memset(entries, 0, sizeof(entries));
  n = df(entries, NENTRIES);
  if (n < 0) {
    dprintf(2, "fnudf: kernel SYS_df failed or unsupported\n");
    dprintf(2, "       Recompile kernel with FNU userland support.\n");
    exit();
  }
  /* Compact only the present entries so the table isn't padded with
   * zero-filled placeholders. */
  for (i = 0; i < NENTRIES; i++) {
    if (entries[i].is_present)
      tmp[valid++] = entries[i];
  }
  if (valid == 0) {
    printf("fnudf: no storage volumes reported by the kernel.\n");
    exit();
  }
  /* Lightweight sort using memmove (no qsort in FNU libc). */
  for (i = 1; i < valid; i++) {
    int j = i;
    while (j > 0 && cmp_label(&tmp[j - 1], &tmp[j]) > 0) {
      struct df_stat sw = tmp[j - 1];
      tmp[j - 1] = tmp[j];
      tmp[j]     = sw;
      j--;
    }
  }

  printf("FNU Disk Free -- %s %s\n", FNU_PRODUCT_NAME, FNU_PRODUCT_VERSION);
  printf("%-*s  %*s  %*s  %*s  %-4s  %-*s  %s\n",
         LENLABEL, "Filesystem",
         LENSIZE,  "Size",
         LENSIZE,  "Used",
         LENSIZE,  "Avail",
         "Use%",
         26,       "Usage",
         "Mount");
  for (i = 0; i < valid; i++) {
    char size[24], used[24], avail[24];
    uint64_t total = tmp[i].total_bytes;
    uint64_t u     = tmp[i].used_bytes;
    uint64_t f     = tmp[i].free_bytes;
    int pct = (total > 0) ? (int)((u * 100ULL + (total - 1) / 2) / total) : 0;
    if (pct > 100) pct = 100;
    printf("%-*s  %*s  %*s  %*s  %3d%%  ",
           LENLABEL, tmp[i].label,
           LENSIZE,  human(total, size),
           LENSIZE,  human(u, used),
           LENSIZE,  human(f, avail),
           pct);
    put_bar(pct);
    printf("  %s", tmp[i].mount_point);
    if (tmp[i].is_read_only)
      printf("  (ro)");
    printf("\n");
  }
  exit();
}
