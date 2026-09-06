/*
 * FNU/OpenProninx fnuman (man) — On-line system manual pager.
 *
 * Part of the FNU (Foundry is Not Unix) userland. Provides reference manuals
 * for commands, utilities, and system concepts (FNU, OpenProninx, ABI, UFS2).
 */
#include "user.h"
#include "inc/product.h"

struct man_entry {
  const char *name;
  const char *aliases;
  const char *section;
  const char *summary;
  const char *synopsis;
  const char *description;
  const char *see_also;
};

static const struct man_entry man_db[] = {
  /* -------------------------------------------------------------
   * System Concepts & Architecture
   * ---------------------------------------------------------- */
  {
    "fnu",
    "foundry",
    "Concepts",
    "Foundry is Not Unix — Userland Environment Specification",
    "man fnu",
    "FNU (Foundry is Not Unix) is the native userland environment designed by\n"
    "Foundry Tech Inc. for OpenProninx. It provides a lightweight, self-contained\n"
    "operating environment without heavy POSIX or GNU dependencies.\n\n"
    "Core FNU components:\n"
    "  * fnusvc     - Service supervisor managing daemons and shell sessions\n"
    "  * PSH        - Proninx Shell with command history and built-in management\n"
    "  * fnudf      - Filesystem and memory usage monitor\n"
    "  * fnudate    - RTC wall-clock reader\n"
    "  * fnufetch   - System summary and hardware banner\n"
    "  * fnuman     - Manual pager for commands and architecture\n\n"
    "FNU utilities are statically linked against the OpenProninx Stable Core ABI\n"
    "and embed directly into the root ramdisk image.",
    "openproninx, abi, psh, fnusvc"
  },
  {
    "openproninx",
    "proninx, kernel",
    "Concepts",
    "Native x86-64 operating system kernel",
    "man openproninx",
    "OpenProninx is a native 64-bit x86-64 operating system kernel under active development.\n"
    "It features a modular driver framework, PCI auto-probing, VirtIO-GPU\n"
    "accelerated framebuffer, Intel E1000 and VirtIO-Net networking via lwIP,\n"
    "SMP multi-core support, ACPI power management, and UFS2 filesystem support.\n\n"
    "The kernel provides the System Call ABI (int $0x30) upon which FNU runs.",
    "fnu, abi, ufs2, fbset"
  },
  {
    "abi",
    "syscall, syscalls",
    "Concepts",
    "OpenProninx Application Binary Interface specification",
    "man abi",
    "OpenProninx uses a 64-bit system call convention via software interrupt 'int $0x30'.\n\n"
    "Calling convention:\n"
    "  * Syscall number: RAX\n"
    "  * Arguments: RDI, RSI, RDX, RCX, R8, R9\n"
    "  * Return value: RAX (-1 on error)\n\n"
    "ABI Tiers:\n"
    "  * Stable Core ABI: 53 system calls with frozen numbers and data structures,\n"
    "    including process management, file I/O, networking (slot-handle API),\n"
    "    terminal ioctl (TCGETS/TCSETS, FBIOGET/PUT_VSCREENINFO), rtctime, and df.\n"
    "  * Experimental ABI: In-kernel authentication (login, doas_auth, useradd,\n"
    "    passwd, users).",
    "openproninx, fnu"
  },
  {
    "ufs2",
    "data, ffs",
    "Concepts",
    "Unix File System 2 data volume support",
    "man ufs2",
    "OpenProninx includes read/write support for UFS2/FFS filesystem images.\n"
    "When an auxiliary storage device is attached, it is mounted at '/data'.\n"
    "The root filesystem ('/') resides in the embedded ramdisk.",
    "fnudf, openproninx"
  },

  /* -------------------------------------------------------------
   * FNU Userland Utilities
   * ---------------------------------------------------------- */
  {
    "fnudf",
    "df",
    "FNU Utilities",
    "Report filesystem and memory usage summary",
    "fnudf",
    "fnudf queries the kernel SYS_df ABI to display formatted usage tables\n"
    "for all present filesystems (FNU Root, FNU Data) as well as total physical RAM,\n"
    "complete with human-friendly unit formatting (B/K/M/G) and progress bars.",
    "fnufetch, fnudate, ufs2"
  },
  {
    "fnudate",
    "date",
    "FNU Utilities",
    "Read and display the CMOS Real-Time Clock wall time",
    "fnudate",
    "fnudate invokes the SYS_rtctime system call to snapshot the hardware RTC\n"
    "registers and formats the current date and time in UTC/host local format.",
    "fnufetch, fnudf"
  },
  {
    "fnufetch",
    "info",
    "FNU Utilities",
    "Pretty system information and hardware banner",
    "fnufetch",
    "fnufetch displays the OpenProninx ASCII emblem alongside OS version, kernel\n"
    "architecture, system uptime, active process count, and RAM utilization.",
    "fnudf, fnudate, status"
  },
  {
    "fbset",
    "resolution, display",
    "FNU Utilities",
    "Inspect or change framebuffer display resolution",
    "fbset [WIDTHxHEIGHT | WIDTH HEIGHT]",
    "fbset interacts with the display controller via ioctl FBIOGET_VSCREENINFO\n"
    "and FBIOPUT_VSCREENINFO.\n\n"
    "When called with no arguments, fbset reports the active resolution.\n"
    "When given dimensions (e.g., 'fbset 1024x768' or 'fbset 1280 720'), fbset\n"
    "dynamically resizes the VirtIO-GPU or VBE framebuffer and rescales the console.",
    "openproninx"
  },
  {
    "psh",
    "sh, shell",
    "FNU Utilities",
    "Proninx interactive command shell",
    "psh",
    "PSH is the default interactive user shell for FNU/OpenProninx.\n\n"
    "Features:\n"
    "  * Command history navigation with Up/Down arrows\n"
    "  * Line editing with Backspace, Ctrl-U (kill line), and Ctrl-C\n"
    "  * Built-in commands: cd, clear, help, history, status, services, health,\n"
    "    start/stop/restart health, fnufetch, fnudf, fnudate, fbset, reboot, poweroff\n"
    "  * Foreground process group management with setforeground()",
    "fnu, fnusvc, man"
  },
  {
    "man",
    "fnuman, help",
    "FNU Utilities",
    "Display reference manual pages for commands and topics",
    "man [topic | command]",
    "man (fnuman) formats and displays documentation pages for FNU commands\n"
    "and OpenProninx system concepts.\n\n"
    "Run 'man' without arguments to see the full table of available topics.",
    "psh, fnu, openproninx"
  },
  {
    "fnusvc",
    "supervisor",
    "FNU Utilities",
    "FNU service manager and supervisor",
    "fnusvc",
    "fnusvc is the system supervisor started by PID 1. It monitors registered\n"
    "services (such as fnu-health and interactive shell sessions), publishes status\n"
    "to '/fnusvc.status', and accepts control commands via '/fnusvc.command'.",
    "health, services, psh"
  },
  {
    "health",
    "fnu-health",
    "FNU Utilities",
    "Inspect or run system health observations",
    "health\nfnu-health [-s|-1]",
    "The 'health' shell command displays the latest system observation recorded by\n"
    "the fnu-health daemon in '/.fnuhealth' (uptime, process count, free memory).\n"
    "The 'fnu-health' daemon runs in the background under fnusvc supervision.",
    "fnusvc, services, status"
  },

  /* -------------------------------------------------------------
   * Network Commands
   * ---------------------------------------------------------- */
  {
    "netinfo",
    "ifconfig",
    "Networking",
    "Display network interface state and traffic counters",
    "netinfo",
    "netinfo queries SYS_netinfo to display the primary interface name, MAC address,\n"
    "assigned IPv4 address, default gateway, DNS server, DHCP lease status, and\n"
    "packet transmission/reception/drop statistics.",
    "netconfig, ping, resolve"
  },
  {
    "netconfig",
    "ip",
    "Networking",
    "Configure IPv4 network parameters or restart DHCP",
    "netconfig [dhcp | IP NETMASK GATEWAY DNS]",
    "netconfig applies IPv4 network configurations via SYS_netconfig.\n"
    "Run 'netconfig dhcp' to request an address lease via DHCP,\n"
    "or supply static IP parameters to configure manual addressing.",
    "netinfo, ping"
  },
  {
    "ping",
    "icmp",
    "Networking",
    "Send ICMP Echo requests to verify IPv4 connectivity",
    "ping IP_ADDRESS",
    "ping transmits ICMP Echo Request datagrams to the specified IPv4 target\n"
    "and reports the round-trip latency in milliseconds.",
    "netinfo, resolve"
  },
  {
    "resolve",
    "dns, nslookup",
    "Networking",
    "Resolve domain hostnames to IPv4 addresses via DNS",
    "resolve HOSTNAME",
    "resolve performs a DNS lookup for the given hostname using the configured\n"
    "DNS server and prints the resolved IPv4 address.",
    "netinfo, ping"
  },

  /* -------------------------------------------------------------
   * System & Account Administration
   * ---------------------------------------------------------- */
  {
    "adduser",
    "useradd",
    "Administration",
    "Create a new user account",
    "adduser [-w] USERNAME",
    "adduser prompts for a password and creates a local user account.\n"
    "The optional '-w' flag grants membership in the administrative 'wheel' group.",
    "passwd, doas, whoami"
  },
  {
    "passwd",
    "password",
    "Administration",
    "Update an account password",
    "passwd [USERNAME]",
    "passwd modifies the SHA-256 password hash for the specified user or current account.",
    "adduser, doas"
  },
  {
    "doas",
    "sudo",
    "Administration",
    "Execute a command with root privileges",
    "doas COMMAND [ARG...]",
    "doas authenticates a wheel-group user and temporarily grants root (UID 0)\n"
    "privileges to execute the target command.",
    "adduser, passwd, whoami"
  },
  {
    "top",
    "ps",
    "Administration",
    "Display process table and memory consumption",
    "top",
    "top lists all active processes, their PIDs, names, execution states, and\n"
    "allocated memory sizes using SYS_procinfo.",
    "status, fnufetch"
  },
  {
    "reboot",
    "restart",
    "Administration",
    "Reboot the system",
    "reboot",
    "reboot pulses the CPU reset line via the keyboard controller (requires root/wheel).",
    "poweroff"
  },
  {
    "poweroff",
    "shutdown, halt",
    "Administration",
    "Shut down the machine",
    "poweroff",
    "poweroff performs an ACPI system shutdown (requires root/wheel).",
    "reboot"
  }
};

