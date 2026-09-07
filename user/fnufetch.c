#include "user/user.h"
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
  struct user_info info;
  int index;
  int uid = getuid();
  for (index = 0; users(&info, index) == 0; index++) {
    if (info.uid == (uint)uid) {
      safestrcpy(buf, info.name, len);
      return;
    }
  }
  safestrcpy(buf, "unknown", len);
}

static void format_uptime(uint64 ticks, char *buf, size_t len) {
  uint64 total_seconds = ticks / 100;
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
    safestrcpy(buf + pos, d, len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, " day", len - pos);
    pos = strlen(buf);
    if (days != 1) {
      safestrcpy(buf + pos, "s", len - pos);
      pos = strlen(buf);
    }
    safestrcpy(buf + pos, " ", len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, hh, len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, ":", len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, mm, len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, ":", len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, ss, len - pos);
  } else if (hours > 0) {
    safestrcpy(buf + pos, hh, len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, ":", len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, mm, len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, ":", len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, ss, len - pos);
  } else {
    safestrcpy(buf + pos, mm, len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, ":", len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, ss, len - pos);
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
  safestrcpy(buf + pos, w, len - pos);
  pos = strlen(buf);
  safestrcpy(buf + pos, ".", len - pos);
  pos = strlen(buf);
  safestrcpy(buf + pos, f, len - pos);
  pos = strlen(buf);
  safestrcpy(buf + pos, " ", len - pos);
  pos = strlen(buf);
  safestrcpy(buf + pos, unit, len - pos);
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
    safestrcpy(buf + pos, k, len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, " KiB", len - pos);
  } else {
    char b[16];
    itoap((int)bytes, b, 1);
    int pos = 0;
    safestrcpy(buf + pos, b, len - pos);
    pos = strlen(buf);
    safestrcpy(buf + pos, " B", len - pos);
  }
}

static void cat(char *b, size_t len, const char *s) {
  safestrcpy(b + strlen(b), s, len - strlen(b));
}

// The console repaints on every write(), so the whole report goes out at once.
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

// four blocks per row, sized to whatever console we ended up on
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
    safestrcpy(buf, "unknown", len);
    return;
  }
  if (hz >= 1000000000ULL) {
    itoap((int)(hz / 1000000000ULL), a, 1);
    itoap((int)((hz % 1000000000ULL) / 10000000ULL), b, 2);
    if (strlen(b) < 2) { b[1] = b[0]; b[0] = '0'; b[2] = 0; }
    safestrcpy(buf, a, len);
    safestrcpy(buf + strlen(buf), ".", len - strlen(buf));
    safestrcpy(buf + strlen(buf), b, len - strlen(buf));
    safestrcpy(buf + strlen(buf), " GHz", len - strlen(buf));
  } else {
    itoap((int)(hz / 1000000ULL), a, 1);
    safestrcpy(buf, a, len);
    safestrcpy(buf + strlen(buf), " MHz", len - strlen(buf));
  }
}

// the brand string carries the part's rated clock. calibration lands a few
// megahertz off it, and the number on the box is the one people recognise
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

// what the fetches all do: drop the trademark noise and the clock suffix, so
// the model is a model and not a paragraph
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
    // "CPU" and "Processor" are only noise where they trail the model
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
    safestrcpy(buf, brand, len);
}

static void num(int v, char *buf, size_t len) {
  char t[16];
  itoap(v, t, 1);
  safestrcpy(buf, t, len);
}

#define FIELDS_MAX 28
static char fkey[FIELDS_MAX][16];
static char fval[FIELDS_MAX][112];
static int nfields;

static void field(const char *k, const char *v) {
  if (nfields >= FIELDS_MAX)
    return;
  safestrcpy(fkey[nfields], k, sizeof(fkey[0]));
  safestrcpy(fval[nfields], v, sizeof(fval[0]));
  nfields++;
}

// green while there is room, yellow when it is filling, red when it is not
static const char *fill_colour(int pct) {
  if (pct >= 90)
    return "\033[1;31m";
  if (pct >= 75)
    return "\033[1;33m";
  return "\033[1;32m";
}

static int percent(uint64 used, uint64 total) {
  if (total == 0)
    return 0;
  return (int)((used * 100) / total);
}

