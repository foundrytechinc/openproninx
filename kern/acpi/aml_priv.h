#ifndef PRONINX_AML_PRIV_H
#define PRONINX_AML_PRIV_H

#include "aml.h"

#define AML_CLEAR(v)                                                           \
  memset(&(v)->integer, 0,                                                     \
         sizeof(struct aml_value) - __builtin_offsetof(struct aml_value, integer))

struct aml_region_handler {
  struct aml_node *dev;
  int space;
  void *cookie;
  int (*fn)(void *, int, uint64_t, int, uint64_t *);
  struct aml_region_handler *next;
};
extern struct aml_region_handler aml_regions[256];
extern struct aml_region_handler *aml_device_regions;
struct aml_region_handler *aml_region_handler_of(struct aml_value *rgn);

#define AML_MAXFIELD (1 << 23)

extern int aml_intlen;
int64_t aml_intmask(int64_t v);
extern int aml_error;

void *aml_alloc(size_t size);
void aml_release(void *p);

int aml_tstbit(const uint8_t *pb, int bit);
void aml_setbit(uint8_t *pb, int bit, int val);
void aml_bufcpy(void *dst, int dpos, const void *src, int spos, int len);

void aml_addref(struct aml_value *v);
void aml_delref(struct aml_value **pv);
struct aml_value *aml_setvalue(struct aml_value *lhs, int type, int64_t ival,
                               const void *bval);
void aml_copyvalue(struct aml_value *lhs, struct aml_value *rhs);
int64_t aml_hextoint(const char *str);
struct aml_value *aml_tryconv(struct aml_value *a, int ctype, int clen);
struct aml_value *aml_convert(struct aml_scope *scope, struct aml_value *a,
                              int ctype, int clen);
uint64_t aml_evalexpr(uint64_t lhs, uint64_t rhs, int opcode);
int aml_compare(struct aml_scope *scope, struct aml_value *a1,
                struct aml_value *a2, int opcode);
struct aml_value *aml_concat(struct aml_scope *scope, struct aml_value *a1,
                             struct aml_value *a2);
struct aml_value *aml_concatres(struct aml_scope *scope, struct aml_value *a1,
                                struct aml_value *a2);
struct aml_value *aml_mid(struct aml_value *src, int index, int length);
int aml_match(struct aml_scope *scope, struct aml_value *pkg, int index,
              int op1, int v1, int op2, int v2);

struct aml_value *aml_fail(struct aml_scope *scope, const char *fmt, ...)
    __printflike(2, 3);

void aml_rwfield(struct aml_value *fld, int bpos, int blen,
                 struct aml_value *val, int mode);
void aml_createfield(struct aml_scope *scope, struct aml_value *field,
                     int opcode, struct aml_value *data, int bpos, int blen,
                     struct aml_value *index, int arg, int flags);
void aml_parsefieldlist(struct aml_scope *scope, int opcode, int flags,
                        struct aml_value *data, struct aml_value *index,
                        int arg);
void aml_regions_init(void);
int aml_mutex_acquire(struct aml_scope *scope, struct aml_value *mtx,
                      int timeout);
void aml_mutex_release(struct aml_scope *scope, struct aml_value *mtx);
int aml_event_wait(struct aml_value *evt, int timeout);
void aml_event_signal(struct aml_value *evt);
void aml_event_reset(struct aml_value *evt);
extern struct aml_value *aml_global_lock;

// the namespace
struct aml_node *aml_search_create(struct aml_node *root, const void *name,
                                   int create);
struct aml_node *aml_search_seg(struct aml_node *root, const uint8_t *seg,
                                int create);
void aml_delchildren(struct aml_node *node);
uint8_t *aml_parsename(struct aml_node *inode, uint8_t *pos,
                       struct aml_value **rval, int create);
void aml_notify(struct aml_node *node, int value);
void aml_defaults(void);
struct aml_value *aml_osi(struct aml_scope *scope);

// the evaluator
void aml_hashopcodes(void);
void aml_evalbegin(void);
extern int aml_loop_seconds;
int aml_parselength(struct aml_scope *scope);
int aml_evalid(void);
void aml_halt(void);
struct aml_scope *aml_pushscope(struct aml_scope *parent,
                                struct aml_value *range,
                                struct aml_node *node, int type);
struct aml_scope *aml_popscope(struct aml_scope *scope);
struct aml_scope *aml_findscope(struct aml_scope *scope, int type, int endscope);
struct aml_value *aml_getstack(struct aml_scope *scope, int opcode);
struct aml_value *aml_run(struct aml_scope *scope, struct aml_value *val,
                          int ret_type, int argc, struct aml_value *argv);
struct aml_value *aml_parse(struct aml_scope *scope, int ret_type,
                            const char *stype);
struct aml_value *aml_loadtable(const char *sig, const char *oemid,
                                const char *oemtable, const char *rootpath);
struct aml_scope *aml_loadregion(struct aml_scope *scope,
                                 struct aml_value *rgn, struct aml_value *ddb);
extern uint8_t *aml_base;
extern int aml_depth;

#endif
