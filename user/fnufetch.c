#include <libgen.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mount.h>
#include <fnu/sys.h>
#include <util.h>
#include "inc/product.h"

static void padzero(char *s) {
  int len = strlen(s);
  if (len < 2) {
    s[1] = s[0];
    s[0] = '0';
    s[2] = 0;
  }
}

static void itoap(int value, char *buf, int width) {
  static const char digits[] = "0123456789";
  char tmp[16];
  int n = 0;
  int isneg = 0;
  unsigned int v;

  if (value < 0) {
    isneg = 1;
    v = (unsigned int)(-value);
  } else {
    v = (unsigned int)value;
  }

  if (v == 0) {
    tmp[n++] = '0';
  } else {
    while (v > 0) {
      tmp[n++] = digits[v % 10];
      v /= 10;
    }
  }

  while (n < width) {
    tmp[n++] = '0';
  }
  if (isneg) {
    tmp[n++] = '-';
  }

  int out = 0;
  while (n > 0) {
    buf[out++] = tmp[--n];
  }
  buf[out] = 0;
}

static void get_user_name(char *buf, size_t len) {
  struct passwd *account;

  if ((account = getpwuid(getuid())) != 0)
    strlcpy(buf, account->pw_name, len);
  else
    strlcpy(buf, "unknown", len);
}

static void get_shell(char *buf, size_t len) {
  struct passwd *account;
  const char *path = getenv("SHELL");

  if ((path == 0 || *path == 0) && (account = getpwuid(getuid())) != 0)
    path = account->pw_shell;
  if (path == 0 || *path == 0)
    path = "unknown";
  strlcpy(buf, path, len);
  strlcpy(buf, basename(buf), len);
}

static void format_uptime(uint64 ticks, char *buf, size_t len) {
  uint64 total_seconds = ticks / FNU_HZ;
  uint64 days = total_seconds / 86400;
  uint64 hours = (total_seconds % 86400) / 3600;
  uint64 minutes = (total_seconds % 3600) / 60;
  uint64 seconds = total_seconds % 60;

  char hh[8], mm[8], ss[8];
  itoap((int)hours, hh, 2);
  itoap((int)minutes, mm, 2);
  itoap((int)seconds, ss, 2);
  padzero(hh);
  padzero(mm);
  padzero(ss);

  char d[16];
  itoap((int)days, d, 1);

  int pos = 0;
  if (days > 0) {
    strlcpy(buf + pos, d, len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, " day", len - pos);
    pos = strlen(buf);
    if (days != 1) {
      strlcpy(buf + pos, "s", len - pos);
      pos = strlen(buf);
    }
    strlcpy(buf + pos, " ", len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, hh, len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, ":", len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, mm, len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, ":", len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, ss, len - pos);
  } else if (hours > 0) {
    strlcpy(buf + pos, hh, len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, ":", len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, mm, len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, ":", len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, ss, len - pos);
  } else {
    strlcpy(buf + pos, mm, len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, ":", len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, ss, len - pos);
  }
}

static void format_mem_fixed(uint64 whole, uint64 frac, const char *unit,
                             char *buf, size_t len) {
  char w[16], f[16];
  itoap((int)whole, w, 1);
  itoap((int)frac, f, 2);
  if (strlen(f) < 2) {
    if (strlen(f) == 0) {
      f[0] = '0'; f[1] = '0'; f[2] = 0;
    } else {
      f[1] = f[0]; f[0] = '0'; f[2] = 0;
    }
  }
  int pos = 0;
  strlcpy(buf + pos, w, len - pos);
  pos = strlen(buf);
  strlcpy(buf + pos, ".", len - pos);
  pos = strlen(buf);
  strlcpy(buf + pos, f, len - pos);
  pos = strlen(buf);
  strlcpy(buf + pos, " ", len - pos);
  pos = strlen(buf);
  strlcpy(buf + pos, unit, len - pos);
}

