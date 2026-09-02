#include "user.h"

// This service deliberately writes no normal output to the interactive PSH
// terminal. Its latest observation is available to administrative commands.
int main(void) {
  struct info system;
  int fd;

  for (;;) {
    if (info(&system) == 0) {
      // The temporary legacy root filesystem has a 14-byte filename limit.
      // Keep service state within it until UFS2 becomes the data VFS.
      fd = open("/.fnuhealth", O_WRONLY);
      if (fd >= 0) {
        dprintf(fd, "uptime=%d ticks\nprocesses=%d\nfree_memory=%d KB\n",
                system.uptime, system.nprocs, system.free_ram / 1024);
        close(fd);
      }
    }
    sleep(60);
  }
}
