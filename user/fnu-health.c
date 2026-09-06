#include "user.h"

// This service periodically updates the health observation file.
// Administrative command 'health' in PSH displays its content.
int main(int argc, char **argv) {
  struct info system;
  int fd;

  // If run with -1 or -s from command line, print immediately and exit
  if (argc > 1 && (strcmp(argv[1], "-1") == 0 || strcmp(argv[1], "-s") == 0)) {
    if (info(&system) == 0) {
      printf("uptime=%d ticks\nprocesses=%d\nfree_memory=%d KB\n",
             system.uptime, system.nprocs, system.free_ram / 1024);
    }
    exit();
  }

  for (;;) {
    if (info(&system) == 0) {
      // The temporary legacy root filesystem has a 14-byte filename limit.
      // Keep service state within it until UFS2 becomes the data VFS.
      fd = open("/.fnuhealth", O_CREATE | O_WRONLY);
      if (fd >= 0) {
        dprintf(fd, "uptime=%d ticks\nprocesses=%d\nfree_memory=%d KB\n",
                system.uptime, system.nprocs, system.free_ram / 1024);
        close(fd);
      }
    }
    sleep(5);
  }
}
