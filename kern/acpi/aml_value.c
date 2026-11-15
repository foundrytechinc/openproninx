// AML VALUES

#include "acpi.h"
#include "aml.h"
#include "aml_priv.h"
#include "defs.h"
#include "malloc.h"

int aml_intlen = 64;

int64_t aml_intmask(int64_t v) {
  return aml_intlen == 32 ? (int64_t)(uint32_t)v : v;
}

void *aml_alloc(size_t size) { return malloc(size, M_ACPI, M_WAITOK | M_ZERO); }

void aml_release(void *p) { free(p, M_ACPI); }

// BITS

int aml_tstbit(const uint8_t *pb, int bit) {
  return pb[bit >> 3] & (1 << (bit & 7));
}

void aml_setbit(uint8_t *pb, int bit, int val) {
  if (val)
    pb[bit >> 3] |= (uint8_t)(1 << (bit & 7));
  else
    pb[bit >> 3] &= (uint8_t)~(1 << (bit & 7));
}

void aml_bufcpy(void *dst, int dpos, const void *src, int spos, int len) {
  const uint8_t *s = src;
  uint8_t *d = dst;
  int i;

  if (((dpos | spos | len) & 7) == 0) {
    memcpy(d + (dpos >> 3), s + (spos >> 3), (size_t)((len + 7) >> 3));
    return;
  }
  for (i = 0; i < len; i++)
    aml_setbit(d, i + dpos, aml_tstbit(s, i + spos));
}

// VALUES

void aml_addref(struct aml_value *v) {
  if (v != 0)
    v->refs++;
}

void aml_delref(struct aml_value **pv) {
  struct aml_value *v;

  if (pv == 0 || *pv == 0)
    return;
  v = *pv;
  if (--v->refs == 0) {
    aml_freevalue(v);
    aml_release(v);
    *pv = 0;
  }
}

static int64_t aml_limit(int64_t n, uint64_t size) {
  if (n >= 0 && n <= 0x7fffffff && (uint64_t)n * size < get_free_ram())
    return n;
  printf("acpi: %ld items of %lu bytes asked for, not given\n", (long)n,
          (unsigned long)size);
  aml_error++;
  return 0;
}

struct aml_value *aml_setvalue(struct aml_value *lhs, int type, int64_t ival,
                               const void *bval) {
  int i;

  AML_CLEAR(lhs);
  lhs->type = type;
  switch (type) {
  case AML_INTEGER:
    lhs->length = aml_intlen >> 3;
    lhs->integer = aml_intmask(ival);
    break;
  case AML_METHOD:
    lhs->method.flags = (int)ival;
    lhs->method.builtin = (struct aml_value * (*)(struct aml_scope *)) bval;
    break;
  case AML_NAMEREF:
    lhs->nameref.name = (uint8_t *)bval;
    break;
  case AML_OBJREF:
    lhs->objref.type = (int)ival;
    lhs->objref.ref = (struct aml_value *)bval;
    break;
  case AML_BUFFER:
    ival = aml_limit(ival, 1);
    lhs->length = (int)ival;
    lhs->buf = aml_alloc((size_t)ival + 1);
    if (bval != 0)
      memcpy(lhs->buf, bval, (size_t)ival);
    break;
  case AML_STRING:
    if (ival == -1)
      ival = (int64_t)strlen((const char *)bval);
    ival = aml_limit(ival, 1);
    lhs->length = (int)ival;
    lhs->str = aml_alloc((size_t)ival + 1);
    if (bval != 0)
      strncpy(lhs->str, (const char *)bval, (size_t)ival);
    break;
  case AML_PACKAGE:
    ival = aml_limit(ival, sizeof(struct aml_value *) + sizeof(struct aml_value));
    lhs->length = (int)ival;
    lhs->pkg = aml_alloc((size_t)ival * sizeof(struct aml_value *) + 1);
    for (i = 0; i < lhs->length; i++)
      lhs->pkg[i] = aml_newvalue(AML_UNINIT, 0, 0);
    break;
  }
  return lhs;
}

