// UMASS

#include "blockdev.h"
#include "buf.h"
#include "defs.h"
#include "malloc.h"
#include "mutex.h"
#include "sleeplock.h"
#include "usbdi.h"

#define UMASS_MAX_LUN 15 // bCBWLUN has four bits
#define CBW_SIGNATURE 0x43425355u
#define CSW_SIGNATURE 0x53425355u
#define CBW_FLAG_IN 0x80
#define CSW_GOOD 0
#define CSW_FAILED 1
#define CSW_PHASE 2

#define UR_BBB_RESET 0xff
#define UR_BBB_GET_MAX_LUN 0xfe

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_INQUIRY 0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_16 0x88
#define SCSI_WRITE_16 0x8a
#define SCSI_SERVICE_ACTION_IN 0x9e
#define SCSI_SA_READ_CAPACITY_16 0x10
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2a
#define SCSI_SYNCHRONIZE_CACHE 0x35
#define SCSI_START_STOP_UNIT 0x1b
#define SSS_START 0x01
#define SSS_LOEJ 0x02
#define SKEY_NOT_READY 0x02
#define SKEY_ILLEGAL_REQUEST 0x05
#define SKEY_UNIT_ATTENTION 0x06
#define SKEY_DATA_PROTECT 0x07

#define CMD_OK 0
#define CMD_CHECK 1 // CHECK CONDITION, the sense says why
#define CMD_FAIL 2  // the transport failed, why says where

#define ERR_PPS 4 // failure lines a second

#define XFER_MS 5000
#define RW_MS 60000
#define SYNC_MS 100000
#define START_MS 50000

#define ASC_NOT_READY 0x04
#define ASCQ_BECOMING_READY 0x01
#define ASCQ_INIT_REQUIRED 0x02
#define READY_POLLS 120
#define READY_POLL_MS 500

struct umass_cbw {
  uint32_t signature;
  uint32_t tag;
  uint32_t length;
  uint8_t flags;
  uint8_t lun;
  uint8_t cdb_len;
  uint8_t cdb[16];
} __packed;

struct umass_csw {
  uint32_t signature;
  uint32_t tag;
  uint32_t residue;
  uint8_t status;
} __packed;

struct scsi_sense {
  uint8_t key, asc, ascq;
};

struct umass_softc {
  struct umass_softc *next;
  int refs;
  struct usb_device *dev;
  struct umass_softc *bus;
  int busunit;
  char name[16]; // the bus's: umassN
  int iface;
  int lun;
  struct usb_pipe in;
  struct usb_pipe out;
  struct sleeplock lock;
  int in_use;
  int unit;   // the block layer's minor
  uint32_t tag;
  uint64_t blocks;
  uint32_t blocksize;
  int present;
  char model[32];
  int parked;
  uint16_t vendor, product;
  struct usb_bus *hc;     // where it hangs, for a match after the sleep
  uint8_t rhport, depth;
  uint32_t route;
  char serial[USB_MAX_STRING_LEN];
  uint32_t quirks; // usbd_quirks
  int dirty;
  int nosync;
  char why[40]; // what the last failure was
  int fails;    // transport failures in a row
  uint8_t last_op;  // the last command a status came back for
  uint64_t last_at; // and when, in ticks
  uint64_t errlast; // the failure lines, a few a second
  int errpps, errhidden;
};

static struct {
  struct mutex lock;
  struct umass_softc *list;
  struct unrhdr units; // the disks
  struct unrhdr buses; // the interfaces
} um;

static int parked_chan;
static int umass_major;

static uint64_t umass_unit_size(uint unit);
static int umass_ident(uint unit, struct fnu_diskinfo *out);
static int umass_flush(uint unit);
static int umass_powerdown(uint unit);
static void umass_unit_release(uint unit);
void umassrw(struct buf *b);

static int umass_removable(uint unit __unused) { return 1; }

static uint umass_maxphys(uint unit) {
  (void)unit;
  return USB_XFER_MAX;
}

static struct blockdrv umass_blockdrv = {
    .name = "umass",
    .prefix = "sd",
    .units = 1,
    .strategy = umassrw,
    .maxphys = umass_maxphys,
    .size = umass_unit_size,
    .removable = umass_removable,
    .flush = umass_flush,
    .powerdown = umass_powerdown,
    .release = umass_unit_release,
    .ident = umass_ident,
};

