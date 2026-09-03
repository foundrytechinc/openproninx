#include "defs.h"
#include "fs.h"
#include "file.h"
#include "inc/abi.h"
#include "inc/stat.h"
#include "param.h"
#include "spinlock.h"

#define AUTH_USERS 16
#define AUTH_PASSWORD_MAX 64
#define AUTH_WHEEL 1

struct sha256 {
  uint state[8];
  uint64_t bits;
  uchar block[64];
  uint used;
};

struct account {
  char name[USER_NAME_MAX];
  char salt[32];
  uchar hash[32];
  uint uid;
  uint gid;
  int flags;
};

static struct spinlock auth_lock;
static struct account accounts[AUTH_USERS] = {
    {"root", "root-openproninx-v1",
     {0xab, 0xfe, 0x9e, 0x34, 0xa2, 0xa4, 0x0c, 0x7f, 0xc3, 0xb1, 0xf4,
      0xbe, 0x7b, 0x58, 0xe7, 0xfe, 0x3b, 0x11, 0xa9, 0xbe, 0x10, 0xf3,
      0x91, 0x8e, 0x27, 0x1c, 0xec, 0x72, 0x16, 0x3d, 0xa3, 0x68},
     0, 0, AUTH_WHEEL},
    {"admin", "admin-openproninx-v1",
     {0xce, 0xfb, 0xa2, 0x02, 0x33, 0x5b, 0x4a, 0xe9, 0x7a, 0x39, 0xd7,
      0x99, 0xeb, 0x8c, 0xa6, 0x86, 0xed, 0x8f, 0x33, 0xdc, 0x8c, 0x0d,
      0xab, 0xdb, 0x70, 0xce, 0x24, 0x6a, 0x53, 0x89, 0x10, 0x71},
     1000, 10, AUTH_WHEEL},
    {"user", "user-openproninx-v1",
     {0x9c, 0x56, 0xf5, 0xa5, 0xd7, 0x3f, 0xcb, 0x92, 0x51, 0xda, 0x70,
      0x54, 0x5f, 0xa8, 0x04, 0x58, 0x8f, 0xb9, 0xa2, 0x12, 0x34, 0x78,
      0xab, 0x0d, 0x28, 0xff, 0xf3, 0xba, 0x88, 0x6a, 0x03, 0xcd},
     1001, 1001, 0},
};
static uint next_uid = 1002;

#define AUTH_DATABASE "/etc/passwd"