struct aml_value *aml_newvalue(int type, int64_t ival, const void *bval) {
  struct aml_value *v = aml_alloc(sizeof(*v));

  v->refs = 1;
  return aml_setvalue(v, type, ival, bval);
}

void aml_freevalue(struct aml_value *v) {
  int i;

  if (v == 0)
    return;
  switch (v->type) {
  case AML_STRING:
    aml_release(v->str);
    break;
  case AML_BUFFER:
    aml_release(v->buf);
    break;
  case AML_PACKAGE:
    for (i = 0; i < v->length; i++)
      aml_delref(&v->pkg[i]);
    aml_release(v->pkg);
    break;
  case AML_OBJREF:
    aml_delref(&v->objref.ref);
    break;
  case AML_BUFFERFIELD:
  case AML_FIELDUNIT:
    aml_delref(&v->field.data);
    aml_delref(&v->field.index);
    break;
  }
  v->type = 0;
  AML_CLEAR(v);
}

void aml_copyvalue(struct aml_value *lhs, struct aml_value *rhs) {
  int i;

  lhs->type = rhs->type;
  switch (lhs->type) {
  case AML_UNINIT:
    break;
  case AML_INTEGER:
    lhs->length = aml_intlen >> 3;
    lhs->integer = rhs->integer;
    break;
  case AML_MUTEX:
    lhs->mutex = rhs->mutex;
    break;
  case AML_POWERRSRC:
    lhs->node = rhs->node;
    lhs->power = rhs->power;
    break;
  case AML_METHOD:
    lhs->method = rhs->method;
    break;
  case AML_BUFFER:
    aml_setvalue(lhs, rhs->type, rhs->length, rhs->buf);
    break;
  case AML_STRING:
    aml_setvalue(lhs, rhs->type, rhs->length, rhs->str);
    break;
  case AML_OPREGION:
    lhs->opreg = rhs->opreg;
    break;
  case AML_PROCESSOR:
    lhs->node = rhs->node;
    lhs->proc = rhs->proc;
    break;
  case AML_NAMEREF:
    lhs->nameref = rhs->nameref;
    break;
  case AML_PACKAGE:
    aml_setvalue(lhs, rhs->type, rhs->length, 0);
    for (i = 0; i < rhs->length; i++)
      aml_copyvalue(lhs->pkg[i], rhs->pkg[i]);
    break;
  case AML_OBJREF:
    lhs->objref = rhs->objref;
    aml_addref(lhs->objref.ref);
    break;
  case AML_DEVICE:
  case AML_THERMZONE:
    lhs->node = rhs->node;
    break;
  default:
    printf("acpi: copyvalue of type %x\n", rhs->type);
    break;
  }
}

// CONVERSION

int64_t aml_hextoint(const char *str) {
  int64_t v = 0;
  int c;

  for (; *str != 0; str++) {
    if (*str >= '0' && *str <= '9')
      c = *str - '0';
    else if (*str >= 'a' && *str <= 'f')
      c = *str - 'a' + 10;
    else if (*str >= 'A' && *str <= 'F')
      c = *str - 'A' + 10;
    else
      break;
    v = (v << 4) + c;
  }
  return v;
}

int64_t aml_val2int(struct aml_value *v) {
  int64_t ival = 0;

  if (v == 0)
    return 0;
  switch (v->type) {
  case AML_INTEGER:
    ival = v->integer;
    break;
  case AML_BUFFER:
    aml_bufcpy(&ival, 0, v->buf, 0, v->length * 8 < aml_intlen ? v->length * 8 : aml_intlen);
    break;
  case AML_STRING:
    ival = aml_hextoint(v->str);
    break;
  }
  return ival;
}