static struct umass_softc *umass_new(struct umass_softc *bus) {
  struct umass_softc *sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);

  sc->refs = 1;
  sc->in_use = 1;
  sc->unit = alloc_unr(&um.units);
  if (bus == 0) {
    sc->bus = sc;
    sc->busunit = alloc_unr(&um.buses);
    snprintf(sc->name, sizeof(sc->name), "umass%d", sc->busunit);
    initsleeplock(&sc->lock, "umass");
  } else {
    sc->bus = bus;
    strlcpy(sc->name, bus->name, sizeof(sc->name));
    sc->dev = bus->dev;
    sc->vendor = bus->vendor;
    sc->product = bus->product;
    sc->quirks = bus->quirks;
    sc->nosync = bus->nosync;
    sc->iface = bus->iface;
  }
  mtx_enter(&um.lock);
  if (bus != 0)
    bus->refs++;
  sc->next = um.list;
  um.list = sc;
  if ((uint)sc->unit + 1 > umass_blockdrv.units)
    umass_blockdrv.units = (uint)sc->unit + 1;
  mtx_leave(&um.lock);
  return sc;
}

static struct umass_softc *umass_get(uint unit) {
  struct umass_softc *sc;

  mtx_enter(&um.lock);
  for (sc = um.list; sc != 0; sc = sc->next)
    if (sc->in_use && sc->unit == (int)unit) {
      sc->refs++;
      break;
    }
  mtx_leave(&um.lock);
  return sc;
}

static void umass_rele(struct umass_softc *sc) {
  struct umass_softc *bus;

  mtx_enter(&um.lock);
  if (--sc->refs > 0) {
    mtx_leave(&um.lock);
    return;
  }
  mtx_leave(&um.lock);
  bus = sc->bus != sc ? sc->bus : 0;
  if (bus == 0)
    free_unr(&um.buses, sc->busunit);
  free(sc, M_DEVBUF);
  if (bus != 0)
    umass_rele(bus);
}

static void umass_remove(struct umass_softc *sc) {
  struct umass_softc **pp;
  int held = 0;

  // a gone disk something still has open keeps its number
  if (umass_major != 0) {
    held = blockdev_busy(makedev(umass_major, sc->unit));
    blockdev_withdraw(makedev(umass_major, sc->unit));
  }
  mtx_enter(&um.lock);
  for (pp = &um.list; *pp != 0; pp = &(*pp)->next)
    if (*pp == sc) {
      *pp = sc->next;
      break;
    }
  sc->in_use = 0;
  mtx_leave(&um.lock);
  if (held)
    printf("%s: gone with a filesystem on it; the next disk gets another number\n",
           sc->name);
  else
    free_unr(&um.units, sc->unit);
  wakeup(&parked_chan);
  umass_rele(sc);
}

static void umass_unit_release(uint unit) {
  struct umass_softc *sc;

  mtx_enter(&um.lock);
  for (sc = um.list; sc != 0 && sc->unit != (int)unit; sc = sc->next)
    ;
  mtx_leave(&um.lock);
  if (sc == 0)
    free_unr(&um.units, (int)unit);
}

static int bus_units(struct umass_softc *bus, struct umass_softc ***out) {
  struct umass_softc *sc, **v;
  int n = 0, i = 0;

  mtx_enter(&um.lock);
  for (sc = um.list; sc != 0; sc = sc->next)
    if (sc->bus == bus)
      n++;
  mtx_leave(&um.lock);
  v = malloc(((size_t)n + 1) * sizeof(*v), M_TEMP, M_WAITOK);
  mtx_enter(&um.lock);
  for (sc = um.list; sc != 0 && i < n; sc = sc->next)
    if (sc->bus == bus && sc != bus) {
      sc->refs++;
      v[i++] = sc;
    }
  if (bus->in_use && i < n) {
    bus->refs++;
    v[i++] = bus;
  }
  mtx_leave(&um.lock);
  *out = v;
  return i;
}

static void units_rele(struct umass_softc **v, int n) {
  int i;

  for (i = 0; i < n; i++)
    umass_rele(v[i]);
  free(v, M_TEMP);
}

// BULK ONLY TRANSPORT

static const char *usb_why(int status) {
  switch (status) {
  case USBD_STALLED:
    return "stall";
  case USBD_TIMEOUT:
    return "timeout";
  case USBD_IOERROR:
    return "i/o error";
  case USBD_CANCELLED:
    return "cancelled";
  default:
    return "error";
  }
}

static void failed(struct umass_softc *t, const char *phase, const char *why) {
  snprintf(t->why, sizeof(t->why), "%s %s", phase, why);
}