#define NUM_MAN_ENTRIES ((int)(sizeof(man_db) / sizeof(man_db[0])))

static int str_equal_ci(const char *a, const char *b) {
  while (*a && *b) {
    char ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
    char cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
    if (ca != cb) return 0;
    a++; b++;
  }
  return *a == 0 && *b == 0;
}

static int contains_word(const char *csv, const char *word) {
  char buf[32];
  while (*csv) {
    while (*csv == ' ' || *csv == ',') csv++;
    if (!*csv) break;
    int i = 0;
    while (*csv && *csv != ',' && *csv != ' ' && i < 31)
      buf[i++] = *csv++;
    buf[i] = 0;
    if (str_equal_ci(buf, word))
      return 1;
  }
  return 0;
}

static const struct man_entry *find_entry(const char *name) {
  for (int i = 0; i < NUM_MAN_ENTRIES; i++) {
    if (str_equal_ci(man_db[i].name, name))
      return &man_db[i];
    if (man_db[i].aliases && contains_word(man_db[i].aliases, name))
      return &man_db[i];
  }
  return 0;
}

static void print_index(void) {
  printf("FNU Manual Pages -- %s %s\n", FNU_PRODUCT_NAME, FNU_PRODUCT_VERSION);
  printf("Usage: man <topic | command>\n\n");

  const char *current_sec = 0;
  for (int i = 0; i < NUM_MAN_ENTRIES; i++) {
    if (!current_sec || strcmp(current_sec, man_db[i].section) != 0) {
      current_sec = man_db[i].section;
      printf("\033[33m[%s]\033[0m\n", current_sec);
    }
    printf("  %-12s - %s\n", man_db[i].name, man_db[i].summary);
  }
  printf("\nTip: type 'man <name>' (e.g. 'man fnudf' or 'man fnu') for full documentation.\n");
}