const char *aml_val2str(const struct aml_value *v) {
  static char buf[256];
  int n;

  switch (v->type) {
  case AML_BUFFER:
    n = v->length < (int)sizeof(buf) - 1 ? v->length : (int)sizeof(buf) - 1;
    memcpy(buf, v->buf, (size_t)n);
    buf[n] = 0;
    break;
  case AML_STRING:
    strlcpy(buf, v->str, sizeof(buf));
    break;
  case AML_INTEGER:
    snprintf(buf, sizeof(buf), "%lx", (unsigned long)v->integer);
    break;
  default:
    snprintf(buf, sizeof(buf), "type %d", v->type);
  }
  return buf;
}

struct aml_value *aml_tryconv(struct aml_value *a, int ctype, int clen) {
  struct aml_value *c = 0;

  if (clen == -1)
    clen = a->length;
  if (a->type == ctype) {
    aml_addref(a);
    return a;
  }
  switch (ctype) {
  case AML_BUFFER:
    switch (a->type) {
    case AML_INTEGER:
      c = aml_newvalue(AML_BUFFER, a->length, &a->integer);
      break;
    case AML_STRING:
      c = aml_newvalue(AML_BUFFER, a->length, a->str);
      break;
    case AML_BUFFERFIELD:
    case AML_FIELDUNIT:
      c = aml_newvalue(AML_BUFFER, 0, 0);
      aml_rwfield(a, 0, a->field.bitlen, c, ACPI_IOREAD);
      break;
    }
    break;
  case AML_INTEGER:
    switch (a->type) {
    case AML_BUFFER:
      c = aml_newvalue(AML_INTEGER, 0, 0);
      memcpy(&c->integer, a->buf,
             (size_t)(a->length < c->length ? a->length : c->length));
      break;
    case AML_STRING:
      c = aml_newvalue(AML_INTEGER, aml_hextoint(a->str), 0);
      break;
    case AML_UNINIT:
      c = aml_newvalue(AML_INTEGER, 0, 0);
      break;
    case AML_BUFFERFIELD:
    case AML_FIELDUNIT:
      if (a->field.bitlen > (uint32_t)aml_intlen)
        break;
      c = aml_newvalue(AML_INTEGER, 0, 0);
      aml_rwfield(a, 0, a->field.bitlen, c, ACPI_IOREAD);
      break;
    }
    break;
  case AML_STRING:
  case AML_HEXSTRING:
  case AML_DECSTRING:
    switch (a->type) {
    case AML_INTEGER:
      c = aml_newvalue(AML_STRING, 20, 0);
      snprintf(c->str, (size_t)c->length,
               ctype == AML_HEXSTRING ? "0x%lx" : "%ld", (long)a->integer);
      break;
    case AML_BUFFER:
      c = aml_newvalue(AML_STRING, a->length, a->buf);
      break;
    case AML_STRING:
      aml_addref(a);
      return a;
    case AML_PACKAGE:
      c = aml_newvalue(AML_STRING, 0, 0);
      break;
    }
    break;
  }
  return c;
}

struct aml_value *aml_convert(struct aml_scope *scope, struct aml_value *a,
                              int ctype, int clen) {
  struct aml_value *c = aml_tryconv(a, ctype, clen);

  if (c == 0) {
    aml_fail(scope, "cannot convert %x to %x", a->type, ctype);
    c = aml_newvalue(ctype == AML_INTEGER || ctype == AML_BUFFER ? ctype : AML_STRING, 0, "");
  }
  return c;
}

// MATH

static uint64_t convradix(uint64_t val, int iradix, int oradix) {
  uint64_t rv = 0, pwr = 1;

  while (val != 0) {
    rv += (val % (uint64_t)iradix) * pwr;
    val /= (uint64_t)iradix;
    pwr *= (uint64_t)oradix;
  }
  return rv;
}

static int lsb(uint64_t val) {
  int n;

  if (val == 0)
    return 0;
  for (n = 1; !(val & 1); n++)
    val >>= 1;
  return n;
}

static int msb(uint64_t val) {
  int n;

  if (val == 0)
    return 0;
  for (n = 1; val != 1; n++)
    val >>= 1;
  return n;
}