static const uint sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static uint rr(uint x, uint n) { return (x >> n) | (x << (32 - n)); }
static void sha_block(struct sha256 *s, const uchar *p) {
  uint w[64], a,b,c,d,e,f,g,h,t1,t2;
  int i;
  for (i=0;i<16;i++) w[i]=((uint)p[4*i]<<24)|((uint)p[4*i+1]<<16)|((uint)p[4*i+2]<<8)|p[4*i+3];
  for (;i<64;i++) w[i]=(rr(w[i-15],7)^rr(w[i-15],18)^(w[i-15]>>3))+w[i-16]+(rr(w[i-2],17)^rr(w[i-2],19)^(w[i-2]>>10))+w[i-7];
  a=s->state[0];b=s->state[1];c=s->state[2];d=s->state[3];e=s->state[4];f=s->state[5];g=s->state[6];h=s->state[7];
  for(i=0;i<64;i++){ t1=h+(rr(e,6)^rr(e,11)^rr(e,25))+((e&f)^((~e)&g))+sha_k[i]+w[i]; t2=(rr(a,2)^rr(a,13)^rr(a,22))+((a&b)^(a&c)^(b&c)); h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
  s->state[0]+=a;s->state[1]+=b;s->state[2]+=c;s->state[3]+=d;s->state[4]+=e;s->state[5]+=f;s->state[6]+=g;s->state[7]+=h;
}
static void sha_init(struct sha256 *s) { static const uint iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; memmove(s->state,iv,sizeof(iv));s->bits=0;s->used=0; }
static void sha_update(struct sha256 *s, const void *data, uint n) { const uchar *p=data; s->bits+=(uint64_t)n*8; while(n--){s->block[s->used++]=*p++;if(s->used==64){sha_block(s,s->block);s->used=0;}} }
static void sha_final(struct sha256 *s, uchar out[32]) { uint64_t bits=s->bits; int i; s->block[s->used++]=0x80; if(s->used>56){while(s->used<64)s->block[s->used++]=0;sha_block(s,s->block);s->used=0;} while(s->used<56)s->block[s->used++]=0; for(i=7;i>=0;i--)s->block[s->used++]=(uchar)(bits>>(i*8));sha_block(s,s->block);for(i=0;i<8;i++){out[4*i]=s->state[i]>>24;out[4*i+1]=s->state[i]>>16;out[4*i+2]=s->state[i]>>8;out[4*i+3]=s->state[i];} }

static int valid_name(const char *name) { int n=0; if(!name) return 0; while(name[n]) { if(n+1>=USER_NAME_MAX || !((name[n]>='a'&&name[n]<='z')||(name[n]>='0'&&name[n]<='9')||name[n]=='-'||name[n]=='_')) return 0; n++; } return n>0; }
static int valid_password(const char *password) { int n=0; if(!password) return 0; while(password[n]) { if(++n>AUTH_PASSWORD_MAX) return 0; } return n>=8; }
static void password_hash(const char *salt, const char *password, uchar out[32]) { struct sha256 s; sha_init(&s);sha_update(&s,salt,strlen(salt));sha_update(&s,password,strlen(password));sha_final(&s,out); }
static int hash_equal(const uchar *a, const uchar *b) { uchar different=0; int i; for(i=0;i<32;i++)different|=a[i]^b[i]; return different==0; }
static struct account *find_name(const char *name) { int i; for(i=0;i<AUTH_USERS;i++)if(accounts[i].name[0]&&strncmp(accounts[i].name,name,USER_NAME_MAX)==0)return &accounts[i]; return 0; }
static struct account *find_uid(uint uid) { int i; for(i=0;i<AUTH_USERS;i++)if(accounts[i].name[0]&&accounts[i].uid==uid)return &accounts[i]; return 0; }

static int auth_store(void) {
  struct inode *ip;
  int result;
  ip = namei(AUTH_DATABASE);
  if (ip == 0) return -1;
  begin_op();
  ilock(ip);
  result = writei(ip, (char *)accounts, 0, sizeof(accounts));
  iunlock(ip);
  iput(ip);
  end_op();
  return result == sizeof(accounts) ? 0 : -1;
}

static void auth_load(void) {
  struct inode *ip;
  int i;
  ip = namei(AUTH_DATABASE);
  if (ip == 0) return;
  ilock(ip);
  if (ip->type == T_FILE && ip->size == sizeof(accounts) &&
      readi(ip, (char *)accounts, 0, sizeof(accounts)) == sizeof(accounts)) {
    next_uid = 1002;
    for (i = 0; i < AUTH_USERS; i++)
      if (accounts[i].name[0] && accounts[i].uid >= next_uid)
        next_uid = accounts[i].uid + 1;
  }
  iunlock(ip);
  iput(ip);
}

void auth_init(void) {
  initlock(&auth_lock,"auth");
  auth_load();
  auth_store();
}
int auth_login(const char *name,const char *password,uint *uid,uint *gid) { struct account *a; uchar hash[32]; int ok=0; if(!valid_name(name)||!password||!uid||!gid)return -1; acquire(&auth_lock);a=find_name(name);if(a){password_hash(a->salt,password,hash);ok=hash_equal(hash,a->hash);if(ok){*uid=a->uid;*gid=a->gid;}}release(&auth_lock);memset(hash,0,sizeof(hash));return ok?0:-1; }
int auth_doas(uint uid,const char *password) { struct account *a; uchar hash[32]; int ok=0; if(!password)return -1; acquire(&auth_lock);a=find_uid(uid);if(a&&(a->flags&AUTH_WHEEL)){password_hash(a->salt,password,hash);ok=hash_equal(hash,a->hash);}release(&auth_lock);memset(hash,0,sizeof(hash));return ok?0:-1; }
int auth_is_wheel(uint uid) { int ok; acquire(&auth_lock); { struct account *a=find_uid(uid);ok=a&&(a->flags&AUTH_WHEEL); } release(&auth_lock);return ok; }
int auth_set_password(uint actor,const char *name,const char *password) { struct account *a; uchar old_hash[32]; if(!valid_name(name)||!valid_password(password))return -1;acquire(&auth_lock);a=find_name(name);if(!a||(actor!=0&&a->uid!=actor)){release(&auth_lock);return -1;}memmove(old_hash,a->hash,sizeof(old_hash));password_hash(a->salt,password,a->hash);if(auth_store()<0){memmove(a->hash,old_hash,sizeof(old_hash));release(&auth_lock);return -1;}release(&auth_lock);return 0; }
int auth_add_user(const char *name,const char *password,int wheel) { struct account *a=0, old; uint old_next; int i;if(!valid_name(name)||!valid_password(password))return -1;acquire(&auth_lock);if(find_name(name)){release(&auth_lock);return -1;}for(i=0;i<AUTH_USERS;i++)if(!accounts[i].name[0]){a=&accounts[i];break;}if(!a){release(&auth_lock);return -1;}old=*a;old_next=next_uid;safestrcpy(a->name,name,sizeof(a->name));a->uid=next_uid++;a->gid=a->uid;a->flags=wheel?AUTH_WHEEL:0;safestrcpy(a->salt,"openproninx-user",sizeof(a->salt));a->salt[16]=(char)('0'+(a->uid/1000)%10);a->salt[17]=(char)('0'+(a->uid/100)%10);a->salt[18]=(char)('0'+(a->uid/10)%10);a->salt[19]=(char)('0'+a->uid%10);a->salt[20]=0;password_hash(a->salt,password,a->hash);if(auth_store()<0){*a=old;next_uid=old_next;release(&auth_lock);return -1;}release(&auth_lock);return 0; }
int auth_user_info(int index,struct user_info *out) { int seen=0,i;if(!out||index<0)return -1;acquire(&auth_lock);for(i=0;i<AUTH_USERS;i++)if(accounts[i].name[0]){if(seen++==index){memset(out,0,sizeof(*out));safestrcpy(out->name,accounts[i].name,sizeof(out->name));out->uid=accounts[i].uid;out->gid=accounts[i].gid;out->wheel=(accounts[i].flags&AUTH_WHEEL)!=0;release(&auth_lock);return 0;}}release(&auth_lock);return -1; }
