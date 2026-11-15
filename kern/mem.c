// /dev/null and /dev/zero
#include "defs.h"
#include "devfs.h"
#include "file.h"
#include "inc/stat.h"
#include "mmu.h"
#include "uio.h"

static int nullread(struct inode *ip, struct uio *uio) {
  (void)ip;
  (void)uio;
  return 0;
}

static int nullwrite(struct inode *ip, struct uio *uio) {
  int n = (int)uio->uio_resid;

  (void)ip;
  uio->uio_resid = 0;
  return n;
}

static int zeroread(struct inode *ip, struct uio *uio) {
  size_t want = uio->uio_resid;
  char *zeros;

  (void)ip;
  if ((zeros = page_alloc()) == 0)
    return -FNU_ERR_NOMEMORY;
  memset(zeros, 0, PGSIZE);
  while (uio->uio_resid > 0) {
    size_t chunk = uio->uio_resid > PGSIZE ? PGSIZE : uio->uio_resid;

    if (uiomove(zeros, chunk, uio) != 0) {
      page_free(zeros);
      return -FNU_ERR_FAULT;
    }
  }
  page_free(zeros);
  return (int)want;
}

void memdevinit(void) {
  devsw[NULLDEV].class = FNU_DEVCLASS_MEMORY;
  devsw[ZERODEV].class = FNU_DEVCLASS_MEMORY;
  devsw[NULLDEV].read = nullread;
  devsw[NULLDEV].write = nullwrite;
  devsw[ZERODEV].read = zeroread;
  devsw[ZERODEV].write = nullwrite;
}

void memdevattach(void) {
  devfs_add("null", FNU_TYPE_DEVICE, NULLDEV, 0, 0666);
  devfs_add("zero", FNU_TYPE_DEVICE, ZERODEV, 0, 0666);
}
