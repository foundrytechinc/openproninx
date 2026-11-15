#ifndef PRONINX_UNVEIL_H
#define PRONINX_UNVEIL_H

#define UV_NONE 0
#define UV_READ 0x01
#define UV_WRITE 0x02
#define UV_CREATE 0x04
#define UV_EXEC 0x08
#define UV_MASK 0x0f
#define UV_USERSET 0x10

#define UNVEIL_MAX_DIRS 128
#define UNVEIL_MAX_NAMES 128

struct inode;
struct proc;
struct process;
struct unveil;

struct lookup_unveil {
  int want;
  struct unveil *match;
};

int unveil_active(struct process *pr);
void unveil_start(struct lookup_unveil *lu, struct inode *dp);
void unveil_component(struct lookup_unveil *lu, struct inode *dp, int dotdot);
int unveil_final(struct lookup_unveil *lu, struct inode *ip, struct inode *dp,
                 const char *name);
void unveil_copy(struct process *from, struct process *to);
void unveil_destroy(struct process *pr);

#endif
