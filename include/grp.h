#ifndef _GRP_H_
#define _GRP_H_

#include <sys/types.h>
#include <limits.h>

#define _PATH_GROUP "/etc/group"

#define _GR_BUF_LEN (1024 + 200 * sizeof(char *))

struct group {
  char *gr_name;
  char *gr_passwd;
  gid_t gr_gid;
  char **gr_mem;
};

struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);
int setgroupent(int);
int gid_from_group(const char *, gid_t *);
const char *group_from_gid(gid_t, int);

int getgrgid_r(gid_t, struct group *, char *, size_t, struct group **);
int getgrnam_r(const char *, struct group *, char *, size_t, struct group **);
int getgrouplist(const char *name, gid_t basegid, gid_t *groups, int *ngroups);
int initgroups(const char *name, gid_t basegid);

#endif
