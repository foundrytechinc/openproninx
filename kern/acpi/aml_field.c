// AML FIELDS

#include "acpi.h"
#include "aml.h"
#include "aml_priv.h"
#include "defs.h"
#include "pci.h"
#include "x86.h"

struct aml_region_handler aml_regions[256];
struct aml_region_handler *aml_device_regions;

struct aml_region_handler *aml_region_handler_of(struct aml_value *rgn) {
  struct aml_region_handler *h;
  struct aml_node *n;

  for (n = rgn->node; n != 0; n = n->parent)
    for (h = aml_device_regions; h != 0; h = h->next)
      if (h->dev == n && h->space == rgn->opreg.space)
        return h;
  return &aml_regions[rgn->opreg.space];
}

// REGION ACCESS

static int genio(int iodir, struct aml_region_handler *region, uint64_t address,
                 int access_size, int len, void *buffer) {
  uint8_t *pb = buffer;
  int reg;

  if (region->fn == 0) {
    printf("acpi: no handler for region space 0x%x\n", region->space);
    return -1;
  }
  for (reg = 0; reg < len; reg += access_size) {
    uint64_t value = 0;
    int err;

    if (iodir == ACPI_IOREAD) {
      err = region->fn(region->cookie, iodir, address + (uint64_t)reg,
                       access_size, &value);
      if (err)
        return err;
      switch (access_size) {
      case 1:
        *(uint8_t *)(pb + reg) = (uint8_t)value;
        break;
      case 2:
        *(uint16_t *)(pb + reg) = (uint16_t)value;
        break;
      case 4:
        *(uint32_t *)(pb + reg) = (uint32_t)value;
        break;
      case 8:
        *(uint64_t *)(pb + reg) = value;
        break;
      default:
        return -1;
      }
    } else {
      switch (access_size) {
      case 1:
        value = *(uint8_t *)(pb + reg);
        break;
      case 2:
        value = *(uint16_t *)(pb + reg);
        break;
      case 4:
        value = *(uint32_t *)(pb + reg);
        break;
      case 8:
        value = *(uint64_t *)(pb + reg);
        break;
      default:
        return -1;
      }
      err = region->fn(region->cookie, iodir, address + (uint64_t)reg,
                       access_size, &value);
      if (err)
        return err;
    }
  }
  return 0;
}

struct pciaddr {
  uint16_t reg;
  uint16_t fun;
  uint8_t dev;
  uint8_t bus;
  uint16_t seg;
};

static void pciaddr_of(struct aml_node *pcidev, uint64_t *out) {
  struct aml_node *path[10] = {0};
  struct pciaddr a;
  int64_t res;
  int n = 0;

  memset(&a, 0, sizeof(a));
  for (;;) {
    path[n] = pcidev;
    pcidev = pcidev->parent;
    if (pcidev == 0 || n == 9)
      break;
    n++;
  }
  for (; n >= 0; n--) {
    if (aml_eval_int(path[n], "_ADR", 0, 0, &res) == 0) {
      a.dev = (uint8_t)(res >> 16);
      a.fun = (uint16_t)(res & 0xFFFF);
    } else if (aml_search(path[n], "_HID") != 0) {
      if (aml_eval_int(path[n], "_SEG", 0, 0, &res) == 0) {
        a.seg = (uint16_t)res;
        a.bus = 0;
        a.dev = 0;
        a.fun = 0;
      }
      if (aml_eval_int(path[n], "_BBN", 0, 0, &res) == 0) {
        a.bus = (uint8_t)res;
        a.dev = 0;
        a.fun = 0;
      }
    } else {
      continue;
    }
    if (n == 0)
      break;
    if (a.seg == 0) {
      uint32_t cls = pci_read32(a.bus, a.dev, a.fun, 0x08);

      if ((cls >> 24) == 0x06 && ((cls >> 16) & 0xff) == 0x04) {
        a.bus = (uint8_t)((pci_read32(a.bus, a.dev, a.fun, 0x18) >> 8) & 0xff);
        a.dev = 0;
        a.fun = 0;
      }
    }
  }
  memcpy(out, &a, sizeof(*out));
}

