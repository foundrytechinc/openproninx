// AML PARSER

#include "acpi.h"
#include "aml.h"
#include "aml_priv.h"
#include "defs.h"
#include "inc/stdarg.h"

#define A_INTEGER 'i'
#define A_BYTE 'b'
#define A_WORD 'w'
#define A_DWORD 'd'
#define A_QWORD 'q'
#define A_OBJLEN 'p'
#define A_STRING 'a'
#define A_BYTELIST 'B'
#define A_REVISION 'R'
#define A_METHOD 'M'
#define A_CREATENAME 'N'
#define A_SEARCHNAME 'n'
#define A_FIELDLIST 'F'
#define A_SIMPLENAME 's'
#define A_SUPERNAME 'S'
#define A_TERMOBJLIST 'T'
#define A_TERMOBJ 't'
#define A_IFELSE 'I'
#define A_STKARG 'A'
#define A_STKLOCAL 'L'
#define A_DEBUG 'D'
#define A_CONST 'c'
#define A_TARGET 'r'

#define AML_REVISION 1

struct aml_opcode {
  uint32_t opcode;
  const char *mnem;
  const char *args;
};

static const struct aml_opcode optable[] = {
    // simple types
    {AMLOP_ZERO, "Zero", "c"},
    {AMLOP_ONE, "One", "c"},
    {AMLOP_ONES, "Ones", "c"},
    {AMLOP_REVISION, "Revision", "R"},
    {AMLOP_BYTEPREFIX, ".Byte", "b"},
    {AMLOP_WORDPREFIX, ".Word", "w"},
    {AMLOP_DWORDPREFIX, ".DWord", "d"},
    {AMLOP_QWORDPREFIX, ".QWord", "q"},
    {AMLOP_STRINGPREFIX, ".String", "a"},
    {AMLOP_DEBUG, "DebugOp", "D"},
    {AMLOP_BUFFER, "Buffer", "piB"},
    {AMLOP_PACKAGE, "Package", "pbT"},
    {AMLOP_VARPACKAGE, "VarPackage", "piT"},

    // locals and arguments
    {AMLOP_LOCAL0, "Local0", "L"},
    {AMLOP_LOCAL0 + 1, "Local1", "L"},
    {AMLOP_LOCAL0 + 2, "Local2", "L"},
    {AMLOP_LOCAL0 + 3, "Local3", "L"},
    {AMLOP_LOCAL0 + 4, "Local4", "L"},
    {AMLOP_LOCAL0 + 5, "Local5", "L"},
    {AMLOP_LOCAL0 + 6, "Local6", "L"},
    {AMLOP_LOCAL7, "Local7", "L"},
    {AMLOP_ARG0, "Arg0", "A"},
    {AMLOP_ARG0 + 1, "Arg1", "A"},
    {AMLOP_ARG0 + 2, "Arg2", "A"},
    {AMLOP_ARG0 + 3, "Arg3", "A"},
    {AMLOP_ARG0 + 4, "Arg4", "A"},
    {AMLOP_ARG0 + 5, "Arg5", "A"},
    {AMLOP_ARG6, "Arg6", "A"},

    // control flow
    {AMLOP_IF, "If", "piI"},
    {AMLOP_ELSE, "Else", "pT"},
    {AMLOP_WHILE, "While", "piT"},
    {AMLOP_BREAK, "Break", ""},
    {AMLOP_CONTINUE, "Continue", ""},
    {AMLOP_RETURN, "Return", "t"},
    {AMLOP_FATAL, "Fatal", "bdi"},
    {AMLOP_NOP, "Nop", ""},
    {AMLOP_BREAKPOINT, "BreakPoint", ""},

    // arithmetic
    {AMLOP_INCREMENT, "Increment", "S"},
    {AMLOP_DECREMENT, "Decrement", "S"},
    {AMLOP_ADD, "Add", "iir"},
    {AMLOP_SUBTRACT, "Subtract", "iir"},
    {AMLOP_MULTIPLY, "Multiply", "iir"},
    {AMLOP_DIVIDE, "Divide", "iirr"},
    {AMLOP_SHL, "ShiftLeft", "iir"},
    {AMLOP_SHR, "ShiftRight", "iir"},
    {AMLOP_AND, "And", "iir"},
    {AMLOP_NAND, "Nand", "iir"},
    {AMLOP_OR, "Or", "iir"},
    {AMLOP_NOR, "Nor", "iir"},
    {AMLOP_XOR, "Xor", "iir"},
    {AMLOP_NOT, "Not", "ir"},
    {AMLOP_MOD, "Mod", "iir"},
    {AMLOP_FINDSETLEFTBIT, "FindSetLeftBit", "ir"},
    {AMLOP_FINDSETRIGHTBIT, "FindSetRightBit", "ir"},

    // logic
    {AMLOP_LAND, "LAnd", "ii"},
    {AMLOP_LOR, "LOr", "ii"},
    {AMLOP_LNOT, "LNot", "i"},
    {AMLOP_LNOTEQUAL, "LNotEqual", "tt"},
    {AMLOP_LLESSEQUAL, "LLessEqual", "tt"},
    {AMLOP_LGREATEREQUAL, "LGreaterEqual", "tt"},
    {AMLOP_LEQUAL, "LEqual", "tt"},
    {AMLOP_LGREATER, "LGreater", "tt"},
    {AMLOP_LLESS, "LLess", "tt"},

    // names
    {AMLOP_NAMECHAR, ".NameRef", "n"},
    {AMLOP_ALIAS, "Alias", "nN"},
    {AMLOP_NAME, "Name", "Nt"},
    {AMLOP_EVENT, "Event", "N"},
    {AMLOP_MUTEX, "Mutex", "Nb"},
    {AMLOP_DATAREGION, "DataRegion", "Nttt"},
    {AMLOP_OPREGION, "OpRegion", "Nbii"},
    {AMLOP_SCOPE, "Scope", "pnT"},
    {AMLOP_DEVICE, "Device", "pNT"},
    {AMLOP_POWERRSRC, "Power Resource", "pNbwT"},
    {AMLOP_THERMALZONE, "ThermalZone", "pNT"},
    {AMLOP_PROCESSOR, "Processor", "pNbdbT"},
    {AMLOP_METHOD, "Method", "pNbM"},

    // fields
    {AMLOP_FIELD, "Field", "pnbF"},
    {AMLOP_INDEXFIELD, "IndexField", "pnnbF"},
    {AMLOP_BANKFIELD, "BankField", "pnnibF"},
    {AMLOP_CREATEFIELD, "CreateField", "tiiN"},
    {AMLOP_CREATEQWORDFIELD, "CreateQWordField", "tiN"},
    {AMLOP_CREATEDWORDFIELD, "CreateDWordField", "tiN"},
    {AMLOP_CREATEWORDFIELD, "CreateWordField", "tiN"},
    {AMLOP_CREATEBYTEFIELD, "CreateByteField", "tiN"},
    {AMLOP_CREATEBITFIELD, "CreateBitField", "tiN"},

    // conversion
    {AMLOP_TOINTEGER, "ToInteger", "tr"},
    {AMLOP_TOBUFFER, "ToBuffer", "tr"},
    {AMLOP_TODECSTRING, "ToDecString", "tr"},
    {AMLOP_TOHEXSTRING, "ToHexString", "tr"},
    {AMLOP_TOSTRING, "ToString", "tir"},
    {AMLOP_MID, "Mid", "tiir"},
    {AMLOP_FROMBCD, "FromBCD", "ir"},
    {AMLOP_TOBCD, "ToBCD", "ir"},

    // mutexes and events
    {AMLOP_ACQUIRE, "Acquire", "Sw"},
    {AMLOP_RELEASE, "Release", "S"},
    {AMLOP_SIGNAL, "Signal", "S"},
    {AMLOP_WAIT, "Wait", "Si"},
    {AMLOP_RESET, "Reset", "S"},

    {AMLOP_INDEX, "Index", "tir"},
    {AMLOP_DEREFOF, "DerefOf", "t"},
    {AMLOP_REFOF, "RefOf", "S"},
    {AMLOP_CONDREFOF, "CondRef", "Sr"},

    {AMLOP_LOADTABLE, "LoadTable", "tttttt"},
    {AMLOP_STALL, "Stall", "i"},
    {AMLOP_SLEEP, "Sleep", "i"},
    {AMLOP_TIMER, "Timer", ""},
    {AMLOP_LOAD, "Load", "nS"},
    {AMLOP_UNLOAD, "Unload", "t"},
    {AMLOP_STORE, "Store", "tS"},
    {AMLOP_CONCAT, "Concat", "ttr"},
    {AMLOP_CONCATRES, "ConcatRes", "ttt"},
    {AMLOP_NOTIFY, "Notify", "Si"},
    {AMLOP_SIZEOF, "Sizeof", "S"},
    {AMLOP_MATCH, "Match", "tbibii"},
    {AMLOP_OBJECTTYPE, "ObjectType", "S"},
    {AMLOP_COPYOBJECT, "CopyObject", "tS"},
};

