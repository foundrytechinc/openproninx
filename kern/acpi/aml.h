#ifndef PRONINX_AML_H
#define PRONINX_AML_H

#include "inc/types.h"

// AML

// OPCODES
#define AMLOP_ZERO 0x00
#define AMLOP_ONE 0x01
#define AMLOP_ALIAS 0x06
#define AMLOP_NAME 0x08
#define AMLOP_BYTEPREFIX 0x0A
#define AMLOP_WORDPREFIX 0x0B
#define AMLOP_DWORDPREFIX 0x0C
#define AMLOP_STRINGPREFIX 0x0D
#define AMLOP_QWORDPREFIX 0x0E
#define AMLOP_SCOPE 0x10
#define AMLOP_BUFFER 0x11
#define AMLOP_PACKAGE 0x12
#define AMLOP_VARPACKAGE 0x13
#define AMLOP_METHOD 0x14
#define AMLOP_DUALNAMEPREFIX 0x2E
#define AMLOP_MULTINAMEPREFIX 0x2F
#define AMLOP_EXTPREFIX 0x5B
#define AMLOP_MUTEX 0x5B01
#define AMLOP_EVENT 0x5B02
#define AMLOP_CONDREFOF 0x5B12
#define AMLOP_CREATEFIELD 0x5B13
#define AMLOP_LOADTABLE 0x5B1F
#define AMLOP_LOAD 0x5B20
#define AMLOP_STALL 0x5B21
#define AMLOP_SLEEP 0x5B22
#define AMLOP_ACQUIRE 0x5B23
#define AMLOP_SIGNAL 0x5B24
#define AMLOP_WAIT 0x5B25
#define AMLOP_RESET 0x5B26
#define AMLOP_RELEASE 0x5B27
#define AMLOP_FROMBCD 0x5B28
#define AMLOP_TOBCD 0x5B29
#define AMLOP_UNLOAD 0x5B2A
#define AMLOP_REVISION 0x5B30
#define AMLOP_DEBUG 0x5B31
#define AMLOP_FATAL 0x5B32
#define AMLOP_TIMER 0x5B33
#define AMLOP_OPREGION 0x5B80
#define AMLOP_FIELD 0x5B81
#define AMLOP_DEVICE 0x5B82
#define AMLOP_PROCESSOR 0x5B83
#define AMLOP_POWERRSRC 0x5B84
#define AMLOP_THERMALZONE 0x5B85
#define AMLOP_INDEXFIELD 0x5B86
#define AMLOP_BANKFIELD 0x5B87
#define AMLOP_DATAREGION 0x5B88
#define AMLOP_ROOTCHAR 0x5C
#define AMLOP_PARENTPREFIX 0x5E
#define AMLOP_NAMECHAR 0x5F
#define AMLOP_LOCAL0 0x60
#define AMLOP_LOCAL7 0x67
#define AMLOP_ARG0 0x68
#define AMLOP_ARG6 0x6E
#define AMLOP_STORE 0x70
#define AMLOP_REFOF 0x71
#define AMLOP_ADD 0x72
#define AMLOP_CONCAT 0x73
#define AMLOP_SUBTRACT 0x74
#define AMLOP_INCREMENT 0x75
#define AMLOP_DECREMENT 0x76
#define AMLOP_MULTIPLY 0x77
#define AMLOP_DIVIDE 0x78
#define AMLOP_SHL 0x79
#define AMLOP_SHR 0x7A
#define AMLOP_AND 0x7B
#define AMLOP_NAND 0x7C
#define AMLOP_OR 0x7D
#define AMLOP_NOR 0x7E
#define AMLOP_XOR 0x7F
#define AMLOP_NOT 0x80
#define AMLOP_FINDSETLEFTBIT 0x81
#define AMLOP_FINDSETRIGHTBIT 0x82
#define AMLOP_DEREFOF 0x83
#define AMLOP_CONCATRES 0x84
#define AMLOP_MOD 0x85
#define AMLOP_NOTIFY 0x86
#define AMLOP_SIZEOF 0x87
#define AMLOP_INDEX 0x88
#define AMLOP_MATCH 0x89
#define AMLOP_CREATEDWORDFIELD 0x8A
#define AMLOP_CREATEWORDFIELD 0x8B
#define AMLOP_CREATEBYTEFIELD 0x8C
#define AMLOP_CREATEBITFIELD 0x8D
#define AMLOP_OBJECTTYPE 0x8E
#define AMLOP_CREATEQWORDFIELD 0x8F
#define AMLOP_LAND 0x90
#define AMLOP_LOR 0x91
#define AMLOP_LNOT 0x92
#define AMLOP_LNOTEQUAL 0x9293
#define AMLOP_LLESSEQUAL 0x9294
#define AMLOP_LGREATEREQUAL 0x9295
#define AMLOP_LEQUAL 0x93
#define AMLOP_LGREATER 0x94
#define AMLOP_LLESS 0x95
#define AMLOP_TOBUFFER 0x96
#define AMLOP_TODECSTRING 0x97
#define AMLOP_TOHEXSTRING 0x98
#define AMLOP_TOINTEGER 0x99
#define AMLOP_TOSTRING 0x9C
#define AMLOP_COPYOBJECT 0x9D
#define AMLOP_MID 0x9E
#define AMLOP_CONTINUE 0x9F
#define AMLOP_IF 0xA0
#define AMLOP_ELSE 0xA1
#define AMLOP_WHILE 0xA2
#define AMLOP_NOP 0xA3
#define AMLOP_RETURN 0xA4
#define AMLOP_BREAK 0xA5
#define AMLOP_BREAKPOINT 0xCC
#define AMLOP_ONES 0xFF
#define AMLOP_INVALID (-1)