static void format_mem(uint64 bytes, char *buf, size_t len) {
  if (bytes >= (1024ULL * 1024 * 1024)) {
    uint64 unit = 1024ULL * 1024 * 1024;
    format_mem_fixed(bytes / unit,
                     ((bytes % unit) * 100) / unit, "GiB", buf, len);
  } else if (bytes >= (1024ULL * 1024)) {
    uint64 unit = 1024ULL * 1024;
    format_mem_fixed(bytes / unit,
                     ((bytes % unit) * 100) / unit, "MiB", buf, len);
  } else if (bytes >= 1024) {
    char k[16];
    itoap((int)(bytes / 1024), k, 1);
    int pos = 0;
    strlcpy(buf + pos, k, len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, " KiB", len - pos);
  } else {
    char b[16];
    itoap((int)bytes, b, 1);
    int pos = 0;
    strlcpy(buf + pos, b, len - pos);
    pos = strlen(buf);
    strlcpy(buf + pos, " B", len - pos);
  }
}

static void cat(char *b, size_t len, const char *s) {
  strlcpy(b + strlen(b), s, len - strlen(b));
}

#define OUT_MAX 8192
static char out[OUT_MAX];
static int out_len;

static void out_str(const char *s) {
  while (*s && out_len < OUT_MAX - 1)
    out[out_len++] = *s++;
}

static void out_pad(int n) {
  while (n-- > 0 && out_len < OUT_MAX - 1)
    out[out_len++] = ' ';
}

static void out_flush(void) {
  if (out_len > 0)
    write(1, out, out_len);
  out_len = 0;
}

static void print_palette(int columns) {
  static const char *colour[] = {"30", "31", "32", "33",
                                "34", "35", "36", "37"};
  int i, j, block = (columns - 3) / 4;

  if (block < 4)
    block = 4;
  if (block > 24)
    block = 24;
  for (i = 0; i < 8; i++) {
    out_str("\033[1;");
    out_str(colour[i]);
    out_str("m");
    for (j = 0; j < block; j++)
      out_str("#");
    out_str("\033[0m");
    out_str(i == 3 || i == 7 ? "\n" : " ");
  }
  out_str("\n");
}

static const char *logo_wide[] = {
    "",
    "                   _-~\\_",
    "        )~~~~\\_    )\\)(/\\-----\\____",
    "       |  \"\\). ~-/~              \"~\"~--__",
    "       )   /',(                         `\\-",
    "        \\\\_//`     _,_,-)            (o)  ~\\",
    "        _/~`        \"~~`              `-'   \\_",
    "      _)`                                    (\\",
    "     _/                                   |  )|",
    "    _(                                    |  /|",
    "   ,^                                    _( _ /",
    " _/'                             ___  __/~_.-'",
    "_(                                 ~~~^   `-'",
    "'                                   ___/~`",
    "                               /^(~/~\\\\",
    "                                      _\\",
    "                                       )",
    "                                       )",
    0,
};

static const char *logo_small[] = {
    "             .-..-.",
    "           /'---'''----_",
    "          |             \\",
    "          //    --   (o) |",
    "         //              /",
    "       //            '--'",
    "      //               \\",
    "     /                 /",
    "   //                  |",
    "   /                   |",
    "  |                   |",
    " |                    |",
    " |                    |",
    " \\\\                _ /",
    "  \\\\        _-\\ _ -'||_",
    "   '-------'  '''\\_-\\-'",
    0,
};

static void format_hz(uint64 hz, char *buf, size_t len) {
  char a[16], b[16];

  if (hz == 0) {
    strlcpy(buf, "unknown", len);
    return;
  }
  if (hz >= 1000000000ULL) {
    itoap((int)(hz / 1000000000ULL), a, 1);
    itoap((int)((hz % 1000000000ULL) / 10000000ULL), b, 2);
    if (strlen(b) < 2) { b[1] = b[0]; b[0] = '0'; b[2] = 0; }
    strlcpy(buf, a, len);
    strlcpy(buf + strlen(buf), ".", len - strlen(buf));
    strlcpy(buf + strlen(buf), b, len - strlen(buf));
    strlcpy(buf + strlen(buf), " GHz", len - strlen(buf));
  } else {
    itoap((int)(hz / 1000000ULL), a, 1);
    strlcpy(buf, a, len);
    strlcpy(buf + strlen(buf), " MHz", len - strlen(buf));
  }
}