#define HASH_OFF 6904
#define HASH_SIZE 179
#define HASH_KEY(k) ((((uint32_t)(k)) ^ HASH_OFF) % HASH_SIZE)

static const struct aml_opcode *ophash[HASH_SIZE];

void aml_hashopcodes(void) {
  uint i;

  for (i = 0; i < sizeof(optable) / sizeof(optable[0]); i++) {
    if (ophash[HASH_KEY(optable[i].opcode)] != 0)
      panic("aml: opcode hash collision at %s", optable[i].mnem);
    ophash[HASH_KEY(optable[i].opcode)] = &optable[i];
  }
}

static const struct aml_opcode *findopcode(int opcode) {
  const struct aml_opcode *op = ophash[HASH_KEY(opcode)];

  return op != 0 && (int)op->opcode == opcode ? op : 0;
}

static struct aml_scope *lastscope;
static int recursion;

int aml_loop_seconds = 30;
static int64_t evalstart;
static uint loops;

void aml_evalbegin(void) {
  struct fnu_timespec now;

  uptime_now(&now);
  evalstart = now.sec;
  loops = 0;
}

static int aml_runaway(struct aml_scope *scope) {
  static int64_t said;
  struct fnu_timespec now;

  if (++loops % 1024 != 0)
    return 0;
  uptime_now(&now);
  if (now.sec - evalstart >= 5 && said != evalstart) {
    said = evalstart;
    printf("acpi: %s has looped for 5 s\n", aml_nodename(scope->node));
  }
  return now.sec - evalstart > aml_loop_seconds;
}

struct aml_value *aml_fail(struct aml_scope *scope, const char *fmt, ...) {
  va_list ap;
  char msg[160];
  struct aml_scope *s;

  if (scope == 0)
    scope = lastscope;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  printf("acpi: AML error %s at 0x%x: %s\n",
          scope != 0 ? aml_nodename(scope->node) : "", scope != 0 ? (uint)(scope->pos - aml_base) : 0, msg);
  for (s = scope; s != 0; s = s->parent)
    s->pos = s->end;
  aml_error++;
  return aml_newvalue(AML_INTEGER, 0, 0);
}

// READING THE STREAM

static int parseopcode(struct aml_scope *scope) {
  int opcode, twocode;

  if (scope->pos >= scope->end)
    return AMLOP_INVALID;
  opcode = scope->pos[0];
  switch (opcode) {
  case AMLOP_ROOTCHAR:
  case AMLOP_PARENTPREFIX:
  case AMLOP_MULTINAMEPREFIX:
  case AMLOP_DUALNAMEPREFIX:
  case AMLOP_NAMECHAR:
    return AMLOP_NAMECHAR;
  }
  if (opcode >= 'A' && opcode <= 'Z')
    return AMLOP_NAMECHAR;

  if (opcode != AMLOP_LNOT && opcode != AMLOP_EXTPREFIX) {
    scope->pos += 1;
    return opcode;
  }
  if (scope->pos + 1 >= scope->end)
    return AMLOP_INVALID;
  twocode = (scope->pos[0] << 8) + scope->pos[1];
  if (opcode == AMLOP_LNOT && twocode != AMLOP_LNOTEQUAL &&
      twocode != AMLOP_LLESSEQUAL && twocode != AMLOP_LGREATEREQUAL) {
    scope->pos += 1;
    return opcode;
  }
  scope->pos += 2;
  return twocode;
}