// "  47% [####------]" in the colour the level deserves
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
  struct info sys;
  struct hwinfo hw;
  struct diskinfo disks[DISK_ENTRIES_MAX];
  struct df_stat mounts[DF_ENTRIES_MAX];
  char user_name[USER_NAME_MAX];
  char buf[96], tmp[64], tmp2[64];
  const char **logo;
  int have_hw, ndisks, nmounts, i, logo_w, line, logo_n;

  if (info(&sys) < 0) {
    dprintf(2, "fnufetch: unable to query system information\n");
    exit();
  }
  have_hw = hwinfo(&hw) == 0;
  ndisks = diskinfo(disks, DISK_ENTRIES_MAX);
  if (ndisks < 0)
    ndisks = 0;
  nmounts = df(mounts, DF_ENTRIES_MAX);
  if (nmounts < 0)
    nmounts = 0;
  get_user_name(user_name, sizeof(user_name));

  field("OS", FNU_PRODUCT_NAME);
  field("Kernel", FNU_PRODUCT_VERSION);
  field("Shell", "PSH (Proninx Shell, FNU)");
  field("Arch", "x86_64 (amd64)");

  if (have_hw) {
    tidy_brand(hw.cpu_brand[0] ? hw.cpu_brand : hw.cpu_vendor, buf,
               sizeof(buf));
    num((int)hw.cpu_count, tmp, sizeof(tmp));
    safestrcpy(buf + strlen(buf), " (", sizeof(buf) - strlen(buf));
    safestrcpy(buf + strlen(buf), tmp, sizeof(buf) - strlen(buf));
    safestrcpy(buf + strlen(buf), hw.cpu_count == 1 ? " core)" : " cores)",
               sizeof(buf) - strlen(buf));
    field("CPU", buf);

    // the rated clock, and the turbo ceiling when the part admits to one
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
    safestrcpy(tmp, "0 B", sizeof(tmp));
  safestrcpy(buf, tmp, sizeof(buf));
  cat(buf, sizeof(buf), " / ");
  cat(buf, sizeof(buf), tmp2);
  cat(buf, sizeof(buf), "  ");
  gauge(percent(sys.total_ram - sys.free_ram, sys.total_ram), buf, sizeof(buf));
  field("Memory", buf);

  for (i = 0; i < nmounts; i++) {
    char key[16];
    if (!mounts[i].is_present || mounts[i].mount_point[0] == 0 ||
        mounts[i].mount_point[0] == '-')
      continue; /* df reports ram as a mount; Memory already covers it */
    format_mem(mounts[i].used_bytes, tmp, sizeof(tmp));
    safestrcpy(buf, tmp, sizeof(buf));
    cat(buf, sizeof(buf), " / ");
    format_mem(mounts[i].total_bytes, tmp, sizeof(tmp));
    cat(buf, sizeof(buf), tmp);
    cat(buf, sizeof(buf), "  ");
    gauge(percent(mounts[i].used_bytes, mounts[i].total_bytes), buf,
          sizeof(buf));
    if (mounts[i].is_read_only)
      cat(buf, sizeof(buf), " ro");
    safestrcpy(key, mounts[i].mount_point, sizeof(key));
    field(key, buf);
  }

  for (i = 0; i < ndisks; i++) {
    char key[16];
    safestrcpy(buf, disks[i].model, sizeof(buf));
    format_mem(disks[i].sectors * (uint64)disks[i].sector_size, tmp,
               sizeof(tmp));
    safestrcpy(buf + strlen(buf), ", ", sizeof(buf) - strlen(buf));
    safestrcpy(buf + strlen(buf), tmp, sizeof(buf) - strlen(buf));
    if (disks[i].link_gen > 0) {
      static const char *gen[] = {"", " SATA-I", " SATA-II", " SATA-III"};
      if (disks[i].link_gen <= 3)
        safestrcpy(buf + strlen(buf), gen[disks[i].link_gen],
                   sizeof(buf) - strlen(buf));
    }
    safestrcpy(key, "Disk", sizeof(key));
    if (ndisks > 1) {
      num(i, tmp, sizeof(tmp));
      safestrcpy(key + 4, tmp, sizeof(key) - 4);
    }
    field(key, buf);
  }

  if (have_hw) {
    num((int)hw.fb_width, tmp, sizeof(tmp));
    safestrcpy(buf, tmp, sizeof(buf));
    safestrcpy(buf + strlen(buf), "x", sizeof(buf) - strlen(buf));
    num((int)hw.fb_height, tmp, sizeof(tmp));
    safestrcpy(buf + strlen(buf), tmp, sizeof(buf) - strlen(buf));
    safestrcpy(buf + strlen(buf), " @ ", sizeof(buf) - strlen(buf));
    num((int)hw.fb_bpp, tmp, sizeof(tmp));
    safestrcpy(buf + strlen(buf), tmp, sizeof(buf) - strlen(buf));
    safestrcpy(buf + strlen(buf), " bpp", sizeof(buf) - strlen(buf));
    if (hw.fb_vram) {
      format_mem(hw.fb_vram, tmp, sizeof(tmp));
      cat(buf, sizeof(buf), ", ");
      cat(buf, sizeof(buf), tmp);
      cat(buf, sizeof(buf), " vram");
    }
    cat(buf, sizeof(buf), hw.fb_modeset ? "" : " (fixed)");
    field("Display", buf);

    num((int)hw.fb_columns, tmp, sizeof(tmp));
    safestrcpy(buf, tmp, sizeof(buf));
    safestrcpy(buf + strlen(buf), " x ", sizeof(buf) - strlen(buf));
    num((int)hw.fb_rows, tmp, sizeof(tmp));
    safestrcpy(buf + strlen(buf), tmp, sizeof(buf) - strlen(buf));
    safestrcpy(buf + strlen(buf), " cells", sizeof(buf) - strlen(buf));
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

  // a wide console gets the big capybara, a narrow one the compact body
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
  exit();
}