static int brand_clock(const char *brand, char *buf, size_t len) {
  int i, whole, frac, digits;

  for (i = 0; brand[i]; i++) {
    if (brand[i] != '@')
      continue;
    i++;
    while (brand[i] == ' ')
      i++;
    if (brand[i] < '0' || brand[i] > '9')
      return -1;
    for (whole = 0; brand[i] >= '0' && brand[i] <= '9'; i++)
      whole = whole * 10 + (brand[i] - '0');
    frac = 0;
    digits = 0;
    if (brand[i] == '.')
      for (i++; brand[i] >= '0' && brand[i] <= '9'; i++, digits++)
        frac = frac * 10 + (brand[i] - '0');
    while (digits < 2) { frac *= 10; digits++; }
    while (digits > 2) { frac /= 10; digits--; }
    if (brand[i] == 'G' || brand[i] == 'g') {
      char a[16], b[16];
      itoap(whole, a, 1);
      itoap(frac, b, 2);
      buf[0] = 0;
      cat(buf, len, a);
      cat(buf, len, ".");
      cat(buf, len, b);
      cat(buf, len, " GHz");
      return 0;
    }
    if (brand[i] == 'M' || brand[i] == 'm') {
      char a[16];
      itoap(whole, a, 1);
      buf[0] = 0;
      cat(buf, len, a);
      cat(buf, len, " MHz");
      return 0;
    }
    return -1;
  }
  return -1;
}

static void tidy_brand(const char *brand, char *buf, size_t len) {
  static const char *mark[] = {"(R)", "(TM)", "(r)", "(tm)"};
  static const char *noun[] = {" CPU", " Processor"};
  size_t i, k, l, n = 0;
  int skipped;

  for (i = 0; brand[i] && n + 1 < len; i++) {
    skipped = 0;
    for (k = 0; k < sizeof(mark) / sizeof(mark[0]); k++) {
      l = strlen(mark[k]);
      if (strncmp(brand + i, mark[k], l) == 0) {
        i += l - 1;
        skipped = 1;
        break;
      }
    }
    for (k = 0; !skipped && k < sizeof(noun) / sizeof(noun[0]); k++) {
      l = strlen(noun[k]);
      if (strncmp(brand + i, noun[k], l) != 0)
        continue;
      if (brand[i + l] == 0 || strncmp(brand + i + l, " @", 2) == 0) {
        i += l - 1;
        skipped = 1;
      }
    }
    if (skipped)
      continue;
    if (brand[i] == '@')
      break;
    if (brand[i] == ' ' && (n == 0 || buf[n - 1] == ' '))
      continue;
    buf[n++] = brand[i];
  }
  while (n > 0 && buf[n - 1] == ' ')
    n--;
  buf[n] = 0;
  if (n == 0)
    strlcpy(buf, brand, len);
}

static void num(int v, char *buf, size_t len) {
  char t[16];
  itoap(v, t, 1);
  strlcpy(buf, t, len);
}

#define FIELDS_MAX 28
static char fkey[FIELDS_MAX][16];
static char fval[FIELDS_MAX][112];
static int nfields;

static void field(const char *k, const char *v) {
  if (nfields >= FIELDS_MAX)
    return;
  strlcpy(fkey[nfields], k, sizeof(fkey[0]));
  strlcpy(fval[nfields], v, sizeof(fval[0]));
  nfields++;
}

static const char *fill_colour(int pct) {
  if (pct >= 90)
    return "\033[1;31m";
  if (pct >= 75)
    return "\033[1;33m";
  return "\033[1;32m";
}

static int percent(uint64 used, uint64 total) {
  if (total == 0 || used > total)
    return total == 0 ? 0 : 100;
  return (int)((used * 100) / total);
}

static void gauge(int pct, char *b, size_t len) {
  char n[16];
  int i, filled = (pct * 10) / 100;

  cat(b, len, fill_colour(pct));
  itoap(pct, n, 1);
  if (pct < 10) cat(b, len, "  ");
  else if (pct < 100) cat(b, len, " ");
  cat(b, len, n);
  cat(b, len, "% [");
  for (i = 0; i < 10; i++)
    cat(b, len, i < filled ? "#" : "-");
  cat(b, len, "]\033[0m");
}