// reset recovery, 5.3.4
// if that fails, reset the port and bring the stick back
static void bbb_reset(struct umass_softc *sc) {
  struct usb_pipe *pipes[2] = {&sc->in, &sc->out};
  int bad = 0;

  if (usbd_do_request(sc->dev, UT_WRITE_CLASS_INTERFACE, UR_BBB_RESET, 0,
                      (uint16_t)sc->iface, 0, 0) < 0)
    bad = 1;
  if (usbd_clear_endpoint_stall(&sc->in) < 0)
    bad = 1;
  if (usbd_clear_endpoint_stall(&sc->out) < 0)
    bad = 1;
  if ((!bad && ++sc->fails < 2) || sc->dev == 0 || sc->dev->gone)
    return;
  printf("%s: %s, resetting the device; the last command it finished was "
         "0x%02x, %lu ms before\n",
         sc->name, sc->why, sc->last_op,
         (unsigned long)((ticks - sc->last_at) * 1000 / HZ));
  sc->fails = 0;
  usbd_reset_device(sc->dev, pipes, 2);
}

static int umass_command(struct umass_softc *sc, const uint8_t *cdb, int cdblen,
                         void *data, int len, int in, int data_ms) {
  struct umass_softc *t = sc->bus;
  struct usb_pipe *dp = in ? &t->in : &t->out;
  struct umass_cbw cbw;
  struct umass_csw csw;

  t->why[0] = 0;
  if (cdblen <= 0 || cdblen > 16 || len < 0 || len > USB_XFER_MAX) {
    failed(t, "command", "too long");
    return CMD_FAIL;
  }

  memset(&cbw, 0, sizeof(cbw));
  cbw.signature = CBW_SIGNATURE;
  cbw.tag = ++t->tag;
  cbw.length = (uint32_t)len;
  cbw.flags = in ? CBW_FLAG_IN : 0;
  cbw.lun = (uint8_t)sc->lun;
  cbw.cdb_len = (uint8_t)cdblen;
  memmove(cbw.cdb, cdb, cdblen);

  if (usbd_bulk_transfer(&t->out, &cbw, sizeof(cbw), XFER_MS) < 0) {
    failed(t, "cbw", usb_why(t->out.sync_status));
    bbb_reset(t);
    return CMD_FAIL;
  }

  // a stalled data phase still has its status to read
  if (len > 0 && usbd_bulk_transfer(dp, data, len, data_ms) < 0) {
    if (dp->sync_status != USBD_STALLED) {
      failed(t, "data", usb_why(dp->sync_status));
      bbb_reset(t);
      return CMD_FAIL;
    }
    usbd_clear_endpoint_stall(dp);
  }

  memset(&csw, 0, sizeof(csw));
  if (usbd_bulk_transfer(&t->in, &csw, sizeof(csw), len > 0 ? XFER_MS : data_ms) < 0) {
    usbd_clear_endpoint_stall(&t->in);
    if (usbd_bulk_transfer(&t->in, &csw, sizeof(csw), XFER_MS) < 0) {
      failed(t, "csw", usb_why(t->in.sync_status));
      bbb_reset(t);
      return CMD_FAIL;
    }
  }

  if (csw.signature != CSW_SIGNATURE || csw.tag != cbw.tag) {
    failed(t, "csw", "not a status wrapper");
    bbb_reset(t);
    return CMD_FAIL;
  }
  t->last_op = cdb[0];
  t->last_at = ticks;
  if (csw.status == CSW_PHASE || csw.residue > (uint32_t)len) {
    failed(t, "csw", "phase error");
    bbb_reset(t);
    return CMD_FAIL;
  }
  t->fails = 0;
  if (csw.status != CSW_GOOD) {
    failed(t, "status", "check condition");
    return CMD_CHECK;
  }
  return CMD_OK;
}

// SCSI

// a command and, after CHECK CONDITION, its sense
static int scsi_cmd_once(struct umass_softc *sc, const uint8_t *cdb, int cdblen,
                         void *data, int len, int in, int ms,
                         struct scsi_sense *sn) {
  uint8_t rs[6], buf[18];
  int rc = umass_command(sc, cdb, cdblen, data, len, in, ms);

  if (sn != 0)
    memset(sn, 0, sizeof(*sn));
  if (rc != CMD_CHECK)
    return rc;
  memset(rs, 0, sizeof(rs));
  rs[0] = SCSI_REQUEST_SENSE;
  rs[4] = sizeof(buf);
  memset(buf, 0, sizeof(buf));
  if (umass_command(sc, rs, sizeof(rs), buf, sizeof(buf), 1, XFER_MS) != CMD_OK)
    return CMD_FAIL;
  if (sn != 0) {
    sn->key = (uint8_t)(buf[2] & 0x0f);
    sn->asc = buf[12];
    sn->ascq = buf[13];
  }
  snprintf(sc->bus->why, sizeof(sc->bus->why), "sense %x/%02x/%02x",
           buf[2] & 0x0f, buf[12], buf[13]);
  return CMD_CHECK;
}