uint64_t aml_evalexpr(uint64_t lhs, uint64_t rhs, int opcode) {
  uint64_t res = 0;

  switch (opcode) {
  case AMLOP_INCREMENT:
  case AMLOP_ADD:
    res = lhs + rhs;
    break;
  case AMLOP_DECREMENT:
  case AMLOP_SUBTRACT:
    res = lhs - rhs;
    break;
  case AMLOP_MULTIPLY:
    res = lhs * rhs;
    break;
  case AMLOP_DIVIDE:
    res = rhs != 0 ? lhs / rhs : 0;
    break;
  case AMLOP_MOD:
    res = rhs != 0 ? lhs % rhs : 0;
    break;
  case AMLOP_SHL:
    res = rhs < 64 ? lhs << rhs : 0;
    break;
  case AMLOP_SHR:
    res = rhs < 64 ? lhs >> rhs : 0;
    break;
  case AMLOP_AND:
    res = lhs & rhs;
    break;
  case AMLOP_NAND:
    res = ~(lhs & rhs);
    break;
  case AMLOP_OR:
    res = lhs | rhs;
    break;
  case AMLOP_NOR:
    res = ~(lhs | rhs);
    break;
  case AMLOP_XOR:
    res = lhs ^ rhs;
    break;
  case AMLOP_NOT:
    res = ~lhs;
    break;
  case AMLOP_FINDSETLEFTBIT:
    res = (uint64_t)msb(lhs);
    break;
  case AMLOP_FINDSETRIGHTBIT:
    res = (uint64_t)lsb(lhs);
    break;
  case AMLOP_TOINTEGER:
    res = lhs;
    break;
  case AMLOP_FROMBCD:
    res = convradix(lhs, 16, 10);
    break;
  case AMLOP_TOBCD:
    res = convradix(lhs, 10, 16);
    break;
  case AMLOP_LAND:
    res = -(uint64_t)(lhs && rhs);
    break;
  case AMLOP_LOR:
    res = -(uint64_t)(lhs || rhs);
    break;
  case AMLOP_LNOT:
    res = -(uint64_t)(!lhs);
    break;
  case AMLOP_LNOTEQUAL:
    res = -(uint64_t)(lhs != rhs);
    break;
  case AMLOP_LLESSEQUAL:
    res = -(uint64_t)(lhs <= rhs);
    break;
  case AMLOP_LGREATEREQUAL:
    res = -(uint64_t)(lhs >= rhs);
    break;
  case AMLOP_LEQUAL:
    res = -(uint64_t)(lhs == rhs);
    break;
  case AMLOP_LGREATER:
    res = -(uint64_t)(lhs > rhs);
    break;
  case AMLOP_LLESS:
    res = -(uint64_t)(lhs < rhs);
    break;
  }
  return res;
}

// COMPARISON

int aml_compare(struct aml_scope *scope, struct aml_value *a1,
                struct aml_value *a2, int opcode) {
  struct aml_value *cv;
  int rc, n;

  if (a1->type != AML_INTEGER && a1->type != AML_STRING &&
      a1->type != AML_BUFFER) {
    cv = aml_tryconv(a1, AML_INTEGER, -1);
    if (cv == 0)
      cv = aml_convert(scope, a1, AML_BUFFER, -1);
    a1 = cv;
  }
  a2 = aml_convert(scope, a2, a1->type, -1);
  if (a1->type == AML_INTEGER) {
    rc = (int)aml_evalexpr((uint64_t)a1->integer, (uint64_t)a2->integer, opcode);
  } else {
    n = a1->length < a2->length ? a1->length : a2->length;
    rc = memcmp(a1->buf, a2->buf, (size_t)n);
    if (rc == 0)
      rc = a1->length - a2->length;
    rc = (int)aml_evalexpr((uint64_t)(int64_t)rc, 0, opcode);
  }
  aml_delref(&a2);
  return rc;
}