int main(void) {
  struct fnu_info sys;
  struct fnu_hwinfo hw;
  struct fnu_diskinfo *disks = 0;
  struct fnu_fsstat *mounts = 0;
  char user_name[FNU_USER_NAME_MAX];
  char buf[96], tmp[64], tmp2[64];
  const char **logo;
  int have_hw, ndisks, nmounts, i, logo_w, line, logo_n;

  if (fnu_info(&sys) < 0) {
    dprintf(2, "fnufetch: unable to query system information\n");
    exit(1);
  }
  have_hw = fnu_hwinfo(&hw) == 0;
  if ((ndisks = fnu_disks(&disks)) < 0)
    ndisks = 0;
  if ((nmounts = fnu_volumes(&mounts)) < 0)
    nmounts = 0;
  get_user_name(user_name, sizeof(user_name));

  field("OS", FNU_PRODUCT_NAME);
  field("Kernel", FNU_PRODUCT_VERSION);
  get_shell(tmp, sizeof(tmp));
  field("Shell", tmp);
  field("Arch", "x86_64 (amd64)");

  if (have_hw) {
    tidy_brand(hw.cpu_brand[0] ? hw.cpu_brand : hw.cpu_vendor, buf,
               sizeof(buf));
    num((int)hw.cpu_count, tmp, sizeof(tmp));
    strlcpy(buf + strlen(buf), " (", sizeof(buf) - strlen(buf));
    strlcpy(buf + strlen(buf), tmp, sizeof(buf) - strlen(buf));
    strlcpy(buf + strlen(buf), hw.cpu_count == 1 ? " core)" : " cores)",
               sizeof(buf) - strlen(buf));
    field("CPU", buf);

    if (hw.cpu_base_hz != 0)
      format_hz(hw.cpu_base_hz, tmp, sizeof(tmp));
    else if (brand_clock(hw.cpu_brand, tmp, sizeof(tmp)) < 0)
      format_hz(hw.tsc_hz, tmp, sizeof(tmp));
    if (hw.cpu_max_hz > hw.cpu_base_hz && hw.cpu_base_hz != 0) {
      format_hz(hw.cpu_max_hz, tmp2, sizeof(tmp2));
      cat(tmp, sizeof(tmp), " base, ");
      cat(tmp, sizeof(tmp), tmp2);
      cat(tmp, sizeof(tmp), " turbo");
    }
    field("CPU Clock", tmp);
  }

  format_uptime(sys.uptime, tmp, sizeof(tmp));
  field("Uptime", tmp);
  num((int)sys.nprocs, tmp, sizeof(tmp));
  field("Procs", tmp);

  format_mem(sys.total_ram, tmp2, sizeof(tmp2));
  if (sys.total_ram > sys.free_ram)
    format_mem(sys.total_ram - sys.free_ram, tmp, sizeof(tmp));
  else
    strlcpy(tmp, "0 B", sizeof(tmp));
  strlcpy(buf, tmp, sizeof(buf));
  cat(buf, sizeof(buf), " / ");
  cat(buf, sizeof(buf), tmp2);
  cat(buf, sizeof(buf), "  ");
  gauge(percent(sys.total_ram - sys.free_ram, sys.total_ram), buf, sizeof(buf));
  field("Memory", buf);

  for (i = 0; i < nmounts; i++) {
    uint64 total = mounts[i].blocks * mounts[i].bsize;
    uint64 used = (mounts[i].blocks - mounts[i].bfree) * mounts[i].bsize;
    char key[16];

    // devfs holds no blocks
    if (total == 0)
      continue;
    format_mem(used, tmp, sizeof(tmp));
    strlcpy(buf, tmp, sizeof(buf));
    cat(buf, sizeof(buf), " / ");
    format_mem(total, tmp, sizeof(tmp));
    cat(buf, sizeof(buf), tmp);
    cat(buf, sizeof(buf), "  ");
    gauge(percent(used, total), buf, sizeof(buf));
    if (mounts[i].flags & FNU_MNT_RDONLY)
      cat(buf, sizeof(buf), " ro");
    strlcpy(key, mounts[i].on, sizeof(key));
    field(key, buf);
  }

  for (i = 0; i < ndisks; i++) {
    char key[16];
    strlcpy(buf, disks[i].model, sizeof(buf));
    format_mem(disks[i].sectors * (uint64)disks[i].sector_size, tmp,
               sizeof(tmp));
    strlcpy(buf + strlen(buf), ", ", sizeof(buf) - strlen(buf));
    strlcpy(buf + strlen(buf), tmp, sizeof(buf) - strlen(buf));
    if (disks[i].link_gen > 0) {
      static const char *gen[] = {"", " SATA-I", " SATA-II", " SATA-III"};
      if (disks[i].link_gen <= 3)
        strlcpy(buf + strlen(buf), gen[disks[i].link_gen],
                   sizeof(buf) - strlen(buf));
    }
    strlcpy(key, "Disk", sizeof(key));
    if (ndisks > 1) {
      num(i, tmp, sizeof(tmp));
      strlcpy(key + 4, tmp, sizeof(key) - 4);
    }
    field(key, buf);
  }

  if (have_hw) {
    num((int)hw.fb_width, tmp, sizeof(tmp));
    strlcpy(buf, tmp, sizeof(buf));
    strlcpy(buf + strlen(buf), "x", sizeof(buf) - strlen(buf));
    num((int)hw.fb_height, tmp, sizeof(tmp));
    strlcpy(buf + strlen(buf), tmp, sizeof(buf) - strlen(buf));
    strlcpy(buf + strlen(buf), " @ ", sizeof(buf) - strlen(buf));
    num((int)hw.fb_bpp, tmp, sizeof(tmp));
    strlcpy(buf + strlen(buf), tmp, sizeof(buf) - strlen(buf));
    strlcpy(buf + strlen(buf), " bpp", sizeof(buf) - strlen(buf));
    if (hw.fb_vram) {
      format_mem(hw.fb_vram, tmp, sizeof(tmp));
      cat(buf, sizeof(buf), ", ");
      cat(buf, sizeof(buf), tmp);
      cat(buf, sizeof(buf), " vram");
    }
    cat(buf, sizeof(buf), hw.fb_modeset ? "" : " (fixed)");
    field("Display", buf);

    num((int)hw.fb_columns, tmp, sizeof(tmp));
    strlcpy(buf, tmp, sizeof(buf));
    strlcpy(buf + strlen(buf), " x ", sizeof(buf) - strlen(buf));
    num((int)hw.fb_rows, tmp, sizeof(tmp));
    strlcpy(buf + strlen(buf), tmp, sizeof(buf) - strlen(buf));
    strlcpy(buf + strlen(buf), " cells", sizeof(buf) - strlen(buf));
    field("Console", buf);

    num((int)hw.pci_total, buf, sizeof(buf));
    cat(buf, sizeof(buf), " PCI functions, ");
    num((int)hw.pci_devices, tmp, sizeof(tmp));
    cat(buf, sizeof(buf), tmp);
    cat(buf, sizeof(buf), " driven");
    field("Devices", buf);
  }

  field("Vendor", FNU_PRODUCT_VENDOR);
  field("Userland", "FNU (Foundry is Not Unix)");
  field("FNU Version", FNU_VERSION);

  if (have_hw && hw.fb_columns >= 110) {
    logo = logo_wide;
    logo_w = 54;
  } else {
    logo = logo_small;
    logo_w = 28;
  }

  for (logo_n = 0; logo[logo_n] != 0; logo_n++)
    ;

  out_str("\n");
  for (line = 0;; line++) {
    const char *art = line < logo_n ? logo[line] : 0;

    if (art == 0 && line >= nfields + 2)
      break;
    if (art != 0) {
      out_str("\033[33m");
      out_str(art);
      out_str("\033[0m");
      out_pad(logo_w - (int)strlen(art));
    } else {
      out_pad(logo_w);
    }

    if (line == 0) {
      out_str("\033[1;32m");
      out_str(user_name);
      out_str("\033[0m@\033[1;32mPSH\033[0m");
    } else if (line == 1) {
      out_str("\033[34m--------------------------------\033[0m");
    } else if (line - 2 < nfields) {
      int k = line - 2;
      out_str("\033[1;33m");
      out_str(fkey[k]);
      out_str(":\033[0m");
      out_pad(12 - (int)strlen(fkey[k]));
      out_str(fval[k]);
    }
    out_str("\n");
  }
  out_str("\n");
  print_palette(have_hw ? (int)hw.fb_columns : 80);
  out_flush();
  fnu_table_free(disks);
  fnu_table_free(mounts);
  exit(0);
}