static void rwgen(struct aml_value *rgn, int bpos, int blen,
                  struct aml_value *val, int mode, int flag) {
  struct aml_value tmp;
  uint64_t addr;
  struct aml_region_handler *handler;
  void *tbit, *vbit;
  int tlen, type, sz;

  memset(&tmp, 0, sizeof(tmp));
  switch (AML_FIELD_ACCESS(flag)) {
  case AML_FIELD_WORDACC:
    sz = 2;
    break;
  case AML_FIELD_DWORDACC:
    sz = 4;
    break;
  case AML_FIELD_QWORDACC:
    sz = 8;
    break;
  default:
    sz = 1;
    break;
  }

  addr = (rgn->opreg.base + (uint64_t)(bpos >> 3)) & ~(uint64_t)(sz - 1);
  bpos += (int)((rgn->opreg.base & (uint64_t)(sz - 1)) << 3);
  bpos &= (sz << 3) - 1;

  if (rgn->opreg.space == ACPI_OPREG_PCICFG) {
    uint64_t base;

    if (rgn->node == 0 || rgn->node->parent == 0) {
      aml_fail(0, "a PCI region that is nowhere");
      return;
    }
    pciaddr_of(rgn->node->parent, &base);
    addr = (base & ~(uint64_t)0xffff) | (addr & 0xffff);
  }

  tbit = &tmp.integer;
  vbit = &val->integer;
  tlen = ((bpos + blen + (sz << 3) - 1) / (sz << 3)) * (sz << 3);
  type = rgn->opreg.space;

  handler = aml_region_handler_of(rgn);
  if (handler->fn == 0) {
    printf("acpi: no handler for region space 0x%x\n", type);
    if (mode == ACPI_IOREAD)
      aml_setvalue(val, AML_INTEGER, 0, 0);
    return;
  }

  if (tlen > aml_intlen) {
    aml_setvalue(&tmp, AML_BUFFER, tlen >> 3, 0);
    tbit = tmp.buf;
  }
  if (blen > aml_intlen) {
    if (mode == ACPI_IOREAD) {
      aml_setvalue(val, AML_BUFFER, (blen + 7) >> 3, 0);
    } else {
      val = aml_convert(0, val, AML_BUFFER, -1);
      if (blen > (val->length << 3))
        blen = val->length << 3;
    }
    vbit = val->buf;
  } else {
    if (mode == ACPI_IOREAD)
      aml_setvalue(val, AML_INTEGER, 0, 0);
    else
      val = aml_convert(0, val, AML_INTEGER, -1);
    if (blen <= aml_intlen)
      vbit = &val->integer;
  }

  if (mode == ACPI_IOREAD) {
    genio(ACPI_IOREAD, handler, addr, sz, tlen >> 3, tbit);
    aml_bufcpy(vbit, 0, tbit, bpos, blen);
  } else {
    if (AML_FIELD_UPDATE(flag) == AML_FIELD_PRESERVE &&
        (bpos != 0 || blen != tlen)) {
      genio(ACPI_IOREAD, handler, addr, sz, tlen >> 3, tbit);
    } else if (AML_FIELD_UPDATE(flag) == AML_FIELD_WRITEASONES) {
      memset(tbit, 0xff, (size_t)(tlen >> 3));
    }
    aml_bufcpy(tbit, bpos, vbit, 0, blen);
    genio(ACPI_IOWRITE, handler, addr, sz, tlen >> 3, tbit);
    aml_delref(&val);
  }
  aml_freevalue(&tmp);
}

static void rwgsb(struct aml_value *conn __unused, int len, int bpos __unused, int blen __unused,
                  struct aml_value *val, int mode, int flag) {
  int buflen;

  switch ((flag >> 6) & 0x3) {
  case 0:
    switch (AML_FIELD_ATTR(flag)) {
    case 0x02:
      buflen = 0;
      break;
    case 0x04:
    case 0x06:
      buflen = 1;
      break;
    case 0x08:
      buflen = 2;
      break;
    default:
      buflen = len;
      break;
    }
    break;
  default:
    buflen = AML_FIELD_ATTR(flag);
    break;
  }
  if (mode == ACPI_IOREAD)
    aml_setvalue(val, AML_BUFFER, buflen + 2, 0);
  if (val->type == AML_BUFFER && val->length > 0)
    val->buf[0] = 5; // EIO
}