static void display_entry(const struct man_entry *e) {
  printf("\033[33m%s(1)\033[0m               FNU Reference Manual               \033[33m%s(1)\033[0m\n\n",
         e->name, e->name);

  printf("\033[32mNAME\033[0m\n");
  printf("    %s -- %s\n\n", e->name, e->summary);

  if (e->synopsis && e->synopsis[0]) {
    printf("\033[32mSYNOPSIS\033[0m\n");
    printf("    %s\n\n", e->synopsis);
  }

  printf("\033[32mDESCRIPTION\033[0m\n");
  const char *p = e->description;
  printf("    ");
  while (*p) {
    putchar(*p);
    if (*p == '\n')
      printf("    ");
    p++;
  }
  printf("\n\n");

  if (e->see_also && e->see_also[0]) {
    printf("\033[32mSEE ALSO\033[0m\n");
    printf("    %s\n\n", e->see_also);
  }

  printf("\033[33m%s %s\033[0m\n", FNU_PRODUCT_NAME, FNU_PRODUCT_VERSION);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_index();
    exit();
  }

  const struct man_entry *e = find_entry(argv[1]);
  if (!e) {
    dprintf(2, "man: no manual entry for '%s'\n", argv[1]);
    dprintf(2, "Type 'man' with no arguments to see available topics.\n");
    exit();
  }

  display_entry(e);
  exit();
}