// start a stopped unit, wait for a coming-up one, go again
static int scsi_cmd(struct umass_softc *sc, const uint8_t *cdb, int cdblen,
                    void *data, int len, int in, int ms,
                    struct scsi_sense *sn) {
  struct scsi_sense s;
  uint8_t start[6];
  int rc, started = 0, polls = 0;

  for (;;) {
    rc = scsi_cmd_once(sc, cdb, cdblen, data, len, in, ms, &s);
    if (rc != CMD_CHECK || cdb[0] == SCSI_START_STOP_UNIT ||
        s.key != SKEY_NOT_READY || s.asc != ASC_NOT_READY)
      break;
    if (s.ascq == ASCQ_INIT_REQUIRED && !started &&
        !(sc->quirks & UQ_NO_START_STOP)) {
      started = 1;
      memset(start, 0, sizeof(start));
      start[0] = SCSI_START_STOP_UNIT;
      start[4] = SSS_START | SSS_LOEJ;
      printf("%s: lun %d: the unit was stopped, starting it\n", sc->name,
             sc->lun);
      if (scsi_cmd_once(sc, start, sizeof(start), 0, 0, 0, START_MS, 0) ==
          CMD_FAIL)
        break;
      continue;
    }
    if (s.ascq != ASCQ_BECOMING_READY || polls++ >= READY_POLLS)
      break;
    usb_pause_ms(READY_POLL_MS);
  }
  if (sn != 0)
    *sn = s;
  return rc;
}

// START UNIT for a device that hangs on TUR
static int scsi_test_unit_ready(struct umass_softc *sc, struct scsi_sense *sn) {
  uint8_t cdb[6];

  memset(cdb, 0, sizeof(cdb));
  cdb[0] = SCSI_TEST_UNIT_READY;
  if (sc->quirks & UQ_NO_TEST_UNIT_READY) {
    cdb[0] = SCSI_START_STOP_UNIT;
    cdb[4] = SSS_START;
  }
  return scsi_cmd(sc, cdb, sizeof(cdb), 0, 0, 1, XFER_MS, sn);
}

static int strvis(char *out, const uint8_t *in, int len) {
  int i, n = 0, space = 0;

  for (i = 0; i < len && in[i] != 0; i++) {
    if (in[i] == ' ') {
      space = n > 0;
      continue;
    }
    if (space)
      out[n++] = ' ';
    space = 0;
    out[n++] = in[i] < 0x20 || in[i] > 0x7e ? '?' : (char)in[i];
  }
  out[n] = 0;
  return n;
}

static int scsi_inquiry(struct umass_softc *sc) {
  uint8_t cdb[6], data[36];
  int n;

  memset(cdb, 0, sizeof(cdb));
  cdb[0] = SCSI_INQUIRY;
  cdb[4] = sizeof(data);
  memset(data, 0, sizeof(data));
  if ((n = scsi_cmd(sc, cdb, sizeof(cdb), data, sizeof(data), 1, XFER_MS, 0)) !=
      CMD_OK)
    return n;

  n = strvis(sc->model, data + 8, 8);
  if (n > 0)
    sc->model[n++] = ' ';
  if (strvis(sc->model + n, data + 16, 16) == 0 && n > 0)
    sc->model[n - 1] = 0;
  return CMD_OK;
}

