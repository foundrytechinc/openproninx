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

static void print_palette(void) {
  printf("\033[1;30m########################\033[0m ");
  printf("\033[1;31m########################\033[0m ");
  printf("\033[1;32m########################\033[0m ");
  printf("\033[1;33m########################\033[0m\n");
  printf("\033[1;34m########################\033[0m ");
  printf("\033[1;35m########################\033[0m ");
  printf("\033[1;36m########################\033[0m ");
  printf("\033[1;37m########################\033[0m\n\n");
}

int main(void) {
  struct info sys;
  char user_name[USER_NAME_MAX];
  char uptime_str[64];
  char total_mem[32];
  char free_mem[32];
  char used_mem[32];
  char nprocs[16];

  if (info(&sys) < 0) {
    dprintf(2, "fnufetch: unable to query system information\n");
    exit();
  }

  get_user_name(user_name, sizeof(user_name));
  format_uptime(sys.uptime, uptime_str, sizeof(uptime_str));
  format_mem(sys.total_ram, total_mem, sizeof(total_mem));
  format_mem(sys.free_ram, free_mem, sizeof(free_mem));
  if (sys.total_ram > sys.free_ram) {
    format_mem(sys.total_ram - sys.free_ram, used_mem, sizeof(used_mem));
  } else {
    safestrcpy(used_mem, "0 B", sizeof(used_mem));
  }
  itoap((int)sys.nprocs, nprocs, 1);

  printf("\033[33m");
  printf("       _/\\                     \033[32m%s\033[0m@\033[32mPSH\033[0m\n", user_name);
  printf("\033[33m     _/   \\_______              \033[34m-----------------------\033[0m\n");
  printf("\033[33m   _/  \\_ /  _    \\___          \033[1;33mOS:\033[0m       %s\n", FNU_PRODUCT_NAME);
  printf("\033[33m  / \\_   \\\\_/ \\       \\         \033[1;33mKernel:\033[0m   %s\n", FNU_PRODUCT_VERSION);
  printf("\033[33m /    \\   \\|< >|      _\\        \033[1;33mShell:\033[0m    PSH (Proninx Shell, FNU)\n");
  printf("\033[33m|      \\   \\\\_/      / |        \033[1;33mArch:\033[0m     x86_64 (amd64)\n");
  printf("\033[33m|       \\   \\        \\_|        \033[1;33mUptime:\033[0m   %s\n", uptime_str);
  printf("\033[33m \\       \\   \\        /         \033[1;33mProcs:\033[0m    %s\n", nprocs);
  printf("\033[33m  \\       \\   \\______/          \033[1;33mMemory:\033[0m   %s used / %s total\n", used_mem, total_mem);
  printf("\033[33m   \\_______\\__/                 \033[1;33mFree RAM:\033[0m %s\n", free_mem);
  printf("                                \033[1;33mVendor:\033[0m   %s\n", FNU_PRODUCT_VENDOR);
  printf("                                \033[1;33mLicense:\033[0m  BSD 3-Clause\n");
  printf("                                \033[1;33mUserland:\033[0m FNU (Foundry is Not Unix)\n");
  printf("                                \033[1;33mFNU Version:\033[0m   %s\n", FNU_VERSION);

  print_palette();

  exit();
}
