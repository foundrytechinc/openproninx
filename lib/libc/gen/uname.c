// uname(3): the kernel's names, each by its sysctl node
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

static int field(const char *node, char *out, size_t room) {
  size_t len = room;

  return sysctlbyname(node, out, &len, 0, 0);
}

int uname(struct utsname *name) {
  int rval = 0;

  if (field("kern.ostype", name->sysname, sizeof(name->sysname)) < 0)
    rval = -1;
  if (field("kern.hostname", name->nodename, sizeof(name->nodename)) < 0)
    rval = -1;
  if (field("kern.osrelease", name->release, sizeof(name->release)) < 0)
    rval = -1;
  if (field("kern.version", name->version, sizeof(name->version)) < 0)
    rval = -1;
  if (field("hw.machine", name->machine, sizeof(name->machine)) < 0)
    rval = -1;
  return rval;
}
