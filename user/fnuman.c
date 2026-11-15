// FNUMAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <math.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/ptrace.h>
#include <fnu/sys.h>
#include "inc/product.h"
#include <sys/syscall.h>
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
  // system
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
    "  * fnusvc     - Service supervisor managing daemons and terminal logins\n"
    "  * sh         - Command shell (FreeBSD's ash)\n"
    "  * df, ps, w  - Disk space, processes, logins\n"
    "  * fnufetch   - System summary and hardware banner\n"
    "  * fnuman     - Manual pager for commands and architecture\n\n"
    "FNU utilities are statically linked against the OpenProninx Stable Core ABI\n"
    "and embed directly into the root ramdisk image.",
    "openproninx, abi, sh, fnusvc"
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
    "fnu, abi, ufs2, vidcontrol"
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
    "  * Stable Core ABI: processes, files, BSD sockets, tty and framebuffer\n"
    "    ioctls, rtctime, mount and df.\n"
    "  * Experimental ABI: credentials (setgid, geteuid, setgroups,\n"
    "    getgroups). login(1) and doas(1) check passwords in userland.",
    "openproninx, fnu"
  },
  {
    "ufs2",
    "data, ffs",
    "Concepts",
    "Unix File System 2 data volume support",
    "man ufs2",
    "The root filesystem is UFS2 on the fnu-root partition. The loader reads\n"
    "/etc/filesystems from it and passes the root to the kernel. Without\n"
    "fnu-root the ramdisk is the root and changes are lost at reboot.\n\n"
    "Other volumes are mounted from fstab at boot, when plugged in, or with\n"
    "mount(8).",
    "df, mount, newfs, fsck, openproninx"
  },

  // utilities
  {
    "df",
    "fnudf",
    "FNU Utilities",
    "Show free disk space",
    "df [-hikl] [-t [no]type[,type...]] [file ...]",
    "df shows size, used and free space of each mounted volume in kilobytes.\n"
    "-h uses human units, -i adds inodes, -l shows local volumes only, -t\n"
    "selects filesystem types. With a file, only its volume is shown.",
    "mount, fstab, du, ufs2"
  },
  {
    "fnufetch",
    "info",
    "FNU Utilities",
    "Pretty system information and hardware banner",
    "fnufetch",
    "fnufetch displays the OpenProninx ASCII emblem alongside OS version, kernel\n"
    "architecture, system uptime, active process count, and RAM utilization.",
    "df, status"
  },
  {
    "sh",
    "psh, shell, umask, jobs, type, command, printf, test, read, export, "
    "local, trap, eval, exec, set, shift, alias, getopts, ulimit, wait",
    "FNU Utilities",
    "The command shell",
    "sh [-eiluvx] [-c string | file [argument ...]]",
    "sh is FreeBSD's Almquist shell, a Bourne shell with job control. An\n"
    "interactive shell reads $ENV (~/.shrc), which sets the prompt.\n\n"
    "Built-ins:\n"
    "  : . [ alias bg break builtin cd chdir command continue echo eval exec\n"
    "  exit export false fg getopts hash jobid jobs kill let local printf pwd\n"
    "  read readonly return set setvar shift test times trap true type ulimit\n"
    "  umask unalias unset wait\n\n"
    "There is no history or line editing. ^H, ^U, ^W and ^R work as usual.",
    "fnu, stty, login"
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
    "sh, fnu, openproninx"
  },
  {
    "fnusvc",
    "supervisor",
    "FNU Utilities",
    "The service supervisor",
    "fnusvc",
    "fnusvc starts, watches and stops services. Enabled services are listed in\n"
    "/etc/rc.conf and described in /etc/rc.d/<name>. The boot stage runs\n"
    "first, then multi. Services that exit are restarted according to\n"
    "restart=, and too many failures put them in maintenance. Logs go to\n"
    "/var/log/svc/<name>.log. At shutdown services stop in reverse order.\n"
    "Commands come in on /var/run/fnusvc.ctl.",
    "rc.d, services, init, init.conf"
  },
  {
    "health",
    0,
    "FNU Utilities",
    "Show system state",
    "health",
    "health prints uptime, process count and free memory.",
    "fnusvc, services, status"
  },
  {
    "services",
    0,
    "FNU Utilities",
    "Control services",
    "services [-a] | status | log name | start | stop | restart | clear | run | reload | enable | disable name",
    "Without arguments services lists each service with its state, pid, uptime,\n"
    "restart count and stage. -a also lists disabled ones.\n\n"
    "  status name   state, reason and the end of the log\n"
    "  log name      the whole log\n"
    "  start, stop, restart name\n"
    "  clear name    leave maintenance\n"
    "  run name      run a step again\n"
    "  reload name   send SIGHUP\n"
    "  enable, disable name   edit /etc/rc.conf",
    "fnusvc, rc.d, health"
  },

  // network
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
    "Send ICMP echo requests to a host",
    "ping [-c count] [-i wait] host",
    "ping sends an ICMP echo request every wait seconds (default 1) and prints\n"
    "each reply. At the end, or on ^C, it prints loss and round-trip stats.",
    "netinfo, resolve"
  },
  {
    "resolve",
    "dns, nslookup, hosts",
    "Networking",
    "Look up the IPv4 address of a host",
    "resolve name ...",
    "resolve prints the IPv4 address of each name. It checks /etc/hosts first,\n"
    "then asks the DNS servers in net.inet.dns.servers.",
    "netinfo, netconfig, ping"
  },

  // administration
  {
    "useradd",
    "adduser",
    "Administration",
    "Create an account",
    "useradd [-mMo] [-c comment] [-d home] [-g group] [-G group,...] [-k skel] [-p hash] [-s shell] [-u uid] name",
    "useradd creates an account. By default it also gets its own group. -G adds\n"
    "more groups (wheel allows doas). -m creates the home directory from\n"
    "/etc/skel. -p sets the bcrypt hash, otherwise the account stays locked\n"
    "until passwd is run. -D prints the defaults from /etc/users.conf.\n"
    "adduser does the same and also asks for a password.",
    "usermod, userdel, groupadd, passwd, users.conf"
  },
  {
    "passwd",
    "password",
    "Administration",
    "Update an account password",
    "passwd [USERNAME]",
    "passwd asks for the old password (not for root), then the new one twice,\n"
    "and stores a bcrypt ($2b$) hash in /etc/master.passwd.",
    "adduser, doas, su"
  },
  {
    "doas",
    "sudo",
    "Administration",
    "Execute a command with root privileges",
    "doas [-ns] [-u user] COMMAND [ARG...]",
    "doas runs a command as root, or as the user given with -u. Rules are in\n"
    "/etc/doas.conf: permit|deny [nopass] identity [as target] [cmd command].\n"
    "The last matching rule wins. It asks for your password unless nopass.",
    "adduser, passwd, whoami"
  },
  {
    "dmesg",
    0,
    "Administration",
    "Print kernel messages",
    "dmesg [-a | -s]",
    "dmesg prints the kernel messages of the previous boot, if kept, and of\n"
    "this one. -a includes syslog messages. -s prints the boot console output\n"
    "(root only).",
    "syslogd, top, fnufetch"
  },
  {
    "top",
    0,
    "Administration",
    "Show processes and memory",
    "top [-b] [-d seconds]",
    "top shows processes with state, priority, nice, CPU and memory use, plus\n"
    "load average and free memory, refreshed every few seconds.",
    "ps, w, fnufetch"
  },
  {
    "ps",
    0,
    "Administration",
    "List processes",
    "ps [-acjluwx] [-p pid] [-t tty] [-U user]",
    "ps lists your processes that have a terminal. -a shows all users, -x\n"
    "adds processes without a terminal, -p, -t and -U filter. -u, -l and -j\n"
    "pick the long formats. -c shows the command name only, -w does not\n"
    "truncate. STAT letters are the BSD ones.",
    "top, w, kill"
  },
  {
    "w",
    "uptime",
    "Administration",
    "Who is logged in and what they are doing",
    "w [-h] [user]\n"
    "uptime",
    "w prints uptime, user count and load average, then each login with its\n"
    "terminal, login time, idle time and current command. -h skips the\n"
    "header. uptime prints only the header.",
    "who, users, last, ps"
  },
  {
    "who",
    0,
    "Administration",
    "Show who is logged in",
    "who [am i]",
    "who lists logins with terminal and time. who am i shows only yours.",
    "w, users, last"
  },
  {
    "last",
    "wtmp",
    "Administration",
    "Show past logins",
    "last [-n count] [name | tty ...]",
    "last lists logins from /var/log/wtmp, newest first, with terminal, host\n"
    "and times.",
    "who, w, login"
  },
  {
    "reboot",
    "restart",
    "Administration",
    "Reboot the system",
    "reboot",
    "reboot asks init to stop all processes, flush the disks and restart the\n"
    "machine. The kernel tries ACPI reset, the keyboard controller, port 0xcf9\n"
    "and a triple fault, in that order.",
    "poweroff, init"
  },
  {
    "poweroff",
    "shutdown, halt",
    "Administration",
    "Shut down the machine",
    "poweroff",
    "poweroff stops the system like reboot and then turns the power off\n"
    "through ACPI. Without ACPI the machine is halted.",
    "reboot, init"
  },
  {
    "zzz",
    "sleep, suspend",
    "Administration",
    "Put the machine to sleep",
    "zzz",
    "zzz suspends to RAM. hw.acpi.suspend_state picks S3 or idle sleep (auto\n"
    "by default). A key, the power button or opening the lid wakes it.\n"
    "Closing the lid does the same as zzz.",
    "hibernate, sysctl"
  },
  {
    "hibernate",
    "",
    "Administration",
    "Write memory to swap and power off",
    "hibernate",
    "hibernate writes compressed memory to swap and powers off. The next boot\n"
    "restores it. Swap must be big enough. hw.acpi.hibernate_mode is\n"
    "shutdown or platform.",
    "zzz, swapctl"
  },

  {
    "ls",
    "dir",
    "File Utilities",
    "List directory contents",
    "ls [-1ACFHLRSTacdfghiklmnopqrstux] [file ...]",
    "ls lists files and directory contents, in columns on a terminal. -l long\n"
    "format, -a and -A show dot files, -R recursive, -h human sizes, -t and -S\n"
    "sort by time or size, -r reverses, -o adds file flags.",
    "cp, mv, rm, find, touch"
  },
  {
    "cp",
    "copy",
    "File Utilities",
    "Copy files and directories",
    "cp [-Rfp] source ... target",
    "cp copies files. With several sources the target must be a directory.\n"
    "-p keeps owner and group, -R copies directories, -f removes a target that\n"
    "cannot be opened.",
    "mv, ln, rm, tar"
  },
  {
    "mv",
    "move,rename",
    "File Utilities",
    "Move or rename files",
    "mv source ... target",
    "mv renames files. With several sources the target must be a directory.\n"
    "Across filesystems it copies and removes.",
    "cp, ln, rm"
  },
  {
    "rm",
    "delete,unlink",
    "File Utilities",
    "Remove files and directories",
    "rm [-dfiRrv] file ...",
    "rm removes files. -r removes directories recursively, -d empty ones, -f\n"
    "never asks and ignores missing files, -i always asks, -v prints names.",
    "rmdir, cp, mv"
  },
  {
    "ln",
    "link,symlink",
    "File Utilities",
    "Make a link to a file",
    "ln [-fs] source ... [target]",
    "ln makes a hard link, or a symbolic link with -s. Hard links cannot cross\n"
    "filesystems or point to directories. -f replaces an existing target.",
    "ls, cp, mv"
  },
  {
    "mkdir",
    0,
    "File Utilities",
    "Make directories",
    "mkdir [-p] [-m mode] directory ...",
    "mkdir creates directories. -p also creates missing parents, -m sets the\n"
    "mode.",
    "rmdir, chmod, ls"
  },
  {
    "rmdir",
    0,
    "File Utilities",
    "Remove empty directories",
    "rmdir [-p] directory ...",
    "rmdir removes empty directories. -p also removes empty parents.",
    "mkdir, rm"
  },
  {
    "chmod",
    "mode",
    "File Utilities",
    "Change file modes",
    "chmod mode file ...",
    "chmod sets file modes, in octal or as [ugoa][+-=][rwxXst] separated by\n"
    "commas.",
    "chown, umask, ls, chflags"
  },
  {
    "chflags",
    "flags,schg,uchg",
    "File Utilities",
    "Change file flags",
    "chflags [-h] [-R [-H | -L | -P]] flags file ...",
    "chflags sets file flags on UFS2: arch, nodump, opaque, sappnd, schg,\n"
    "uappnd, uchg. Prefix with no to clear. Only root can set the s flags.\n"
    "ls -lo shows them.",
    "chmod, ls"
  },
  {
    "chown",
    "chgrp,owner",
    "File Utilities",
    "Change file owner and group",
    "chown owner[:group] file ...",
    "chown sets owner and group, by name or number. ':group' changes only the\n"
    "group. Only root can change the owner.",
    "chmod, ls, users"
  },
  {
    "cat",
    0,
    "Text Utilities",
    "Copy files to the standard output",
    "cat [-benstuv] [file ...]",
    "cat prints files, or standard input. -n numbers lines, -b only non-blank\n"
    "ones, -s squeezes blank lines, -v shows non-printing characters, -e and\n"
    "-t also mark line ends and tabs, -u is unbuffered.",
    "more, head, tail, cp"
  },
  {
    "head",
    0,
    "Text Utilities",
    "Print the first lines of a file",
    "head [-count | -n count] [file ...]",
    "head prints the first lines of each file (default 10).",
    "tail, cat, more"
  },
  {
    "tail",
    0,
    "Text Utilities",
    "Print the last lines of a file",
    "tail [-f | -r] [-b n | -c n | -n n | -number] [file ...]",
    "tail prints the last lines of each file (default 10). +n counts from the\n"
    "start. -c counts bytes, -b 512-byte blocks, -r prints in reverse, -f\n"
    "keeps reading as the file grows.",
    "head, cat, more"
  },
  {
    "grep",
    "egrep,fgrep",
    "Text Utilities",
    "Search a file for a pattern",
    "grep [-abcEFGhHilLnoqrsvwx] [-A n] [-B n] [-C[n]] [-e pattern] [-f file] [pattern] [file ...]",
    "grep prints matching lines. -E extended regex, -F fixed string, -i ignore\n"
    "case, -v invert, -w and -x match words or whole lines, -c count, -l and -L\n"
    "list files, -n line numbers, -o only the match, -q quiet, -r recursive,\n"
    "-A -B -C context. Exit 0 on match, 1 on none, 2 on error.",
    "sed, awk, find"
  },
  {
    "sed",
    "stream",
    "Text Utilities",
    "Stream editor",
    "sed [-aEnru] [-i[extension]] command [file ...]",
    "sed runs a script over each input line. Commands: { } = a b c d D g G h\n"
    "H i l n N p P q r s t w x y : #. Addresses are line numbers, $ or\n"
    "regexes, ! inverts. -n only prints on p, -E extended regex, -e and -f\n"
    "give the script, -i edits in place.",
    "grep, awk, vi"
  },
  {
    "awk",
    0,
    "Text Utilities",
    "Pattern scanning and processing language",
    "awk [-F sep] [-v var=value] program [file ...]",
    "awk runs pattern { action } pairs over each input line. Fields are $1,\n"
    "$2 and so on, NF is the field count, NR the record number.",
    "sed, grep, sort"
  },
  {
    "sort",
    0,
    "Text Utilities",
    "Sort lines of text",
    "sort [-bcCfmnrsu] [-k field1[,field2]] [-o output] [-t char] [file ...]",
    "sort sorts lines. -n numeric, -f ignore case, -b ignore leading blanks,\n"
    "-r reverse, -u unique, -k key fields, -t field separator, -c check only,\n"
    "-m merge sorted files, -o output file.",
    "uniq, wc, grep"
  },
  {
    "uniq",
    0,
    "Text Utilities",
    "Report or filter repeated lines",
    "uniq [-ci] [-d | -u] [-f fields] [-s chars] [input [output]]",
    "uniq removes repeated adjacent lines. -c counts, -d prints only repeated\n"
    "lines, -u only unique ones, -i ignores case, -f and -s skip fields or\n"
    "characters. Usually run after sort.",
    "sort, wc"
  },
  {
    "wc",
    0,
    "Text Utilities",
    "Count lines, words and bytes",
    "wc [-clmw] [file ...]",
    "wc counts lines, words and bytes. -l, -w, -c and -m select counts.",
    "sort, uniq, cat"
  },
  {
    "diff",
    0,
    "Text Utilities",
    "Compare two files line by line",
    "diff [-abdiNqrstTw] [-c | -C n | -u | -U n] file1 file2",
    "diff shows the differences between two files. -u unified, -c context,\n"
    "-q only report if they differ, -i -b -w ignore case or whitespace, -r\n"
    "compare directories, -N treat missing files as empty. Exit 0 same, 1\n"
    "different, 2 error.",
    "cmp, grep"
  },
  {
    "cksum",
    "sha224, sha256, sha384, sha512",
    "Text Utilities",
    "Checksums and SHA-2 digests of files",
    "cksum [-qr] [-a algorithms] [-c] [-s string] [file ...]\n"
    "sha256 [-qr] [-c] [-s string] [file ...]",
    "cksum prints the POSIX CRC and size of each file. -a picks algorithms:\n"
    "cksum, sha224, sha256, sha384, sha512, sha512/256. As sha256 and so on it\n"
    "uses that algorithm. -r and -q change the output format, -s hashes a\n"
    "string, -c checks a list of sums.",
    "cmp"
  },
  {
    "cmp",
    0,
    "Text Utilities",
    "Compare two files byte by byte",
    "cmp [-l | -s] file1 file2 [skip1 [skip2]]",
    "cmp prints where two files first differ. -l lists all differences, -s\n"
    "is silent. Exit 0 same, 1 different, 2 error.",
    "diff"
  },
  {
    "find",
    0,
    "File Utilities",
    "Walk a file hierarchy",
    "find [path ...] [expression]",
    "find walks the given paths and tests each file against the expression.\n"
    "Primaries: -name -iname -path -type -size -newer -mtime -mmin -atime\n"
    "-ctime -empty -perm -user -links -maxdepth -mindepth -print -print0\n"
    "-prune -depth -delete -exec. Combine with -a, -o, ! and parentheses.",
    "ls, grep, xargs"
  },
  {
    "xargs",
    0,
    "Text Utilities",
    "Build and run command lines from input",
    "xargs [-0oprt] [-E eofstr] [-I replstr] [-L number] [-n number] [utility [argument ...]]",
    "xargs runs a command with arguments read from standard input. -n and -L\n"
    "limit arguments or lines per run, -I replaces a string, -0 reads\n"
    "NUL-separated input, -t prints each command.",
    "find, echo"
  },
  {
    "tar",
    "archive",
    "File Utilities",
    "Make and unpack archives",
    "tar {crtux}[014578beFfHhLmNOoPpqsvwX] [-C directory] [file ...]",
    "tar is OpenBSD's pax. c creates, r and u append, x extracts, t lists. f\n"
    "names the archive, v is verbose, p keeps owners and modes, h follows\n"
    "symlinks, -C changes directory first. Leading / and .. are stripped\n"
    "unless P is given. z, j and Z need gzip, bzip2 and compress, which are\n"
    "missing.",
    "cp, ls, dd"
  },
  {
    "dd",
    0,
    "Storage",
    "Convert and copy a file",
    "dd [operand ...]",
    "dd (DragonFly's) copies if= to of= in blocks of bs= (or ibs= and obs=),\n"
    "count= blocks, skipping skip= input and seek= output blocks. conv= takes\n"
    "the usual conversions plus sparse, fsync and fdatasync. iflag=direct and\n"
    "oflag=direct bypass the cache, oflag=sync waits for each write.\n"
    "status=progress reports every second.",
    "cp, newfs, fsck"
  },
  {
    "mount",
    0,
    "Storage",
    "Mount a filesystem",
    "mount [-a] | [-frsvw] [-o options] [-t type] device directory | mount -u [-frsvw] [-o options] directory",
    "Without arguments mount lists mounts. -t sets the type (default: detect),\n"
    "-r read-only, -s nosuid, -o ro, rw, nosuid, suid, force. -a mounts the\n"
    "boot entries from /etc/filesystems. -u changes a mounted filesystem: -w\n"
    "makes it writable, -r read-only.",
    "umount, fsck, fstab"
  },
  {
    "umount",
    "unmount",
    "Storage",
    "Unmount a filesystem",
    "umount [-f] directory | special",
    "umount unmounts by directory or device. -f forces it while in use.",
    "mount, fstab"
  },
  {
    "fstab",
    0,
    "Storage",
    "The filesystem table",
    "/etc/filesystems",
    "One line per volume:\n"
    "\n"
    "  from=/dev/fs-by-label/DATA on=/data type=ufs2 mode=rw,nosuid at=boot\n"
    "\n"
    "type is ufs2, fat32 or auto. mode is ro, rw, suid, nosuid. at is boot,\n"
    "plug or manual. Use stable names under /dev: part-by-uuid,\n"
    "part-by-label, fs-by-uuid, fs-by-label, serno. Several devices can be\n"
    "given separated by ';'. The loader reads the line for /.",
    "mount, umount, fnuplug"
  },
  {
    "newfs",
    0,
    "Storage",
    "Make a new UFS2 filesystem",
    "newfs [-b size] [-f size] [-i bytes] [-L label] device",
    "newfs creates an empty UFS2 filesystem on the device. Existing data is\n"
    "lost.",
    "fsck, mount, dd"
  },
  {
    "fsck",
    "fsck_ffs,fsck_msdos",
    "Storage",
    "Check and repair a filesystem",
    "fsck [-dfnpvy] [-t type] [device]",
    "fsck checks and repairs a filesystem with fsck_ffs (UFS2) or fsck_msdos\n"
    "(FAT). -y repairs everything, -n only reports (the default). -p without\n"
    "a device checks the boot filesystems. Exit 8 means manual repair is\n"
    "needed.",
    "newfs, mount, df"
  },
  {
    "swapctl",
    "swapon,swapoff",
    "Storage",
    "Control the swap device",
    "swapctl -A | swapctl -a device | swapctl -d device | swapctl -l [-k] | swapctl -s [-k]",
    "swapctl -A enables the swap entries in /etc/filesystems, -a adds a\n"
    "device, -d removes it, -l and -s show usage (in KB with -k).",
    "top, status"
  },
  {
    "kill",
    0,
    "Administration",
    "Send a signal to a process",
    "kill [-signal_number | -signal_name | -s signal_name] pid ...",
    "kill sends a signal (default SIGTERM) to each process. -l lists signals.",
    "top, trace, init"
  },
  {
    "su",
    0,
    "Administration",
    "Become another user",
    "su [-l] [login]",
    "su starts a shell as another user, root by default. -l gives a login\n"
    "shell with that user's home and environment. It asks for the target's\n"
    "password. Only wheel members can become root.",
    "doas, login, passwd"
  },
  {
    "stty",
    0,
    "Administration",
    "Set terminal options",
    "stty [-a | -e | -g] [-f file] [operand ...]",
    "Without operands stty prints settings that differ from the defaults. -a\n"
    "prints all, -g in a form stty can read back. Operands are flags (-flag\n"
    "clears), control characters with a value, and sane, raw, cooked, cbreak,\n"
    "nl, ek, dec.",
    "sh, vi"
  },
  {
    "vi",
    "edit,editor,ex",
    "Text Utilities",
    "Screen editor",
    "vi [-esv] [file ...]",
    "vi is neatvi, a vi clone with UTF-8, split windows (^W) and syntax\n"
    "highlighting. :w saves, :q quits, :e opens a file, :r !cmd reads command\n"
    "output. ex or vi -e starts in line mode. Startup commands come from\n"
    "EXINIT or ~/.neatvi.",
    "sed, more, stty"
  },
  {
    "more",
    "pager",
    "Text Utilities",
    "Page through a file",
    "more [-number] [file ...]",
    "more shows a screen at a time. Space for the next page, Enter for one\n"
    "line, q to quit. -number sets the page size.",
    "cat, head, tail"
  },
  {
    "trace",
    "ptrace",
    "Administration",
    "Run a program under the debugger",
    "trace [-s count] [-S] [-f] [-b addr] program [argument ...]",
    "trace single-steps a program from its first instruction and prints the\n"
    "program counter. -S prints system calls, -f follows forks, -b sets a\n"
    "breakpoint.",
    "kill, top"
  },
  {
    "powerd",
    "",
    "Administration",
    "Control CPU speed",
    "powerd [-v] [-a mode] [-b mode] [-n mode] [-i %] [-r %] [-m freq]\n"
    "       [-M freq] [-A freq] [-B freq] [-p ival]",
    "powerd sets hw.cpu.freq from the load and the power source every ival ms\n"
    "(250). Modes are min, max, adaptive and hiadaptive, set for AC (-a),\n"
    "battery (-b) and unknown (-n). -r and -i are the busy and idle marks.\n"
    "-m and -M limit the speed, -A and -B only on AC or battery, in MHz or\n"
    "nominal (fastest without turbo). The service runs powerd -B nominal.",
    "sysctl, services"
  },
  {
    "moused",
    "mouse, consolectl, sysmouse",
    "FNU Utilities",
    "Pass mouse data to the console driver",
    "moused [-3df] [-a X[,Y]] [-A exp[,offset]] [-C threshold] [-E timeout]\n"
    "       [-m N=M] [-w N] [-z target] [-VH] [-U threshold] [-L distance]\n"
    "       [-T distance[,time[,after]]] [-p port]",
    "moused reads all /dev/wsmouse* devices, including ones plugged in later,\n"
    "and sends events to /dev/consolectl. -f stays in the foreground, -d\n"
    "debugs. -a and -A set acceleration, -C the double click time, -3 and -E\n"
    "emulate a middle button, -m remaps buttons, -z and -w set up the wheel,\n"
    "-V and -H scroll with the middle button, -T filters drift. SIGHUP\n"
    "reopens the mice.",
    "vidcontrol, mouse, services"
  },
  {
    "kbdcontrol",
    "keyboard, keymap, layout, repeat",
    "FNU Utilities",
    "Keyboard settings",
    "kbdcontrol [-dFix] [-r delay.repeat | slow|normal|fast] [-l mapfile]\n"
    "           [-f # string] [-L mapfile] [-P path]",
    "kbdcontrol works on the keyboard of the current terminal. -l loads a\n"
    "keymap from /usr/share/vt/keymaps (ru, de, fr, us.dvorak, see\n"
    "INDEX.keymaps) or a path. -d and -x print the current keymap, -L prints a\n"
    "file as a C table. -r sets the repeat rate, -f and -F set or reset the\n"
    "function keys, -i shows keyboard info.",
    "vidcontrol, stty"
  },
  {
    "vidcontrol",
    "console, pointer, resolution, display",
    "FNU Utilities",
    "Console video mode and mouse pointer",
    "vidcontrol [-i active | adapter | mode] [-m on | off] [mode]",
    "vidcontrol works on the current console terminal. -i active shows the\n"
    "current mode, -i adapter the framebuffer, -i mode the list of modes. Set\n"
    "a mode with MODE_<n> or <width>x<height>. Under UEFI the firmware's mode\n"
    "cannot be changed. -m on or off shows or hides the mouse pointer.",
    "moused, mouse"
  },
  {
    "audioctl",
    "audio,tone",
    "FNU Utilities",
    "Audio device settings",
    "audioctl [-anq] [-f device] [-w wait] [name[=value] ...]",
    "audioctl prints the audio device state as name=value lines. -n omits\n"
    "names, -w repeats every few seconds. play.rate and record.rate can be set\n"
    "while idle. tone(1) plays a test tone.",
    "fnufetch"
  },
  {
    "syslog",
    0,
    "Networking",
    "Send a message to a syslog server",
    "syslog -h IPv4-server [-cis] [-f file] [-p pri] [-t tag] [message ...]",
    "syslog sends an RFC 3164 message to UDP port 514 (default priority\n"
    "user.notice). -t sets the tag, -i adds the pid, -s also prints to stderr,\n"
    "-f sends a file line by line, -c falls back to the console.",
    "netinfo, ntp"
  },
  {
    "ntp",
    0,
    "Networking",
    "Query an NTP server",
    "ntp IPv4-server",
    "ntp prints the time from an NTP server. It does not set the clock.",
    "netinfo"
  },
  {
    "fnuplug",
    "hotplug",
    "FNU Utilities",
    "Mount plugged-in devices",
    "fnuplug",
    "fnuplug watches /dev/hotplug. New volumes are mounted where an at=plug\n"
    "line in /etc/filesystems says, or under /mnt with nosuid. Removed ones\n"
    "are unmounted.",
    "mount, fstab, fnusvc"
  },
  {
    "init",
    0,
    "Administration",
    "Process one",
    "init [-s]",
    "init is the first process. It runs the supervisor and restarts it if it\n"
    "dies. -s boots single user: a root shell on the console, with a password\n"
    "if the console is insecure. ^D continues to multi user. At shutdown\n"
    "services get shutdown_timeout seconds, then the rest gets SIGHUP,\n"
    "SIGTERM and SIGKILL.\n\n"
    "Signals: SIGHUP reload, SIGINT reboot, SIGTERM single user, SIGUSR1\n"
    "halt, SIGUSR2 power off, SIGTSTP stop starting new processes.",
    "init.conf, fnusvc, services, reboot"
  },
  {
    "init.conf",
    0,
    "Administration",
    "init settings",
    "/etc/init.conf",
    "start= is single, boot or multi. console=insecure asks for the root\n"
    "password in single user. shutdown_timeout= is the time services get to\n"
    "stop. trace=yes prints each step and service on the console.",
    "init, fnusvc, rc.d"
  },
  {
    "rc.d",
    0,
    "Administration",
    "Service file",
    "/etc/rc.d/name",
    "One name=value per line:\n\n"
    "  desc=      description\n"
    "  stage=     boot or multi (default)\n"
    "  run=       long running command\n"
    "  step=      one-shot command\n"
    "  stop=      stop command instead of SIGTERM\n"
    "  requires=, before=, provides=   ordering\n"
    "  restart=   always, failure or never\n"
    "  limit=N/S  maintenance after N failures in S seconds (5/600)\n"
    "  user=, dir=, env=   how it runs\n"
    "  log=       file or console (default /var/log/svc/name.log)\n"
    "  timeout=   seconds for a step or for stopping\n"
    "  on=, secure=   terminal, and whether root may log in on it",
    "fnusvc, services, init.conf"
  },
  {
    "userdel",
    0,
    "Administration",
    "Remove an account",
    "userdel [-r] name",
    "userdel removes an account, its group memberships and its own group if\n"
    "unused. -r also removes the home directory.",
    "useradd, usermod, groupdel"
  },
  {
    "usermod",
    0,
    "Administration",
    "Change an account",
    "usermod [-aLmoU] [-c comment] [-d home] [-g group] [-G group,...] [-l name] [-p hash] [-s shell] [-u uid] name",
    "usermod changes an account. -G sets groups (-a appends), -l renames, -d\n"
    "changes the home (-m moves it), -L locks and -U unlocks.",
    "useradd, userdel, chpass"
  },
  {
    "groupadd",
    "groupdel,groupmod",
    "Administration",
    "Add, remove or change a group",
    "groupadd [-o] [-g gid] name | groupdel name | groupmod [-o] [-g gid] [-n name] name",
    "groupadd adds a group (next free gid unless -g). groupdel removes a group\n"
    "that is not a user's primary group. groupmod changes gid or name.",
    "useradd, usermod"
  },
  {
    "chpass",
    "chsh,chfn",
    "Administration",
    "Change an account's shell or name",
    "chsh [-s shell] [user] | chfn [-f name] [user]",
    "chsh changes the login shell, chfn the full name. Users can change their\n"
    "own, to a shell listed in /etc/shells. Root can change anyone's.",
    "passwd, usermod"
  },
  {
    "syslogd",
    0,
    "Administration",
    "System logger",
    "syslogd",
    "syslogd reads /dev/klog and writes messages as /etc/syslog.conf says: to\n"
    "a file, all terminals (*) or another host (@host). Files over 1 MB are\n"
    "rotated to .0. SIGHUP reloads the config.",
    "syslog.conf, dmesg, logger"
  },
  {
    "syslog.conf",
    0,
    "Administration",
    "syslogd configuration",
    "/etc/syslog.conf",
    "One rule per line: selectors, then a destination. A selector is\n"
    "facility.priority (that priority or higher), =priority, none or *.\n"
    "Separate selectors with ;. The destination is a file, * or @host.",
    "syslogd"
  },
  {
    "nologin",
    0,
    "Administration",
    "Refuse a login",
    "nologin",
    "nologin prints /etc/nologin.txt or a short message, logs the attempt and\n"
    "exits with an error.",
    "useradd, login"
  },

  {
    "echo",
    0,
    "Text Utilities",
    "Print arguments",
    "echo [-n] [string ...]",
    "echo prints its arguments separated by spaces. -n omits the newline.\n"
    "Backslash escapes are not handled, use printf.",
    "printf, cat, sh"
  },
  {
    "basename",
    0,
    "File Utilities",
    "Strip directory from a path",
    "basename string [suffix]",
    "basename prints the last component of a path, without suffix if given.",
    "dirname, ls"
  },
  {
    "dirname",
    0,
    "File Utilities",
    "Strip the last component of a path",
    "dirname string",
    "dirname prints the path without its last component, or '.' if there is\n"
    "no slash.",
    "basename, ls"
  },
  {
    "du",
    0,
    "Storage",
    "Show disk usage",
    "du [-achkrsx] [-d depth] [file ...]",
    "du shows disk usage of files and directories. -s summary only, -a every\n"
    "file, -d depth, -k kilobytes, -h human units, -c total.",
    "df, ls, find"
  },
  {
    "env",
    0,
    "Administration",
    "Print or change the environment",
    "env [name=value ...] [utility [argument ...]]",
    "env prints the environment, or runs a command with the given variables.",
    "sh, printf"
  },
  {
    "true",
    "false",
    "Administration",
    "Exit with success or failure",
    "true\nfalse",
    "true exits 0, false exits 1. Both are shell built-ins too.",
    "sh, test"
  },
  {
    "sleep",
    0,
    "Administration",
    "Wait",
    "sleep seconds",
    "sleep waits for the given number of seconds (fractions allowed).",
    "sh, kill"
  },
  {
    "sync",
    0,
    "Storage",
    "Flush buffers to disk",
    "sync",
    "sync writes all cached data to disk and waits until it is done.",
    "mount, umount, df"
  },
  {
    "tee",
    0,
    "Text Utilities",
    "Copy input to files and output",
    "tee [-ai] [file ...]",
    "tee copies standard input to standard output and to files. -a appends,\n"
    "-i ignores SIGINT.",
    "cat, xargs"
  },
  {
    "touch",
    0,
    "File Utilities",
    "Change file times or create files",
    "touch [-acm] [-r file | -t [[cc]yy]mmddHHMM[.SS]] file ...",
    "touch sets file times to now, to another file's (-r) or to a given time\n"
    "(-t). -a and -m pick access or modification time. -c does not create\n"
    "missing files.",
    "ls, find, chmod"
  },
  {
    "users",
    0,
    "Administration",
    "List logged in users",
    "users",
    "users prints the logged in user names on one line, sorted.",
    "who, w, last, whoami"
  },
  {
    "whoami",
    0,
    "Administration",
    "Print the current user",
    "whoami",
    "whoami prints the effective user name.",
    "users, su, doas"
  },
  {
    "login",
    0,
    "Administration",
    "Log in",
    "login",
    "login asks for a user name and password, checks them against\n"
    "/etc/master.passwd and starts the user's shell. While /etc/nologin exists\n"
    "only root can log in.",
    "passwd, su, doas, fnusvc"
  },
  {
    "netecho",
    "",
    "Networking",
    "Echo over TCP or UDP",
    "netecho [-u] port [address message]",
    "With only a port, netecho runs a TCP echo server (UDP with -u). With an\n"
    "address and a message it sends the message and prints the reply.",
    "netinfo, ping, syslog"
  },
  {
    "tests",
    "run",
    "Administration",
    "System tests",
    "/usr/tests/run [test ...]\n    /usr/tests/fstorture [dir [MB]] | /usr/tests/sertest [line]",
    "Each test prints ok or FAILED for every check. run runs the given tests,\n"
    "or all that need no arguments, and lists the failures.\n"
    "\n"
    "  alarmtest    alarm, signal handlers, sigwait\n"
    "  exectest     exec, text busy, bad binaries\n"
    "  fdtest       large descriptor tables, select, poll\n"
    "  fnuonly      a program with libfnu only\n"
    "  fnuthreads   native threads\n"
    "  fputest      FPU state per process\n"
    "  inettest     TCP and UDP over loopback\n"
    "  mmaptest     file mappings\n"
    "  nxtest       no-execute pages\n"
    "  pipetest     pipes\n"
    "  ptytest      a shell on a pty\n"
    "  readytest    select, poll, ppoll\n"
    "  schedtest    CPU shares and nice\n"
    "  sshtest      SSH crypto test vectors\n"
    "  swaptest     paging to swap and back\n"
    "  synctest     delayed writes and fsync\n"
    "  threadtest   C11 threads\n"
    "  unixtest     local sockets and fd passing\n"
    "  fstorture    filesystem stress (needs a directory)\n"
    "  sertest      serial line settings (needs a port)",
    "trace, top, fsck"
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

static int is_sep(char c) {
  return c == ' ' || c == ',' || c == '\t' || c == '\n';
}

static int contains_word(const char *csv, const char *word) {
  char buf[32];
  while (*csv) {
    while (is_sep(*csv)) csv++;
    if (!*csv) break;
    int i = 0;
    while (*csv && !is_sep(*csv)) {
      if (i < (int)sizeof(buf) - 1)
        buf[i++] = *csv;
      csv++;
    }
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

  /* every entry of a section under one heading, whatever order the table
   * happens to be in */
  for (int s = 0; s < NUM_MAN_ENTRIES; s++) {
    int seen = 0, i;

    for (i = 0; i < s; i++)
      if (strcmp(man_db[i].section, man_db[s].section) == 0)
        seen = 1;
    if (seen)
      continue;
    printf("\033[33m[%s]\033[0m\n", man_db[s].section);
    for (i = 0; i < NUM_MAN_ENTRIES; i++)
      if (strcmp(man_db[i].section, man_db[s].section) == 0)
        printf("  %-12s - %s\n", man_db[i].name, man_db[i].summary);
  }
  printf("\nTip: type 'man <name>' (e.g. 'man df' or 'man fnu') for full documentation.\n");
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
    exit(0);
  }

  const struct man_entry *e = find_entry(argv[1]);
  if (!e) {
    dprintf(2, "man: no manual entry for '%s'\n", argv[1]);
    dprintf(2, "Type 'man' with no arguments to see available topics.\n");
    exit(1);
  }

  display_entry(e);
  exit(0);
}