static void rwindexfield(struct aml_value *fld, struct aml_value *val,
                         int mode) {
  struct aml_value tmp, *data, *index;
  void *tbit, *vbit;
  int vpos, bpos, blen, indexval, sz, len;

  index = fld->field.index;
  data = fld->field.data;
  bpos = (int)fld->field.bitpos;
  blen = (int)fld->field.bitlen;

  memset(&tmp, 0, sizeof(tmp));
  tmp.refs = 1;
  switch (AML_FIELD_ACCESS(fld->field.flags)) {
  case AML_FIELD_WORDACC:
    sz = 2;
    break;
  case AML_FIELD_DWORDACC:
    sz = 4;
    break;
  case AML_FIELD_QWORDACC:
    sz = 8;
    break;
  default:
    sz = 1;
    break;
  }
  if (blen > aml_intlen) {
    if (mode == ACPI_IOREAD) {
      aml_setvalue(val, AML_BUFFER, (blen + 7) >> 3, 0);
    } else {
      val = aml_convert(0, val, AML_BUFFER, -1);
      if (blen > (val->length << 3))
        blen = val->length << 3;
    }
    vbit = val->buf;
  } else {
    if (mode == ACPI_IOREAD)
      aml_setvalue(val, AML_INTEGER, 0, 0);
    else
      val = aml_convert(0, val, AML_INTEGER, -1);
    vbit = &val->integer;
  }
  tbit = &tmp.integer;
  vpos = 0;

  indexval = (bpos >> 3) & ~(sz - 1);
  bpos = bpos - (indexval << 3);
  while (blen > 0) {
    len = blen < (sz << 3) - bpos ? blen : (sz << 3) - bpos;

    aml_setvalue(&tmp, AML_INTEGER, indexval, 0);
    aml_rwfield(index, 0, aml_intlen, &tmp, ACPI_IOWRITE);
    indexval += sz;
    aml_setvalue(&tmp, AML_INTEGER, 0, 0);
    if (mode == ACPI_IOWRITE)
      aml_bufcpy(tbit, 0, vbit, vpos, len);
    aml_rwfield(data, bpos, len, &tmp, mode);
    if (mode == ACPI_IOREAD)
      aml_bufcpy(vbit, vpos, tbit, 0, len);
    vpos += len;
    blen -= len;
    bpos = 0;
  }
  if (mode == ACPI_IOWRITE)
    aml_delref(&val);
}

// lock held
static int glock;

// the lock word sits in a packed table
static uint32_t *glk_word(struct acpi_facs *facs) {
  return (uint32_t *)((char *)facs + __builtin_offsetof(struct acpi_facs, global_lock));
}

void aml_glk_enter(void) {
  struct acpi_facs *facs = acpi_facs();

  if (glock++ != 0 || facs == 0)
    return;
  while (!acpi_acquire_glk(glk_word(facs)))
    ;
}

void aml_glk_leave(void) {
  struct acpi_facs *facs = acpi_facs();

  if (--glock != 0 || facs == 0)
    return;
  if (acpi_release_glk(glk_word(facs)))
    acpi_glk_release_signal();
}

// a field of a buffer
static void rwbuffer(struct aml_value *fld, struct aml_value *val, int mode) {
  struct aml_value *data = fld->field.data, *conv;
  int bpos = (int)fld->field.bitpos, blen = (int)fld->field.bitlen;

  if (data == 0 || data->type != AML_BUFFER ||
      (uint64_t)bpos + (uint64_t)blen > (uint64_t)data->length * 8) {
    aml_fail(0, "a field that is not in its buffer");
    if (mode == ACPI_IOREAD)
      aml_setvalue(val, AML_INTEGER, 0, 0);
    return;
  }
  if (mode == ACPI_IOREAD) {
    if (blen > aml_intlen) {
      aml_setvalue(val, AML_BUFFER, (blen + 7) >> 3, 0);
      aml_bufcpy(val->buf, 0, data->buf, bpos, blen);
    } else {
      aml_setvalue(val, AML_INTEGER, 0, 0);
      aml_bufcpy(&val->integer, 0, data->buf, bpos, blen);
    }
    return;
  }
  if (blen > aml_intlen) {
    conv = aml_convert(0, val, AML_BUFFER, -1);
    aml_bufcpy(data->buf, bpos, conv->buf, 0,
               blen < conv->length * 8 ? blen : conv->length * 8);
  } else {
    conv = aml_convert(0, val, AML_INTEGER, -1);
    aml_bufcpy(data->buf, bpos, &conv->integer, 0, blen);
  }
  aml_delref(&conv);
}