int aml_parselength(struct aml_scope *scope) {
  int len;
  uint8_t lcode = *scope->pos++;

  if (lcode <= 0x3F)
    return lcode;
  len = lcode & 0xF;
  len += *scope->pos++ << 4;
  if (lcode >= 0x80)
    len += *scope->pos++ << 12;
  if (lcode >= 0xC0)
    len += *scope->pos++ << 20;
  return len;
}

static uint8_t *parseend(struct aml_scope *scope) {
  uint8_t *pos = scope->pos;
  int len = aml_parselength(scope);

  if (len < 0 || len > scope->end - pos)
    return scope->end;
  if (pos + len < scope->pos)
    return scope->pos;
  return pos + len;
}

// SCOPES

struct aml_scope *aml_findscope(struct aml_scope *scope, int type,
                                int endscope) {
  while (scope != 0) {
    switch (endscope) {
    case AMLOP_RETURN:
      scope->pos = scope->end;
      if (scope->type == AMLOP_WHILE)
        scope->pos = 0;
      break;
    case AMLOP_CONTINUE:
      scope->pos = scope->end;
      break;
    case AMLOP_BREAK:
      scope->pos = scope->end;
      if (scope->type == type)
        scope->parent->pos = scope->end;
      break;
    }
    if (scope->type == type)
      break;
    scope = scope->parent;
  }
  return scope;
}

struct aml_value *aml_getstack(struct aml_scope *scope, int opcode) {
  struct aml_value *sp = 0;

  scope = aml_findscope(scope, AMLOP_METHOD, 0);
  if (scope == 0)
    return 0;
  if (opcode >= AMLOP_LOCAL0 && opcode <= AMLOP_LOCAL7) {
    if (scope->locals == 0)
      scope->locals = aml_newvalue(AML_PACKAGE, AML_MAX_LOCAL, 0);
    sp = scope->locals->pkg[opcode - AMLOP_LOCAL0];
    sp->stack = opcode;
  } else if (opcode >= AMLOP_ARG0 && opcode <= AMLOP_ARG6) {
    if (scope->args == 0)
      scope->args = aml_newvalue(AML_PACKAGE, AML_MAX_ARG, 0);
    sp = scope->args->pkg[opcode - AMLOP_ARG0];
    if (sp->type == AML_OBJREF)
      sp = sp->objref.ref;
  }
  return sp;
}

struct aml_scope *aml_pushscope(struct aml_scope *parent,
                                struct aml_value *range, struct aml_node *node,
                                int type) {
  struct aml_scope *scope;
  uint8_t *start, *end;

  if (range->type == AML_METHOD) {
    start = range->method.start;
    end = range->method.end;
  } else {
    start = range->buf;
    end = start + range->length;
    if (start == end)
      return 0;
  }
  scope = aml_alloc(sizeof(*scope));
  scope->node = node;
  scope->start = start;
  scope->end = end;
  scope->pos = start;
  scope->parent = parent;
  scope->type = type;
  if (parent != 0) {
    scope->depth = parent->depth + 1;
    scope->evalid = parent->evalid;
  } else {
    scope->evalid = aml_evalid();
  }
  lastscope = scope;
  return scope;
}

struct aml_scope *aml_popscope(struct aml_scope *scope) {
  struct aml_scope *next;

  if (scope == 0)
    return 0;
  next = scope->parent;
  if (scope->type == AMLOP_METHOD)
    aml_delchildren(scope->node);
  if (scope->locals != 0) {
    aml_freevalue(scope->locals);
    aml_release(scope->locals);
  }
  if (scope->args != 0) {
    aml_freevalue(scope->args);
    aml_release(scope->args);
  }
  aml_release(scope);
  lastscope = next;
  return next;
}

static struct aml_value *gettgt(struct aml_value *val, int opcode) {
  while (val != 0 && val->type == AML_OBJREF) {
    if (opcode == AMLOP_STORE && val->objref.type == AMLOP_PACKAGE)
      return val;
    val = val->objref.ref;
  }
  return val;
}

// STORING

#define INT_BITS 0x7fffffff
static int bits(int64_t n) { return n < 0 || n > INT_BITS ? -1 : (int)n; }

static int bitoff(int64_t bytes) {
  if (bytes < 0 || bytes > INT_BITS / 8)
    return -1;
  return (int)(bytes * 8);
}

static void store(struct aml_scope *scope, struct aml_value *lhs, int64_t ival,
                  struct aml_value *rhs) {
  struct aml_value tmp;
  struct aml_node *node;
  int mlen;

  if (lhs == rhs || lhs == 0 || lhs->type == AML_NOTARGET)
    return;
  memset(&tmp, 0, sizeof(tmp));
  tmp.refs = 1;
  if (rhs == 0)
    rhs = aml_setvalue(&tmp, AML_INTEGER, ival, 0);
  if (rhs->type == AML_BUFFERFIELD || rhs->type == AML_FIELDUNIT) {
    aml_rwfield(rhs, 0, (int)rhs->field.bitlen, &tmp, ACPI_IOREAD);
    rhs = &tmp;
  }
  if (lhs->stack >= AMLOP_LOCAL0 && lhs->stack <= AMLOP_LOCAL7)
    aml_freevalue(lhs);
  lhs = gettgt(lhs, AMLOP_STORE);
  if (lhs->stack >= AMLOP_LOCAL0 && lhs->stack <= AMLOP_LOCAL7)
    aml_freevalue(lhs);
  switch (lhs->type) {
  case AML_UNINIT:
    aml_copyvalue(lhs, rhs);
    break;
  case AML_BUFFERFIELD:
  case AML_FIELDUNIT:
    aml_rwfield(lhs, 0, (int)lhs->field.bitlen, rhs, ACPI_IOWRITE);
    break;
  case AML_DEBUGOBJ:
    break;
  case AML_INTEGER:
    rhs = aml_convert(scope, rhs, lhs->type, -1);
    lhs->integer = rhs->integer;
    aml_delref(&rhs);
    break;
  case AML_BUFFER:
  case AML_STRING:
    rhs = aml_convert(scope, rhs, lhs->type, -1);
    if (lhs->length < rhs->length) {
      aml_freevalue(lhs);
      aml_setvalue(lhs, rhs->type, rhs->length, 0);
    }
    mlen = lhs->length < rhs->length ? lhs->length : rhs->length;
    memset(lhs->buf, 0, (size_t)lhs->length);
    memcpy(lhs->buf, rhs->buf, (size_t)mlen);
    aml_delref(&rhs);
    break;
  case AML_PACKAGE:
    if (rhs->type != AML_PACKAGE) {
      aml_fail(scope, "a package is stored over what is no package");
      break;
    }
    aml_freevalue(lhs);
    aml_copyvalue(lhs, rhs);
    break;
  case AML_NAMEREF:
    node = aml_search_create(scope->node, aml_getname(lhs->nameref.name), 1);
    if (node == 0) {
      aml_fail(scope, "no way to make %s", aml_getname(lhs->nameref.name));
      break;
    }
    aml_copyvalue(node->value, rhs);
    break;
  case AML_OBJREF:
    if (lhs->objref.type != AMLOP_PACKAGE) {
      aml_fail(scope, "a package was to be stored into");
      break;
    }
    aml_freevalue(lhs->objref.ref);
    aml_copyvalue(lhs->objref.ref, rhs);
    break;
  case AML_METHOD:
    if (rhs->type != AML_INTEGER) {
      aml_fail(scope, "a method is stored over with what is no integer");
      break;
    }
    aml_freevalue(lhs);
    aml_copyvalue(lhs, rhs);
    break;
  default:
    aml_fail(scope, "a store into type %x", lhs->type);
    break;
  }
  aml_freevalue(&tmp);
}