static uint32_t get_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// READ CAPACITY(10)
static int scsi_read_capacity(struct umass_softc *sc) {
  uint8_t cdb[16], data[32];
  uint32_t last, size;
  int rc;

  memset(cdb, 0, sizeof(cdb));
  cdb[0] = SCSI_READ_CAPACITY_10;
  memset(data, 0, sizeof(data));
  if ((rc = scsi_cmd(sc, cdb, 10, data, 8, 1, XFER_MS, 0)) != CMD_OK)
    return rc;

  last = get_be32(data);
  size = get_be32(data + 4);
  if (last == 0xffffffff) {
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_SERVICE_ACTION_IN;
    cdb[1] = SCSI_SA_READ_CAPACITY_16;
    cdb[13] = 32;
    memset(data, 0, sizeof(data));
    if ((rc = scsi_cmd(sc, cdb, 16, data, 32, 1, XFER_MS, 0)) != CMD_OK)
      return rc;
    sc->blocksize = get_be32(data + 8);
    sc->blocks = (((uint64_t)get_be32(data) << 32) | get_be32(data + 4)) + 1;
    return sc->blocksize == 0 ? CMD_FAIL : CMD_OK;
  }
  if (size == 0 || last == 0)
    return CMD_FAIL;
  sc->blocksize = size;
  sc->blocks = (uint64_t)last + 1;
  return CMD_OK;
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

// READ(10)
static int scsi_rw(struct umass_softc *sc, uint64_t lba, void *data,
                   uint blocks, int write, struct scsi_sense *sn) {
  uint8_t cdb[16];

  if (blocks == 0 || blocks > 0xffff)
    return CMD_FAIL;
  memset(cdb, 0, sizeof(cdb));
  if (lba + blocks > 0xffffffffull) {
    cdb[0] = write ? SCSI_WRITE_16 : SCSI_READ_16;
    put_be32(&cdb[2], (uint32_t)(lba >> 32));
    put_be32(&cdb[6], (uint32_t)lba);
    put_be32(&cdb[10], blocks);
    return scsi_cmd(sc, cdb, 16, data, (int)(sc->blocksize * blocks),
                    write ? 0 : 1, RW_MS + XFER_MS, sn);
  }
  cdb[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
  put_be32(&cdb[2], (uint32_t)lba);
  cdb[7] = (uint8_t)(blocks >> 8);
  cdb[8] = (uint8_t)blocks;
  return scsi_cmd(sc, cdb, 10, data, (int)(sc->blocksize * blocks),
                  write ? 0 : 1, RW_MS + XFER_MS, sn);
}

// THE BLOCK LAYER

static uint64_t umass_unit_size(uint unit) {
  struct umass_softc *sc = umass_get(unit);
  uint64_t n;

  if (sc == 0)
    return 0;
  n = sc->blocksize == FNU_BLOCKDEV_SECTOR_SIZE ? sc->blocks : 0;
  umass_rele(sc);
  return n;
}

static int umass_ident(uint unit, struct fnu_diskinfo *out) {
  struct umass_softc *sc = umass_get(unit);
  struct usb_device *dev;

  if (sc == 0)
    return -1;
  if ((dev = sc->dev) == 0) {
    umass_rele(sc);
    return -1;
  }
  strlcpy(out->model, sc->model[0] ? sc->model : "USB disk", FNU_DISK_MODEL_MAX);
  if (dev->serial[0] != 0 && sc->lun == 0)
    strlcpy(out->serial, dev->serial, FNU_DISK_SERIAL_MAX);
  else if (dev->serial[0] != 0)
    snprintf(out->serial, FNU_DISK_SERIAL_MAX, "%s-%d", dev->serial, sc->lun);
  out->kind = FNU_DISK_USB;
  umass_rele(sc);
  return 0;
}

void umassrw(struct buf *b) {
  struct umass_softc *sc;
  struct scsi_sense sn;
  char why[sizeof(sc->why)];
  int rc, tries;

  if (b == 0)
    return;
  sc = umass_get(minor(b->dev));
  if (sc == 0 || sc->blocksize != FNU_BLOCKDEV_SECTOR_SIZE) {
    if (sc != 0)
      umass_rele(sc);
    b->flags |= B_ERROR;
    b->flags &= ~B_DIRTY;
    return;
  }

  if (panicstr == NULL)
    acquiresleep(&sc->bus->lock);
  while (sc->parked && sc->in_use && panicstr == NULL) {
    releasesleep(&sc->bus->lock);
    tsleep(&parked_chan, PRIBIO, "umpark", 0);
    acquiresleep(&sc->bus->lock);
  }
  if (!sc->in_use) {
    if (panicstr == NULL)
      releasesleep(&sc->bus->lock);
    umass_rele(sc);
    b->flags |= B_ERROR;
    b->flags &= ~B_DIRTY;
    return;
  }
  // five tries; a refusal is not retried
  for (rc = CMD_FAIL, tries = 0; rc != CMD_OK && tries < 5; tries++) {
    if (sc->bus->dev == 0 || sc->bus->dev->gone)
      break;
    rc = scsi_rw(sc, b->blockno, bufdata(b), bufcount(b) / sc->blocksize,
                 (b->flags & B_DIRTY) != 0, &sn);
    if (rc == CMD_CHECK &&
        (sn.key == SKEY_ILLEGAL_REQUEST || sn.key == SKEY_DATA_PROTECT))
      break;
  }
  if (rc == CMD_OK) {
    if (b->flags & B_DIRTY) {
      b->flags &= ~B_DIRTY;
      sc->dirty = 1;
    } else
      b->flags |= B_VALID;
  }
  strlcpy(why, sc->bus->why, sizeof(why));
  if (panicstr == NULL)
    releasesleep(&sc->bus->lock);

  if (rc != CMD_OK && ppsratecheck(&sc->errlast, &sc->errpps, ERR_PPS)) {
    if (sc->errhidden != 0)
      printf("%s: lun %d: %d more failures not shown\n", sc->name, sc->lun,
             sc->errhidden);
    sc->errhidden = 0;
    printf("%s: lun %d: %s of block %lu, %u blocks, failed: %s\n", sc->name,
           sc->lun, (b->flags & B_DIRTY) ? "write" : "read",
           (unsigned long)b->blockno, bufcount(b) / sc->blocksize, why);
  } else if (rc != CMD_OK) {
    sc->errhidden++;
  }
  if (rc != CMD_OK) {
    b->flags |= B_ERROR;
    b->flags &= ~B_DIRTY;
  }
  umass_rele(sc);
}

// SYNCHRONIZE CACHE
// lock held
static int umass_sync(struct umass_softc *sc) {
  struct scsi_sense sn;
  uint8_t cdb[10];
  int rc;

  if (!sc->in_use || sc->parked || !sc->dirty || sc->nosync)
    return 0;
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = SCSI_SYNCHRONIZE_CACHE;
  sc->dirty = 0;
  if ((rc = scsi_cmd(sc, cdb, sizeof(cdb), 0, 0, 0, SYNC_MS, &sn)) == CMD_OK)
    return 0;
  if (rc == CMD_CHECK && sn.key == SKEY_ILLEGAL_REQUEST) {
    sc->nosync = 1;
    printf("%s: '%s' takes no SYNCHRONIZE CACHE; its writes are its own to keep\n",
           sc->name, sc->model);
    return 0;
  }
  printf("%s: '%s' would not flush its cache: %s\n", sc->name, sc->model,
         sc->bus->why);
  sc->dirty = 1;
  return -1;
}

static int umass_flush(uint unit) {
  struct umass_softc *sc = umass_get(unit);
  int rc;

  if (sc == 0)
    return -1;
  acquiresleep(&sc->bus->lock);
  rc = umass_sync(sc);
  releasesleep(&sc->bus->lock);
  umass_rele(sc);
  return rc;
}

// START STOP UNIT; the stick flushes itself before the power goes
static int umass_powerdown(uint unit) {
  struct umass_softc *sc = umass_get(unit);
  struct scsi_sense sn;
  uint8_t cdb[6];

  if (sc == 0)
    return -1;
  acquiresleep(&sc->bus->lock);
  if (sc->in_use && !sc->parked)
    umass_sync(sc);
  if (sc->in_use && !sc->parked && !(sc->quirks & UQ_NO_START_STOP)) {
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_START_STOP_UNIT;
    scsi_cmd(sc, cdb, sizeof(cdb), 0, 0, 0, SYNC_MS, &sn);
  }
  releasesleep(&sc->bus->lock);
  umass_rele(sc);
  return 0;
}

// ATTACH

static int umass_match(struct usb_attach_arg *uaa) {
  if (uaa->iface < 0 || uaa->class_code != UCLASS_MASS)
    return USB_MATCH_NONE;
  if (uaa->proto != UPROTO_MASS_BBB)
    return USB_MATCH_NONE;
  return USB_MATCH_IFACE_CLASS + 1;
}

static int umass_attach_lun(struct umass_softc *sc) {
  int tries;

  for (tries = 0; tries < 10; tries++) {
    if (scsi_test_unit_ready(sc, 0) == CMD_OK)
      break;
    usb_pause_ms(100);
  }
  if (scsi_inquiry(sc) != CMD_OK) {
    printf("%s: logical unit %d will not identify itself\n", sc->name, sc->lun);
    return -1;
  }
  sc->present = scsi_read_capacity(sc) == CMD_OK;
  if (!sc->present) {
    printf("%s: lun %d: '%s', no media: %s\n", sc->name, sc->lun, sc->model,
           sc->bus->why);
    return 0;
  }
  if (umass_major != 0 && sc->blocksize == FNU_BLOCKDEV_SECTOR_SIZE)
    blockdev_publish(makedev(umass_major, sc->unit));
  printf("%s: lun %d: '%s', %d MB in %d byte blocks\n", sc->name, sc->lun,
          sc->model, (uint)(sc->blocks * sc->blocksize / (1024 * 1024)),
          sc->blocksize);
  if (sc->quirks & UQ_NO_SYNC_CACHE)
    printf("%s: '%s' is listed as taking no SYNCHRONIZE CACHE; its writes are "
           "its own to keep\n",
           sc->name, sc->model);
  if (sc->blocksize != FNU_BLOCKDEV_SECTOR_SIZE)
    printf("%s: '%s' is not addressable, the block layer wants %d byte "
            "sectors\n",
            sc->name, sc->model, FNU_BLOCKDEV_SECTOR_SIZE);
  return 0;
}

// same place, same ids, same serial
static struct umass_softc *parked_of(struct usb_attach_arg *uaa) {
  struct usb_device *dev = uaa->dev;
  struct umass_softc *sc;

  mtx_enter(&um.lock);
  for (sc = um.list; sc != 0; sc = sc->next)
    if (sc->in_use && sc->parked && sc->bus == sc && sc->vendor == uaa->vendor &&
        sc->product == uaa->product && sc->iface == uaa->iface &&
        sc->hc == dev->bus && sc->rhport == dev->rhport &&
        sc->depth == dev->depth && sc->route == dev->route &&
        strncmp(sc->serial, dev->serial, sizeof(sc->serial)) == 0) {
      sc->refs++;
      break;
    }
  mtx_leave(&um.lock);
  return sc;
}

static int same_unit(struct umass_softc *sc) {
  char model[sizeof(sc->model)];
  uint64_t blocks = sc->blocks;
  uint32_t bsize = sc->blocksize;
  int tries;

  strlcpy(model, sc->model, sizeof(model));
  for (tries = 0; tries < 10; tries++) {
    if (scsi_test_unit_ready(sc, 0) == CMD_OK)
      break;
    usb_pause_ms(100);
  }
  if (scsi_inquiry(sc) != CMD_OK || strcmp(model, sc->model) != 0)
    return 0;
  if (sc->present && (scsi_read_capacity(sc) != CMD_OK || sc->blocks != blocks ||
                      sc->blocksize != bsize))
    return 0;
  return 1;
}

static int iface_index(struct usb_device *dev, int iface) {
  int i;

  for (i = 0; i < USB_MAX_INTERFACES; i++) {
    const struct usb_interface_descriptor *id = usbd_find_iface(dev, i);

    if (id == 0)
      break;
    if (id->bInterfaceNumber == iface)
      return i;
  }
  return 0;
}

static void open_pipes(struct umass_softc *sc, struct usb_device *dev, int ifidx) {
  const struct usb_endpoint_descriptor *ed;
  int i;

  memset(&sc->in, 0, sizeof(sc->in));
  memset(&sc->out, 0, sizeof(sc->out));
  for (i = 0; i < 4 && (ed = usbd_find_endpoint(dev, ifidx, i)) != 0; i++) {
    if (UE_GET_XFERTYPE(ed->bmAttributes) != UE_BULK)
      continue;
    if (UE_GET_DIR(ed->bEndpointAddress)) {
      if (!sc->in.open)
        usbd_open_pipe(dev, ed, &sc->in);
    } else if (!sc->out.open)
      usbd_open_pipe(dev, ed, &sc->out);
  }
}

static int rebind(struct usb_attach_arg *uaa, struct umass_softc *bus) {
  struct usb_device *dev = uaa->dev;
  struct umass_softc **v;
  int n, i, same;

  open_pipes(bus, dev, iface_index(dev, uaa->iface));
  acquiresleep(&bus->lock);
  n = bus_units(bus, &v);
  same = bus->in.open && bus->out.open;
  for (i = 0; i < n; i++) {
    v[i]->dev = dev;
    if (same && !same_unit(v[i]))
      same = 0;
  }
  for (i = 0; i < n && same; i++)
    v[i]->parked = 0;
  releasesleep(&bus->lock);
  wakeup(&parked_chan);
  if (!same) {
    printf("%s: what came back after the sleep is not '%s'\n", bus->name,
            bus->model);
    usbd_close_pipe(&bus->in);
    usbd_close_pipe(&bus->out);
    for (i = 0; i < n; i++)
      umass_remove(v[i]);
    units_rele(v, n);
    return -1;
  }
  units_rele(v, n);
  uaa->softc = bus;
  printf("%s: '%s' back after the sleep\n", bus->name, bus->model);
  return 0;
}

static int umass_attach(struct usb_attach_arg *uaa) {
  struct usb_device *dev = uaa->dev;
  struct umass_softc *sc, *other;
  uint8_t max_lun = 0;
  int i, rebound;

  if ((sc = parked_of(uaa)) != 0) {
    rebound = rebind(uaa, sc) == 0;
    umass_rele(sc);
    if (rebound)
      return 0;
  }
  sc = umass_new(0);
  sc->vendor = uaa->vendor;
  sc->product = uaa->product;
  sc->quirks = usbd_quirks(dev);
  sc->nosync = (sc->quirks & UQ_NO_SYNC_CACHE) != 0;
  sc->dev = dev;
  sc->lun = 0;
  sc->iface = uaa->iface;
  sc->hc = dev->bus;
  sc->rhport = dev->rhport;
  sc->depth = dev->depth;
  sc->route = dev->route;
  strlcpy(sc->serial, dev->serial, sizeof(sc->serial));

  open_pipes(sc, dev, iface_index(dev, uaa->iface));
  if (!sc->in.open || !sc->out.open) {
    printf("%s: no bulk pipe pair\n", sc->name);
    usbd_close_pipe(&sc->in);
    usbd_close_pipe(&sc->out);
    umass_remove(sc);
    return -1;
  }

  if ((sc->quirks & UQ_NO_GETMAXLUN) ||
      usbd_do_request(dev, UT_READ_CLASS_INTERFACE, UR_BBB_GET_MAX_LUN, 0,
                      (uint16_t)uaa->iface, 1, &max_lun) < 0 ||
      max_lun > UMASS_MAX_LUN)
    max_lun = 0;

  if (umass_attach_lun(sc) < 0) {
    usbd_close_pipe(&sc->in);
    usbd_close_pipe(&sc->out);
    umass_remove(sc);
    return -1;
  }
  for (i = 1; i <= (int)max_lun; i++) {
    other = umass_new(sc);
    other->lun = i;
    if (umass_attach_lun(other) < 0)
      umass_remove(other);
  }
  uaa->softc = sc;
  return 0;
}

#define ASC_MEDIUM_CHANGED 0x28
#define ASC_MEDIUM_NOT_PRESENT 0x3a

static void umass_media_lun(struct umass_softc *sc) {
  struct scsi_sense sn;
  uint64_t was;
  int rc, ready, changed = 0, gone = 0;

  acquiresleep(&sc->bus->lock);
  rc = scsi_test_unit_ready(sc, &sn);
  ready = rc == CMD_OK;
  if (rc == CMD_CHECK) {
    if (sn.key == SKEY_UNIT_ATTENTION && sn.asc == ASC_MEDIUM_CHANGED)
      changed = 1;
    else if (sn.asc == ASC_MEDIUM_NOT_PRESENT)
      gone = 1;
    if (changed)
      ready = scsi_test_unit_ready(sc, 0) == CMD_OK;
  }

  if (ready) {
    was = sc->blocks;
    if (scsi_read_capacity(sc) != CMD_OK) {
      releasesleep(&sc->bus->lock);
      return;
    }
    if (!sc->present || changed || sc->blocks != was)
      changed = 1;
    sc->present = 1;
  } else if (gone && sc->present) {
    sc->present = 0;
    sc->blocks = 0;
  } else {
    releasesleep(&sc->bus->lock);
    return;
  }
  releasesleep(&sc->bus->lock);

  if (!sc->present) {
    printf("%s: lun %d: the media was taken out\n", sc->name, sc->lun);
    blockdev_withdraw(makedev(umass_major, sc->unit));
  } else if (changed) {
    printf("%s: lun %d: the media changed, looking at it again\n", sc->name,
            sc->lun);
    blockdev_publish(makedev(umass_major, sc->unit));
    partition_reprobe(makedev(umass_major, sc->unit));
  }
}

static void umass_media(struct usb_device *dev, void *softc) {
  struct umass_softc **v;
  int n, i;

  (void)dev;
  if (umass_major == 0)
    return;
  n = bus_units(softc, &v);
  for (i = 0; i < n; i++)
    umass_media_lun(v[i]);
  units_rele(v, n);
}

static void umass_woke(void) {
  struct umass_softc *sc;

  for (;;) {
    mtx_enter(&um.lock);
    for (sc = um.list; sc != 0 && !(sc->in_use && sc->parked); sc = sc->next)
      ;
    if (sc != 0)
      sc->refs++;
    mtx_leave(&um.lock);
    if (sc == 0)
      break;
    printf("%s: lun %d: '%s' did not come back after the sleep\n", sc->name,
            sc->lun, sc->model);
    sc->parked = 0;
    umass_remove(sc);
    umass_rele(sc);
  }
  wakeup(&parked_chan);
}

static void umass_detach(struct usb_device *dev, void *softc) {
  struct umass_softc *bus = softc, **v;
  int n, i;

  acquiresleep(&bus->lock);
  n = bus_units(bus, &v);
  if (usbd_sleeping()) {
    for (i = 0; i < n; i++) {
      if (!dev->gone)
        umass_sync(v[i]);
      v[i]->parked = 1;
      v[i]->dev = 0;
    }
    usbd_close_pipe(&bus->in);
    usbd_close_pipe(&bus->out);
    releasesleep(&bus->lock);
    units_rele(v, n);
    return;
  }
  usbd_close_pipe(&bus->in);
  usbd_close_pipe(&bus->out);
  for (i = 0; i < n; i++) {
    v[i]->present = 0;
    v[i]->blocks = 0;
    v[i]->dev = 0;
  }
  releasesleep(&bus->lock);
  for (i = 0; i < n; i++)
    umass_remove(v[i]);
  units_rele(v, n);
}

static const struct usb_driver umass_driver = {
    .name = "umass",
    .match = umass_match,
    .attach = umass_attach,
    .detach = umass_detach,
    .media = umass_media,
    .woke = umass_woke,
};

void umass_init(void) {
  mtx_init(&um.lock, "umass");
  init_unrhdr(&um.units, "umassdisk");
  init_unrhdr(&um.buses, "umass");
  umass_major = blockdev_register(&umass_blockdrv);
  usbd_register_driver(&umass_driver);
}