static int isfield(const struct aml_value *v) {
  return v != 0 && (v->type == AML_FIELDUNIT || v->type == AML_BUFFERFIELD);
}

static int isregion(const struct aml_value *v) {
  return v != 0 && v->type == AML_OPREGION;
}

void aml_rwfield(struct aml_value *fld, int bpos, int blen,
                 struct aml_value *val, int mode) {
  static int nest;
  struct aml_value *data, *index;
  int locked = AML_FIELD_LOCK(fld->field.flags);

  index = fld->field.index;
  data = fld->field.data;
  if (blen > (int)fld->field.bitlen)
    blen = (int)fld->field.bitlen;

  if (nest > 8 || fld->field.bitlen > AML_MAXFIELD || blen < 0 ||
      (fld->field.type == AMLOP_INDEXFIELD &&
       (!isfield(index) || !isfield(data))) ||
      (fld->field.type == AMLOP_BANKFIELD &&
       (!isfield(index) || !isregion(data))) ||
      (fld->field.type == AMLOP_FIELD && !isregion(data))) {
    aml_fail(0, "a field that is not what it says");
    if (mode == ACPI_IOREAD)
      aml_setvalue(val, AML_INTEGER, 0, 0);
    return;
  }
  nest++;
  if (locked)
    aml_glk_enter();
  if (fld->field.type == AMLOP_INDEXFIELD) {
    rwindexfield(fld, val, mode);
  } else if (fld->field.type == AMLOP_BANKFIELD) {
    struct aml_value tmp;

    memset(&tmp, 0, sizeof(tmp));
    tmp.refs = 1;
    aml_setvalue(&tmp, AML_INTEGER, fld->field.arg, 0);
    aml_rwfield(index, 0, aml_intlen, &tmp, ACPI_IOWRITE);
    rwgen(data, (int)fld->field.bitpos, (int)fld->field.bitlen, val, mode,
          fld->field.flags);
  } else if (fld->field.type == AMLOP_FIELD) {
    switch (data->opreg.space) {
    case ACPI_OPREG_GPIO:
      aml_fail(0, "a GeneralPurposeIO region, and no GPIO controller");
      if (mode == ACPI_IOREAD)
        aml_setvalue(val, AML_INTEGER, 0, 0);
      break;
    case ACPI_OPREG_GSB:
      rwgsb(index, fld->field.arg, (int)fld->field.bitpos + bpos, blen, val,
            mode, fld->field.flags);
      break;
    default:
      rwgen(data, (int)fld->field.bitpos + bpos, blen, val, mode,
            fld->field.flags);
      break;
    }
  } else {
    rwbuffer(fld, val, mode);
  }
  if (locked)
    aml_glk_leave();
  nest--;
}