// RUNNING A VALUE

struct aml_value *aml_run(struct aml_scope *scope, struct aml_value *my_ret,
                          int ret_type, int argc, struct aml_value *argv) {
  struct aml_value *tmp = my_ret, *sp;
  struct aml_scope *ms;
  int idx;

  if (my_ret == 0)
    return 0;
  switch (tmp->type) {
  case AML_NAMEREF:
    my_ret = aml_fail(scope, "undefined name %s", aml_getname(tmp->nameref.name));
    break;
  case AML_METHOD:
    if (recursion > 60 || (scope != 0 && scope->depth > 60)) {
      my_ret = aml_fail(scope, "calls nest too deep at %s", aml_nodename(tmp->node));
      break;
    }
    ms = aml_pushscope(scope, tmp, tmp->node, AMLOP_METHOD);
    if (ms == 0) {
      my_ret = aml_newvalue(AML_INTEGER, 0, 0);
      break;
    }
    for (idx = 0; idx < AML_METHOD_ARGCOUNT(tmp->method.flags); idx++) {
      sp = aml_getstack(ms, AMLOP_ARG0 + idx);
      if (argv != 0) {
        if (idx < argc)
          aml_copyvalue(sp, &argv[idx]);
      } else {
        aml_setvalue(sp, AML_OBJREF, AMLOP_ARG0 + idx, 0);
        sp->objref.ref = aml_parse(scope, 't', "ARGX");
      }
    }
    if (aml_error) {
      aml_popscope(ms);
      my_ret = aml_newvalue(AML_INTEGER, 0, 0);
      break;
    }
    aml_base = tmp->method.base;
    if (tmp->method.builtin != 0) {
      my_ret = tmp->method.builtin(ms);
    } else {
      aml_parse(ms, 'T', "METHEVAL");
      my_ret = ms->retv;
    }
    aml_popscope(ms);
    break;
  case AML_BUFFERFIELD:
  case AML_FIELDUNIT:
    my_ret = aml_newvalue(0, 0, 0);
    aml_rwfield(tmp, 0, (int)tmp->field.bitlen, my_ret, ACPI_IOREAD);
    break;
  }
  if (ret_type == 'i' && my_ret != 0 && my_ret->type != AML_INTEGER) {
    printf("acpi: %s is not an integer\n", aml_nodename(tmp->node));
    aml_fail(scope, "an integer was wanted");
  }
  return my_ret;
}

static struct aml_value *parsesimple(struct aml_scope *scope, char ch,
                                     struct aml_value *rv) {
  uint64_t v;

  if (rv == 0)
    rv = aml_newvalue(0, 0, 0);
  switch (ch) {
  case A_REVISION:
    aml_setvalue(rv, AML_INTEGER, AML_REVISION, 0);
    break;
  case A_DEBUG:
    aml_setvalue(rv, AML_DEBUGOBJ, 0, 0);
    break;
  case A_BYTE:
    aml_setvalue(rv, AML_INTEGER, scope->pos[0], 0);
    scope->pos += 1;
    break;
  case A_WORD:
    aml_setvalue(rv, AML_INTEGER, scope->pos[0] | (scope->pos[1] << 8), 0);
    scope->pos += 2;
    break;
  case A_DWORD:
    memcpy(&v, scope->pos, 4);
    v &= 0xffffffffull;
    aml_setvalue(rv, AML_INTEGER, (int64_t)v, 0);
    scope->pos += 4;
    break;
  case A_QWORD:
    memcpy(&v, scope->pos, 8);
    aml_setvalue(rv, AML_INTEGER, (int64_t)v, 0);
    scope->pos += 8;
    break;
  case A_STRING:
    aml_setvalue(rv, AML_STRING, -1, scope->pos);
    scope->pos += rv->length + 1;
    break;
  }
  return rv;
}

// LOADING MORE

struct aml_value *aml_loadtable(const char *sig, const char *oemid,
                                const char *oemtable, const char *rootpath) {
  int i;

  for (i = 0; i < acpi_table_count(); i++) {
    struct acpi_sdt_header *hdr = acpi_table(i);

    if (strncmp(hdr->signature, sig, 4) == 0 &&
        strncmp(hdr->oem_id, oemid, 6) == 0 &&
        strncmp(hdr->oem_table_id, oemtable, 8) == 0) {
      uint8_t *code = acpi_table_aml(i);

      if (code == 0)
        break;
      aml_load(code, hdr->length - (uint)sizeof(*hdr), rootpath);
      return aml_newvalue(AML_DDBHANDLE, 0, 0);
    }
  }
  return aml_newvalue(AML_INTEGER, 0, 0);
}