// Match()
#define AML_MATCH_TR 0
#define AML_MATCH_EQ 1
#define AML_MATCH_LE 2
#define AML_MATCH_LT 3
#define AML_MATCH_GE 4
#define AML_MATCH_GT 5

enum {
  AML_UNINIT = 0,
  AML_INTEGER,
  AML_STRING,
  AML_BUFFER,
  AML_PACKAGE,
  AML_FIELDUNIT,
  AML_DEVICE,
  AML_EVENT,
  AML_METHOD,
  AML_MUTEX,
  AML_OPREGION,
  AML_POWERRSRC,
  AML_PROCESSOR,
  AML_THERMZONE,
  AML_BUFFERFIELD,
  AML_DDBHANDLE,
  AML_DEBUGOBJ,

  AML_NAMEREF = 0x100,
  AML_OBJREF,
  AML_SCOPE,
  AML_NOTARGET,        // Zero as a target
  AML_HEXSTRING,
  AML_DECSTRING,
};

// method flags
#define AML_METHOD_ARGCOUNT(v) ((v) & 0x7)
#define AML_METHOD_SERIALIZED(v) (((v) >> 3) & 0x1)

// field flags
#define AML_FIELD_ACCESS(v) ((v) & 0xF)
#define AML_FIELD_ANYACC 0x0
#define AML_FIELD_BYTEACC 0x1
#define AML_FIELD_WORDACC 0x2
#define AML_FIELD_DWORDACC 0x3
#define AML_FIELD_QWORDACC 0x4
#define AML_FIELD_BUFFERACC 0x5
#define AML_FIELD_LOCK(v) (((v) >> 4) & 0x1)
#define AML_FIELD_UPDATE(v) (((v) >> 5) & 0x3)
#define AML_FIELD_PRESERVE 0x0
#define AML_FIELD_WRITEASONES 0x1
#define AML_FIELD_WRITEASZEROES 0x2
#define AML_FIELD_ATTR(v) ((v) >> 8)