void aml_createfield(struct aml_scope *scope, struct aml_value *field,
                     int opcode, struct aml_value *data, int bpos, int blen,
                     struct aml_value *index, int arg, int flags) {
  struct aml_value *own = 0;

  field->type = (opcode == AMLOP_FIELD || opcode == AMLOP_INDEXFIELD ||
                 opcode == AMLOP_BANKFIELD)
                    ? AML_FIELDUNIT
                    : AML_BUFFERFIELD;
  if (field->type == AML_BUFFERFIELD && data->type != AML_BUFFER)
    own = data = aml_convert(scope, data, AML_BUFFER, -1);
  if (bpos < 0 || blen < 0 || blen > AML_MAXFIELD ||
      (field->type == AML_BUFFERFIELD &&
       (uint64_t)bpos + (uint64_t)blen > (uint64_t)data->length * 8)) {
    aml_fail(scope, "a field of %d bits at bit %d, outside what holds it", blen,
             bpos);
    field->type = AML_UNINIT;
    aml_delref(&own);
    return;
  }
  field->field.type = (uint16_t)opcode;
  field->field.bitpos = (uint32_t)bpos;
  field->field.bitlen = (uint32_t)blen;
  field->field.arg = arg;
  field->field.index = index;
  field->field.data = data;
  field->field.flags = (uint16_t)flags;
  if (own == 0)
    aml_addref(data);
  aml_addref(index);
}

void aml_parsefieldlist(struct aml_scope *mscope, int opcode, int flags,
                        struct aml_value *data, struct aml_value *index,
                        int arg) {
  struct aml_value *conn = 0, *rv;
  int bpos = 0, blen;

  if (mscope == 0)
    return;
  while (mscope->pos < mscope->end) {
    switch (*mscope->pos) {
    case 0x00: // a gap
      mscope->pos++;
      blen = aml_parselength(mscope);
      break;
    case 0x01:
      mscope->pos++;
      blen = 0;
      flags = mscope->pos[0] | (mscope->pos[1] << 8);
      mscope->pos += 2;
      break;
    case 0x02:
      mscope->pos++;
      blen = 0;
      conn = aml_parse(mscope, 'o', "Connection");
      if (conn == 0)
        goto out;
      conn->node = mscope->node;
      break;
    case 0x03: // how, in full
      mscope->pos++;
      blen = 0;
      flags = mscope->pos[0] | (mscope->pos[1] << 8);
      arg = mscope->pos[2];
      mscope->pos += 3;
      break;
    default: // a name
      mscope->pos = aml_parsename(mscope->node, mscope->pos, &rv, 1);
      blen = aml_parselength(mscope);
      aml_createfield(mscope, rv, opcode, data, bpos, blen,
                      conn != 0 ? conn : index, arg, flags);
      aml_delref(&rv);
      break;
    }
    bpos += blen;
  }
out:
  aml_popscope(mscope);
}

// MUTEXES AND EVENTS

int aml_mutex_acquire(struct aml_scope *scope, struct aml_value *mtx,
                      int timeout) {
  if (mtx->mutex.owner == 0 || mtx->mutex.owner == scope->evalid) {
    mtx->mutex.owner = scope->evalid;
    mtx->mutex.count++;
    if (mtx == aml_global_lock)
      aml_glk_enter();
    return 0;
  }
  if (timeout == 0)
    return -1;
  mtx->mutex.owner = scope->evalid;
  mtx->mutex.count = 1;
  if (mtx == aml_global_lock)
    aml_glk_enter();
  return 0;
}

void aml_mutex_release(struct aml_scope *scope __unused, struct aml_value *mtx) {
  if (mtx == aml_global_lock)
    aml_glk_leave();
  if (mtx->mutex.count > 0 && --mtx->mutex.count == 0)
    mtx->mutex.owner = 0;
}

int aml_event_wait(struct aml_value *evt, int timeout) {
  while (evt->event.state == 0 && timeout >= 0) {
    if (acpi_task_run())
      continue;
    aml_msleep(1);
    if (timeout < AML_NO_TIMEOUT)
      timeout--;
  }
  if (evt->event.state == 0)
    return -1;
  evt->event.state--;
  return 0;
}

void aml_event_signal(struct aml_value *evt) { evt->event.state++; }

void aml_event_reset(struct aml_value *evt) { evt->event.state = 0; }

void aml_regions_init(void) {
  int i;

  for (i = 0; i < 256; i++)
    aml_regions[i].space = i;
  aml_regions[ACPI_OPREG_SYSMEM].fn = acpi_region_sysmem;
  aml_regions[ACPI_OPREG_SYSIO].fn = acpi_region_sysio;
  aml_regions[ACPI_OPREG_PCICFG].fn = acpi_region_pcicfg;
}