struct aml_scope *aml_loadregion(struct aml_scope *scope, struct aml_value *rgn,
                                 struct aml_value *ddb) {
  struct aml_value tmp;
  uint8_t *code;
  uint len;

  ddb->type = AML_DDBHANDLE;
  ddb->integer = 0;
  memset(&tmp, 0, sizeof(tmp));
  if (rgn->type != AML_OPREGION || rgn->opreg.space != ACPI_OPREG_SYSMEM)
    goto fail;
  if ((code = acpi_table_copy(rgn->opreg.base, &len)) == 0)
    goto fail;
  tmp.buf = code;
  tmp.length = (int)len;
  return aml_pushscope(scope, &tmp, scope->node, AMLOP_LOAD);
fail:
  printf("acpi: cannot load %s\n", aml_nodename(rgn->node));
  return 0;
}

// THE PARSER

struct aml_value *aml_parse(struct aml_scope *scope, int ret_type,
                            const char *stype __unused) {
  int opcode, idx, pc;
  const struct aml_opcode *htab;
  struct aml_value *opargs[8], *my_ret, *rv;
  struct aml_scope *mscope, *iscope;
  uint8_t *start, *end;
  const char *ch;
  int64_t ival, rem;

  my_ret = 0;
  if (scope == 0 || scope->pos >= scope->end || aml_error)
    return 0;
  if (recursion++ > 125) {
    recursion--;
    return aml_fail(scope, "nested too deep");
  }
  end = scope->end;
  iscope = scope;
start:
  // the opcode
  start = scope->pos;
  pc = (int)(scope->pos - aml_base);
  opcode = parseopcode(scope);
  htab = findopcode(opcode);
  if (htab == 0) {
    my_ret = aml_fail(scope, "unknown opcode %x at %x", opcode, pc);
    memset(opargs, 0, sizeof(opargs));
    goto done;
  }

  // its operands
  memset(opargs, 0, sizeof(opargs));
  idx = 0;
  for (ch = htab->args; *ch; ch++) {
    rv = 0;
    switch (*ch) {
    case A_OBJLEN:
      end = parseend(scope);
      break;
    case A_IFELSE:
      ch = (end < scope->end && *end == AMLOP_ELSE) ? "-TbpT" : "-T";
      break;
    case 's':
    case 'S':
    case A_TARGET:
    case A_TERMOBJ:
    case A_INTEGER:
      if (*ch == 'r' && scope->pos < scope->end &&
          *scope->pos == AMLOP_ZERO) {
        rv = aml_newvalue(AML_NOTARGET, 0, 0);
        scope->pos++;
      } else {
        rv = aml_parse(scope, *ch, htab->mnem);
        if (rv == 0)
          goto done;
      }
      break;
    case A_BYTELIST:
    case A_METHOD:
    case A_FIELDLIST:
    case A_TERMOBJLIST:
      rv = aml_newvalue(AML_SCOPE, 0, 0);
      rv->buf = scope->pos;
      if (end > scope->pos) {
        rv->length = (int)(end - scope->pos);
        scope->pos = end;
      }
      break;
    case A_CONST:
      rv = aml_newvalue(AML_INTEGER, (char)opcode, 0);
      break;
    case A_CREATENAME:
      scope->pos = aml_parsename(scope->node, scope->pos, &rv, 1);
      break;
    case A_SEARCHNAME:
      scope->pos = aml_parsename(scope->node, scope->pos, &rv, 0);
      break;
    case A_BYTE:
    case A_WORD:
    case A_DWORD:
    case A_QWORD:
    case A_DEBUG:
    case A_STRING:
    case A_REVISION:
      rv = parsesimple(scope, *ch, 0);
      break;
    case A_STKLOCAL:
    case A_STKARG:
      rv = aml_getstack(scope, opcode);
      aml_addref(rv);
      break;
    default:
      aml_fail(scope, "no such operand letter %c", *ch);
      break;
    }
    if (rv != 0)
      opargs[idx++] = rv;
    if (aml_error)
      goto done;
  }

  // what it does
  ival = 0;
  my_ret = 0;
  mscope = 0;
  switch (opcode) {
  case AMLOP_NOP:
  case AMLOP_BREAKPOINT:
    break;
  case AMLOP_LOCAL0:
  case AMLOP_LOCAL0 + 1:
  case AMLOP_LOCAL0 + 2:
  case AMLOP_LOCAL0 + 3:
  case AMLOP_LOCAL0 + 4:
  case AMLOP_LOCAL0 + 5:
  case AMLOP_LOCAL0 + 6:
  case AMLOP_LOCAL7:
  case AMLOP_ARG0:
  case AMLOP_ARG0 + 1:
  case AMLOP_ARG0 + 2:
  case AMLOP_ARG0 + 3:
  case AMLOP_ARG0 + 4:
  case AMLOP_ARG0 + 5:
  case AMLOP_ARG6:
    my_ret = opargs[0];
    break;
  case AMLOP_NAMECHAR:
    my_ret = opargs[0];
    if (my_ret == 0) {
      my_ret = aml_fail(scope, "a name that is nothing");
      break;
    }
    if (scope->type == AMLOP_PACKAGE && my_ret->node != 0) {
      my_ret = aml_newvalue(AML_OBJREF, AMLOP_NAMECHAR, opargs[0]);
      aml_addref(my_ret->objref.ref);
    } else if (my_ret->type == AML_OBJREF) {
      my_ret = my_ret->objref.ref;
      aml_addref(my_ret);
    }
    if (my_ret != 0 && (ret_type == 'i' || ret_type == 't' || ret_type == 'T'))
      my_ret = aml_run(scope, my_ret, ret_type, 0, 0);
    break;

  case AMLOP_ZERO:
  case AMLOP_ONE:
  case AMLOP_ONES:
  case AMLOP_DEBUG:
  case AMLOP_REVISION:
  case AMLOP_BYTEPREFIX:
  case AMLOP_WORDPREFIX:
  case AMLOP_DWORDPREFIX:
  case AMLOP_QWORDPREFIX:
  case AMLOP_STRINGPREFIX:
    my_ret = opargs[0];
    break;

  case AMLOP_BUFFER:
    my_ret = aml_newvalue(AML_BUFFER, opargs[0]->integer, 0);
    memcpy(my_ret->buf, opargs[1]->buf,
           (size_t)(opargs[1]->length < my_ret->length ? opargs[1]->length
                                                       : my_ret->length));
    break;
  case AMLOP_PACKAGE:
  case AMLOP_VARPACKAGE:
    my_ret = aml_newvalue(AML_PACKAGE, opargs[0]->integer, 0);
    mscope = aml_pushscope(scope, opargs[1], scope->node, AMLOP_PACKAGE);
    for (idx = 0; mscope != 0 && idx < my_ret->length; idx++) {
      rv = aml_parse(mscope, 'o', "Package");
      if (rv != 0) {
        aml_delref(&my_ret->pkg[idx]);
        my_ret->pkg[idx] = rv;
      }
    }
    aml_popscope(mscope);
    mscope = 0;
    break;

  // arithmetic and logic
  case AMLOP_OR:
  case AMLOP_ADD:
  case AMLOP_AND:
  case AMLOP_NAND:
  case AMLOP_XOR:
  case AMLOP_SHL:
  case AMLOP_SHR:
  case AMLOP_NOR:
  case AMLOP_MOD:
  case AMLOP_SUBTRACT:
  case AMLOP_MULTIPLY:
    ival = (int64_t)aml_evalexpr((uint64_t)opargs[0]->integer,
                                 (uint64_t)opargs[1]->integer, opcode);
    store(scope, opargs[2], ival, 0);
    break;
  case AMLOP_DIVIDE:
    if (opargs[1]->integer == 0) {
      my_ret = aml_fail(scope, "divide by zero");
      break;
    }
    rem = (int64_t)aml_evalexpr((uint64_t)opargs[0]->integer,
                                (uint64_t)opargs[1]->integer, AMLOP_MOD);
    ival = (int64_t)aml_evalexpr((uint64_t)opargs[0]->integer,
                                 (uint64_t)opargs[1]->integer, AMLOP_DIVIDE);
    store(scope, opargs[2], rem, 0);
    store(scope, opargs[3], ival, 0);
    break;
  case AMLOP_NOT:
  case AMLOP_TOBCD:
  case AMLOP_FROMBCD:
  case AMLOP_FINDSETLEFTBIT:
  case AMLOP_FINDSETRIGHTBIT:
    ival = (int64_t)aml_evalexpr((uint64_t)opargs[0]->integer, 0, opcode);
    store(scope, opargs[1], ival, 0);
    break;
  case AMLOP_INCREMENT:
  case AMLOP_DECREMENT:
    my_ret = aml_run(scope, opargs[0], 'i', 0, 0);
    ival = (int64_t)aml_evalexpr((uint64_t)my_ret->integer, 1, opcode);
    store(scope, opargs[0], ival, 0);
    break;
  case AMLOP_LNOT:
    ival = (int64_t)aml_evalexpr((uint64_t)opargs[0]->integer, 0, opcode);
    break;
  case AMLOP_LOR:
  case AMLOP_LAND:
    ival = (int64_t)aml_evalexpr((uint64_t)opargs[0]->integer,
                                 (uint64_t)opargs[1]->integer, opcode);
    break;
  case AMLOP_LLESS:
  case AMLOP_LEQUAL:
  case AMLOP_LGREATER:
  case AMLOP_LNOTEQUAL:
  case AMLOP_LLESSEQUAL:
  case AMLOP_LGREATEREQUAL:
    ival = aml_compare(scope, opargs[0], opargs[1], opcode);
    break;

  // references and stores
  case AMLOP_CONDREFOF:
    ival = 0;
    if (opargs[0]->node != 0) {
      rv = aml_newvalue(AML_OBJREF, opcode, opargs[0]);
      aml_addref(opargs[0]);
      store(scope, opargs[1], 0, rv);
      aml_delref(&rv);
      ival = -1;
    }
    break;
  case AMLOP_REFOF:
    my_ret = aml_newvalue(AML_OBJREF, opcode, opargs[0]);
    aml_addref(my_ret->objref.ref);
    break;
  case AMLOP_INDEX:
    idx = (int)opargs[1]->integer;
    if (idx >= opargs[0]->length || idx < 0)
      break;
    switch (opargs[0]->type) {
    case AML_PACKAGE:
      if (ret_type == 't' || ret_type == 'i' || ret_type == 'T') {
        my_ret = opargs[0]->pkg[idx];
        aml_addref(my_ret);
      } else {
        my_ret = aml_newvalue(AML_OBJREF, AMLOP_PACKAGE, opargs[0]->pkg[idx]);
        my_ret->objref.index = idx;
        aml_addref(my_ret->objref.ref);
      }
      break;
    case AML_BUFFER:
    case AML_STRING:
    case AML_INTEGER:
      rv = aml_convert(scope, opargs[0], AML_BUFFER, -1);
      if (ret_type == 't' || ret_type == 'i' || ret_type == 'T') {
        ival = rv->buf[idx];
      } else {
        my_ret = aml_newvalue(0, 0, 0);
        aml_createfield(scope, my_ret, AMLOP_INDEX, rv, 8 * idx, 8, 0, 0,
                        AML_FIELD_BYTEACC);
      }
      aml_delref(&rv);
      break;
    default:
      my_ret = aml_fail(scope, "Index of type %x", opargs[0]->type);
      break;
    }
    store(scope, opargs[2], ival, my_ret);
    break;
  case AMLOP_DEREFOF:
    if (opargs[0]->type == AML_OBJREF) {
      my_ret = opargs[0]->objref.ref;
      aml_addref(my_ret);
    } else {
      my_ret = opargs[0];
    }
    break;
  case AMLOP_COPYOBJECT:
    my_ret = opargs[0];
    aml_freevalue(opargs[1]);
    aml_copyvalue(opargs[1], opargs[0]);
    break;
  case AMLOP_STORE:
    my_ret = opargs[0];
    store(scope, opargs[1], 0, opargs[0]);
    break;

  // conversions
  case AMLOP_TOINTEGER:
    my_ret = aml_convert(scope, opargs[0], AML_INTEGER, -1);
    store(scope, opargs[1], 0, my_ret);
    break;
  case AMLOP_TOBUFFER:
    my_ret = aml_convert(scope, opargs[0], AML_BUFFER, -1);
    store(scope, opargs[1], 0, my_ret);
    break;
  case AMLOP_TOHEXSTRING:
    my_ret = aml_convert(scope, opargs[0], AML_HEXSTRING, -1);
    store(scope, opargs[1], 0, my_ret);
    break;
  case AMLOP_TODECSTRING:
    my_ret = aml_convert(scope, opargs[0], AML_DECSTRING, -1);
    store(scope, opargs[1], 0, my_ret);
    break;
  case AMLOP_TOSTRING:
    my_ret = aml_convert(scope, opargs[0], AML_STRING, (int)opargs[1]->integer);
    store(scope, opargs[2], 0, my_ret);
    break;
  case AMLOP_CONCAT:
    my_ret = aml_concat(scope, opargs[0], opargs[1]);
    store(scope, opargs[2], 0, my_ret);
    break;
  case AMLOP_CONCATRES:
    my_ret = aml_concatres(scope, opargs[0], opargs[1]);
    store(scope, opargs[2], 0, my_ret);
    break;
  case AMLOP_MID:
    my_ret = aml_mid(opargs[0], (int)opargs[1]->integer, (int)opargs[2]->integer);
    store(scope, opargs[3], 0, my_ret);
    break;
  case AMLOP_MATCH:
    ival = aml_match(scope, opargs[0], (int)opargs[5]->integer,
                     (int)opargs[1]->integer, (int)opargs[2]->integer,
                     (int)opargs[3]->integer, (int)opargs[4]->integer);
    break;
  case AMLOP_SIZEOF:
    rv = gettgt(opargs[0], opcode);
    ival = rv->length;
    break;
  case AMLOP_OBJECTTYPE:
    rv = gettgt(opargs[0], opcode);
    ival = rv->type;
    break;

  // mutexes and events
  case AMLOP_ACQUIRE:
    rv = gettgt(opargs[0], opcode);
    ival = aml_mutex_acquire(scope, rv, (int)opargs[1]->integer);
    break;
  case AMLOP_RELEASE:
    rv = gettgt(opargs[0], opcode);
    aml_mutex_release(scope, rv);
    break;
  case AMLOP_WAIT:
    rv = gettgt(opargs[0], opcode);
    ival = aml_event_wait(rv, (int)opargs[1]->integer);
    break;
  case AMLOP_RESET:
    rv = gettgt(opargs[0], opcode);
    aml_event_reset(rv);
    break;
  case AMLOP_SIGNAL:
    rv = gettgt(opargs[0], opcode);
    aml_event_signal(rv);
    break;

  // named objects
  case AMLOP_NAME:
    rv = opargs[0];
    aml_freevalue(rv);
    aml_copyvalue(rv, opargs[1]);
    break;
  case AMLOP_ALIAS:
    if ((rv = gettgt(opargs[0], opcode)) == 0) {
      printf("acpi: alias of a name that is not there\n");
      rv = opargs[1];
      break;
    }
    aml_addref(rv);
    rv = aml_setvalue(opargs[1], AML_OBJREF, opcode, rv);
    break;
  case AMLOP_OPREGION:
    rv = aml_setvalue(opargs[0], AML_OPREGION, 0, 0);
    rv->opreg.space = (uint8_t)opargs[1]->integer;
    rv->opreg.base = (uint64_t)opargs[2]->integer;
    rv->opreg.len = (uint32_t)opargs[3]->integer;
    break;
  case AMLOP_DATAREGION:
    rv = aml_setvalue(opargs[0], AML_OPREGION, 0, 0);
    rv->opreg.space = GAS_SYSTEM_MEMORY;
    my_ret = aml_fail(scope, "DataTableRegion is not here");
    break;
  case AMLOP_EVENT:
    rv = aml_setvalue(opargs[0], AML_EVENT, 0, 0);
    rv->event.state = 0;
    break;
  case AMLOP_MUTEX:
    rv = aml_setvalue(opargs[0], AML_MUTEX, 0, 0);
    rv->mutex.synclevel = (int)opargs[1]->integer;
    break;
  case AMLOP_SCOPE:
    rv = opargs[0];
    if (rv->type == AML_NAMEREF) {
      printf("acpi: no scope %s\n", aml_getname(rv->nameref.name));
      break;
    }
    mscope = aml_pushscope(scope, opargs[1], rv->node, opcode);
    break;
  case AMLOP_DEVICE:
    rv = aml_setvalue(opargs[0], AML_DEVICE, 0, 0);
    mscope = aml_pushscope(scope, opargs[1], rv->node, opcode);
    break;
  case AMLOP_THERMALZONE:
    rv = aml_setvalue(opargs[0], AML_THERMZONE, 0, 0);
    mscope = aml_pushscope(scope, opargs[1], rv->node, opcode);
    break;
  case AMLOP_POWERRSRC:
    rv = aml_setvalue(opargs[0], AML_POWERRSRC, 0, 0);
    rv->power.level = (uint8_t)opargs[1]->integer;
    rv->power.order = (uint16_t)opargs[2]->integer;
    mscope = aml_pushscope(scope, opargs[3], rv->node, opcode);
    break;
  case AMLOP_PROCESSOR:
    rv = aml_setvalue(opargs[0], AML_PROCESSOR, 0, 0);
    rv->proc.id = (uint8_t)opargs[1]->integer;
    rv->proc.addr = (uint32_t)opargs[2]->integer;
    rv->proc.len = (uint8_t)opargs[3]->integer;
    mscope = aml_pushscope(scope, opargs[4], rv->node, opcode);
    break;
  case AMLOP_METHOD:
    rv = aml_setvalue(opargs[0], AML_METHOD, 0, 0);
    rv->method.flags = (int)opargs[1]->integer;
    rv->method.start = opargs[2]->buf;
    rv->method.end = rv->method.start + opargs[2]->length;
    rv->method.base = aml_base;
    break;

  // fields
  case AMLOP_CREATEFIELD:
    rv = aml_setvalue(opargs[3], AML_BUFFERFIELD, 0, 0);
    aml_createfield(scope, rv, opcode, opargs[0], bits(opargs[1]->integer),
                    bits(opargs[2]->integer), 0, 0, 0);
    break;
  case AMLOP_CREATEBITFIELD:
    rv = aml_setvalue(opargs[2], AML_BUFFERFIELD, 0, 0);
    aml_createfield(scope, rv, opcode, opargs[0], bits(opargs[1]->integer), 1, 0,
                    0, 0);
    break;
  case AMLOP_CREATEBYTEFIELD:
    rv = aml_setvalue(opargs[2], AML_BUFFERFIELD, 0, 0);
    aml_createfield(scope, rv, opcode, opargs[0], bitoff(opargs[1]->integer), 8,
                    0, 0, AML_FIELD_BYTEACC);
    break;
  case AMLOP_CREATEWORDFIELD:
    rv = aml_setvalue(opargs[2], AML_BUFFERFIELD, 0, 0);
    aml_createfield(scope, rv, opcode, opargs[0], bitoff(opargs[1]->integer),
                    16, 0, 0, AML_FIELD_WORDACC);
    break;
  case AMLOP_CREATEDWORDFIELD:
    rv = aml_setvalue(opargs[2], AML_BUFFERFIELD, 0, 0);
    aml_createfield(scope, rv, opcode, opargs[0], bitoff(opargs[1]->integer),
                    32, 0, 0, AML_FIELD_DWORDACC);
    break;
  case AMLOP_CREATEQWORDFIELD:
    rv = aml_setvalue(opargs[2], AML_BUFFERFIELD, 0, 0);
    aml_createfield(scope, rv, opcode, opargs[0], bitoff(opargs[1]->integer),
                    64, 0, 0, AML_FIELD_QWORDACC);
    break;
  case AMLOP_FIELD:
    mscope = aml_pushscope(scope, opargs[2], scope->node, opcode);
    aml_parsefieldlist(mscope, opcode, (int)opargs[1]->integer, opargs[0], 0, 0);
    mscope = 0;
    break;
  case AMLOP_INDEXFIELD:
    mscope = aml_pushscope(scope, opargs[3], scope->node, opcode);
    aml_parsefieldlist(mscope, opcode, (int)opargs[2]->integer, opargs[1],
                       opargs[0], 0);
    mscope = 0;
    break;
  case AMLOP_BANKFIELD:
    mscope = aml_pushscope(scope, opargs[4], scope->node, opcode);
    aml_parsefieldlist(mscope, opcode, (int)opargs[3]->integer, opargs[0],
                       opargs[1], (int)opargs[2]->integer);
    mscope = 0;
    break;

  // the rest
  case AMLOP_STALL:
    delay((uint)opargs[0]->integer);
    break;
  case AMLOP_SLEEP:
    aml_msleep((int)opargs[0]->integer);
    break;
  case AMLOP_NOTIFY:
    rv = gettgt(opargs[0], opcode);
    aml_notify(rv->node, (int)opargs[1]->integer);
    break;
  case AMLOP_TIMER: {
    // in hundreds of nanoseconds
    struct fnu_timespec now;

    uptime_now(&now);
    ival = now.sec * 10000000 + now.nsec / 100;
    break;
  }
  case AMLOP_FATAL:
    my_ret = aml_fail(scope, "Fatal %x %x %x", (int)opargs[0]->integer,
                      (int)opargs[1]->integer, (int)opargs[2]->integer);
    break;
  case AMLOP_LOADTABLE:
    my_ret = aml_loadtable(opargs[0]->str, opargs[1]->str, opargs[2]->str,
                           opargs[3]->str);
    break;
  case AMLOP_LOAD:
    mscope = aml_loadregion(scope, opargs[0], opargs[1]);
    break;
  case AMLOP_UNLOAD:
    my_ret = aml_fail(scope, "Unload is not here");
    break;

  // control flow
  case AMLOP_IF:
    if (opargs[0]->integer) {
      mscope = aml_pushscope(scope, opargs[1], scope->node, AMLOP_IF);
    } else if (opargs[3] != 0) {
      mscope = aml_pushscope(scope, opargs[3], scope->node, AMLOP_ELSE);
    }
    break;
  case AMLOP_WHILE:
    if (opargs[0]->integer && aml_runaway(scope)) {
      my_ret = aml_fail(scope, "a loop that does not end");
    } else if (opargs[0]->integer) {
      scope->pos = start;
      mscope = aml_pushscope(scope, opargs[1], scope->node, AMLOP_WHILE);
    }
    break;
  case AMLOP_BREAK:
    aml_findscope(scope, AMLOP_WHILE, AMLOP_BREAK);
    break;
  case AMLOP_CONTINUE:
    aml_findscope(scope, AMLOP_WHILE, AMLOP_CONTINUE);
    break;
  case AMLOP_RETURN:
    mscope = aml_findscope(scope, AMLOP_METHOD, AMLOP_RETURN);
    if (mscope == 0) {
      my_ret = aml_fail(scope, "Return outside a method");
      break;
    }
    if (mscope->retv != 0) {
      my_ret = aml_fail(scope, "two Returns");
      mscope = 0;
      break;
    }
    mscope->retv = aml_newvalue(0, 0, 0);
    aml_copyvalue(mscope->retv, opargs[0]);
    mscope = 0;
    break;
  default:
    my_ret = aml_fail(scope, "opcode %x (%s) not done", opcode, htab->mnem);
    break;
  }
  if (mscope != 0)
    scope = mscope;
  if ((ret_type == 'i' || ret_type == 't') && my_ret == 0)
    my_ret = aml_newvalue(AML_INTEGER, ival, 0);
  if (ret_type == 'i' && my_ret != 0 && my_ret->type != AML_INTEGER) {
    rv = aml_convert(scope, my_ret, AML_INTEGER, -1);
    for (idx = 0; idx < 8; idx++)
      if (opargs[idx] == my_ret)
        opargs[idx] = 0;
    aml_delref(&my_ret);
    my_ret = rv;
  }

done:
  // the operands are given up
  for (idx = 0; idx < 8; idx++) {
    if (opargs[idx] == my_ret)
      opargs[idx] = 0;
    aml_delref(&opargs[idx]);
  }

  if (ret_type == 'T') {
    aml_delref(&my_ret);
    my_ret = 0;
    while (scope->pos >= scope->end && scope != iscope)
      scope = aml_popscope(scope);
    if (scope->pos != 0 && scope->pos < scope->end)
      goto start;
  }
  recursion--;
  return my_ret;
}