#define AML_MAX_ARG 7
#define AML_MAX_LOCAL 8
#define AML_NAMESEG_LEN 4
#define AML_NO_TIMEOUT 0xffff

struct aml_node;
struct aml_scope;
struct aml_value;

struct aml_value {
  int type;
  int length;
  int refs;
  int stack;
  struct aml_node *node;
  union {
    int64_t integer;
    char *str;
    uint8_t *buf;
    struct aml_value **pkg;
    struct {
      uint8_t space;
      uint64_t base;
      uint32_t len;
    } opreg;
    struct {
      int flags;
      uint8_t *start;
      uint8_t *end;
      struct aml_value *(*builtin)(struct aml_scope *);
      uint8_t *base;
    } method;
    struct {
      uint16_t type;
      uint16_t flags;
      uint32_t bitpos;
      uint32_t bitlen;
      struct aml_value *data;
      struct aml_value *index;
      int arg;
    } field;
    struct {
      uint8_t id;
      uint32_t addr;
      uint8_t len;
    } proc;
    struct {
      int type;
      int index;
      struct aml_value *ref;
    } objref;
    struct {
      uint8_t level;
      uint16_t order;
    } power;
    struct {
      uint8_t *name;
    } nameref;
    struct {
      int synclevel;
      int count;
      int owner;
    } mutex;
    struct {
      int state;
    } event;
  };
};

struct aml_node {
  struct aml_node *parent;
  struct aml_node *son; // the first child
  struct aml_node *sib;
  char name[AML_NAMESEG_LEN + 1];
  int attached;
  struct aml_value *value;
  void *cookie;
};

struct aml_scope {
  uint8_t *pos;
  uint8_t *start;
  uint8_t *end;
  struct aml_node *node;
  struct aml_scope *parent;
  struct aml_value *locals;
  struct aml_value *args;
  struct aml_value *retv;
  int type;
  int depth;
  int evalid;
};

// EVALUATING

void aml_init(void);
struct aml_node *aml_root(void);
int aml_load(const uint8_t *aml, uint len, const char *rootpath);
void aml_fixup(void);

int aml_eval(struct aml_node *node, int argc, struct aml_value *argv,
             struct aml_value *res);
int aml_eval_name(struct aml_node *parent, const char *name, int argc,
                  struct aml_value *argv, struct aml_value *res);
int aml_eval_int(struct aml_node *parent, const char *name, int argc,
                 struct aml_value *argv, int64_t *ival);
int aml_set_int(struct aml_node *node, int64_t val);
int aml_eval_hid(struct aml_node *node, struct aml_value *res);

// NAMES

struct aml_node *aml_search(struct aml_node *start, const char *name);
struct aml_node *aml_search_up(struct aml_node *start, const char *name);
const char *aml_nodename(struct aml_node *node);
const char *aml_getname(const uint8_t *name);
const char *aml_eisaid(uint32_t pid);
void aml_walk(struct aml_node *node, int post,
              int (*cb)(struct aml_node *, void *), void *arg);
void aml_find(struct aml_node *node, const char *name,
              int (*cb)(struct aml_node *, void *), void *arg);

// VALUES

struct aml_value *aml_newvalue(int type, int64_t ival, const void *bval);
void aml_freevalue(struct aml_value *v);
int64_t aml_val2int(struct aml_value *v);
const char *aml_val2str(const struct aml_value *v);

void aml_notify_register(struct aml_node *node,
                         void (*fn)(struct aml_node *, int, void *), void *arg);
void aml_notify_post(struct aml_node *node, int value);
void aml_region_register(struct aml_node *dev, int space, void *cookie,
                         int (*fn)(void *, int dir, uint64_t addr, int size,
                                   uint64_t *val));
void aml_region_default(int space, void *cookie,
                        int (*fn)(void *, int dir, uint64_t addr, int size,
                                  uint64_t *val));

// RESOURCES
union acpi_resource;
int aml_parse_resource(struct aml_value *res,
                       int (*cb)(int, union acpi_resource *, void *),
                       void *arg);