struct aml_value *aml_concat(struct aml_scope *scope, struct aml_value *a1,
                             struct aml_value *a2) {
  struct aml_value *c = 0;

  if (a1->type != AML_INTEGER && a1->type != AML_STRING &&
      a1->type != AML_BUFFER)
    a1 = aml_convert(scope, a1, AML_STRING, -1);
  a2 = aml_convert(scope, a2, a1->type, -1);
  switch (a1->type) {
  case AML_INTEGER:
    c = aml_newvalue(AML_BUFFER, a1->length + a2->length, 0);
    memcpy(c->buf, &a1->integer, (size_t)a1->length);
    memcpy(c->buf + a1->length, &a2->integer, (size_t)a2->length);
    break;
  case AML_BUFFER:
    c = aml_newvalue(AML_BUFFER, a1->length + a2->length, 0);
    memcpy(c->buf, a1->buf, (size_t)a1->length);
    memcpy(c->buf + a1->length, a2->buf, (size_t)a2->length);
    break;
  case AML_STRING:
    c = aml_newvalue(AML_STRING, a1->length + a2->length, 0);
    memcpy(c->str, a1->str, (size_t)a1->length);
    memcpy(c->str + a1->length, a2->str, (size_t)a2->length);
    break;
  default:
    aml_fail(scope, "concat of %d and %d", a1->type, a2->type);
    c = aml_newvalue(AML_STRING, 0, "");
    break;
  }
  aml_delref(&a2);
  return c;
}

static int ccrlen(int idx __unused, union acpi_resource *rs, void *arg) {
  *(int *)arg += AML_CRSLEN(rs);
  return 0;
}

struct aml_value *aml_concatres(struct aml_scope *scope, struct aml_value *a1,
                                struct aml_value *a2) {
  struct aml_value *c;
  int l1 = 0, l2 = 0;
  static const uint8_t end[2] = {SRT_ENDTAG, 0x00};

  if (a1->type != AML_BUFFER || a2->type != AML_BUFFER) {
    aml_fail(scope, "ConcatenateResTemplate of what is no buffer");
    return aml_newvalue(AML_INTEGER, 0, 0);
  }
  aml_parse_resource(a1, ccrlen, &l1);
  aml_parse_resource(a2, ccrlen, &l2);
  c = aml_newvalue(AML_BUFFER, l1 + l2 + 2, 0);
  memcpy(c->buf, a1->buf, (size_t)l1);
  memcpy(c->buf + l1, a2->buf, (size_t)l2);
  memcpy(c->buf + l1 + l2, end, 2);
  return c;
}

struct aml_value *aml_mid(struct aml_value *src, int index, int length) {
  if (index > src->length)
    index = src->length;
  if (index + length > src->length)
    length = src->length - index;
  return aml_newvalue(src->type, length, src->buf + index);
}

// MATCH

static int matchtest(int64_t a, int64_t b, int op) {
  switch (op) {
  case AML_MATCH_TR:
    return 1;
  case AML_MATCH_EQ:
    return a == b;
  case AML_MATCH_LT:
    return a < b;
  case AML_MATCH_LE:
    return a <= b;
  case AML_MATCH_GE:
    return a >= b;
  case AML_MATCH_GT:
    return a > b;
  }
  return 0;
}

int aml_match(struct aml_scope *scope, struct aml_value *pkg, int index,
              int op1, int v1, int op2, int v2) {
  struct aml_value *tmp;
  int flag;

  if (pkg->type != AML_PACKAGE) {
    aml_fail(scope, "Match over what is no package");
    return -1;
  }
  for (index = index < 0 ? 0 : index; index < pkg->length; index++) {
    tmp = aml_convert(scope, pkg->pkg[index], AML_INTEGER, -1);
    flag = matchtest(tmp->integer, v1, op1) && matchtest(tmp->integer, v2, op2);
    aml_delref(&tmp);
    if (flag)
      return index;
  }
  return -1;
}