void aml_lock(void);
void aml_unlock(void);
void aml_glk_enter(void);
void aml_glk_leave(void);
int aml_cold(void);
void aml_msleep(int ms);

// resource descriptors
#define SRT_ENDTAG 0x79
#define SR_IRQ 0x04
#define SR_IOPORT 0x08
#define SR_FIXEDPORT 0x09
#define LR_MEM24 0x81
#define LR_MEM32 0x85
#define LR_MEM32FIXED 0x86
#define LR_DWORD 0x87
#define LR_WORD 0x88
#define LR_EXTIRQ 0x89
#define LR_QWORD 0x8A

union acpi_resource {
  struct {
    uint8_t typecode;
    uint16_t length;
  } __packed hdr;
  struct {
    uint8_t typecode;
    uint16_t irq_mask;
    uint8_t irq_flags;
#define SR_IRQ_SHR (1 << 4)
#define SR_IRQ_POLARITY (1 << 3)
#define SR_IRQ_MODE (1 << 0)
  } __packed sr_irq;
  struct {
    uint8_t typecode;
    uint8_t flags;
    uint16_t _min;
    uint16_t _max;
    uint8_t _aln;
    uint8_t _len;
  } __packed sr_ioport;
  struct {
    uint8_t typecode;
    uint16_t _bas;
    uint8_t _len;
  } __packed sr_fioport;
  struct {
    uint8_t typecode;
    uint16_t length;
    uint8_t _info;
    uint16_t _min;
    uint16_t _max;
    uint16_t _aln;
    uint16_t _len;
  } __packed lr_m24;
  struct {
    uint8_t typecode;
    uint16_t length;
    uint8_t _info;
    uint32_t _min;
    uint32_t _max;
    uint32_t _aln;
    uint32_t _len;
  } __packed lr_m32;
  struct {
    uint8_t typecode;
    uint16_t length;
    uint8_t _info;
    uint32_t _bas;
    uint32_t _len;
  } __packed lr_m32fixed;
  struct {
    uint8_t typecode;
    uint16_t length;
    uint8_t flags;
#define LR_EXTIRQ_SHR (1 << 3)
#define LR_EXTIRQ_POLARITY (1 << 2)
#define LR_EXTIRQ_MODE (1 << 1)
    uint8_t irq_count;
    uint32_t irq[1];
  } __packed lr_extirq;
  struct {
    uint8_t typecode;
    uint16_t length;
    uint8_t type;
#define LR_TYPE_MEMORY 0
#define LR_TYPE_IO 1
#define LR_TYPE_BUS 2
    uint8_t flags;
    uint8_t tflags;
    uint16_t _gra;
    uint16_t _min;
    uint16_t _max;
    uint16_t _tra;
    uint16_t _len;
  } __packed lr_word;
  struct {
    uint8_t typecode;
    uint16_t length;
    uint8_t type;
    uint8_t flags;
    uint8_t tflags;
    uint32_t _gra;
    uint32_t _min;
    uint32_t _max;
    uint32_t _tra;
    uint32_t _len;
  } __packed lr_dword;
  struct {
    uint8_t typecode;
    uint16_t length;
    uint8_t type;
    uint8_t flags;
    uint8_t tflags;
    uint64_t _gra;
    uint64_t _min;
    uint64_t _max;
    uint64_t _tra;
    uint64_t _len;
  } __packed lr_qword;
  uint8_t pad[64];
} __packed;

#define AML_CRSTYPE(x)                                                         \
  (((x)->hdr.typecode & 0x80) ? (x)->hdr.typecode : (x)->hdr.typecode >> 3)
#define AML_CRSLEN(x)                                                          \
  (((x)->hdr.typecode & 0x80) ? 3 + (x)->hdr.length                            \
                              : 1 + ((x)->hdr.typecode & 0x7))

#endif
