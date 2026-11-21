// FNUPKG — FNU package manager with repositories
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sha2.h>

#include "inc/fnu.h"

extern char *__progname;

#define CONF "/etc/fnupkg.conf"
#define DB "/var/db/fnupkg"
#define INDEX_FMT DB "/repos/%d/index"
#define BASE_FMT DB "/repos/%d/base"
#define PIN_FMT DB "/repos/%d/index.pin"
#define INSTALLED DB "/installed"
#define FILES DB "/files"
#define CACHE DB "/cache"

#define META_MAX 16
#define DEPS_MAX 32
#define REPOS_MAX 32
#define LINE_MAX_LEN 2048
#define MAX_DEPTH 16

struct pkg {
  char name[64];
  char version[32];
  char arch[32];
  char desc[256];
  char depends[DEPS_MAX][64];
  int ndeps;
  char file[128];
  char sha[SHA256_DIGEST_STRING_LENGTH];
  long size;
};

struct repo {
  char base[PATH_MAX];
  char pin[SHA256_DIGEST_STRING_LENGTH]; // optional index sha256 pin ("" = none)
};

static char g_root[PATH_MAX] = ""; // -C prefix, "" = /
static struct repo g_repos[REPOS_MAX];
static int g_nrepos;

static void usage(void) {
  fprintf(stderr,
      "usage: %s [-C root] command [args]\n"
      "  update              fetch indexes from configured repositories\n"
      "  search pattern      search available packages (substring)\n"
      "  info name           show package information\n"
      "  list                list installed packages\n"
      "  install name...     install packages (or .fpkg paths)\n"
      "  remove name...      remove packages\n"
      "  upgrade [name...]   upgrade installed packages from repositories\n"
      "  files name          list files of an installed package\n"
      "  check [name]        verify checksums of installed files\n"
      "  repo list           list configured repositories\n"
      "\n"
      "exit status: 0 success, 1 failure, 2 usage error\n",
      __progname);
  exit(2);
}

// join g_root with an absolute path into dst; dst and path may alias
static void apath(char *dst, size_t n, const char *path) {
  char tmp[PATH_MAX];

  strlcpy(tmp, path, sizeof(tmp));
  if (g_root[0] == 0)
    strlcpy(dst, tmp, n);
  else
    snprintf(dst, n, "%s%s", g_root, tmp);
}

static int mkdir_p(const char *path, mode_t mode) {
  char buf[PATH_MAX];
  size_t len;
  char *p;

  strlcpy(buf, path, sizeof(buf));
  len = strlen(buf);
  while (len > 1 && buf[len - 1] == '/')
    buf[--len] = 0;
  for (p = buf + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = 0;
    if (mkdir(buf, mode) < 0 && errno != EEXIST)
      return -1;
    *p = '/';
  }
  if (mkdir(buf, mode) < 0 && errno != EEXIST)
    return -1;
  return 0;
}

static int copy_file(const char *src, const char *dst) {
  char buf[8192];
  int in, out;
  ssize_t n;

  if ((in = open(src, O_RDONLY)) < 0)
    return -1;
  if ((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
    close(in);
    return -1;
  }
  while ((n = read(in, buf, sizeof(buf))) > 0)
    if (write(out, buf, (size_t)n) != n) {
      close(in);
      close(out);
      return -1;
    }
  close(in);
  close(out);
  return n < 0 ? -1 : 0;
}

// sha256 of a file as hex; returns 0 on success
static int file_sha(const char *path, char *out, size_t outn) {
  SHA2_CTX ctx;
  unsigned char buf[8192];
  char *hex;
  ssize_t n;
  int fd;

  if (outn < SHA256_DIGEST_STRING_LENGTH)
    return -1;
  if ((fd = open(path, O_RDONLY)) < 0)
    return -1;
  SHA256Init(&ctx);
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    SHA256Update(&ctx, buf, (size_t)n);
  close(fd);
  if (n < 0)
    return -1;
  if ((hex = SHA256End(&ctx, 0)) == 0)
    return -1;
  strlcpy(out, hex, outn);
  free(hex);
  return 0;
}

static int hex_eq(const char *a, const char *b) {
  return a && b && strcasecmp(a, b) == 0;
}

// case-insensitive substring (strcasestr is not in FNU libc)
static int contains_ci(const char *hay, const char *needle) {
  size_t nlen = strlen(needle);
  if (!nlen)
    return 1;
  for (; *hay; hay++)
    if (strncasecmp(hay, needle, nlen) == 0)
      return 1;
  return 0;
}

// recursive delete without shell; does not follow symlinks
static int rm_rf(const char *path) {
  struct stat st;
  DIR *d;
  struct dirent *de;

  if (lstat(path, &st) < 0)
    return errno == ENOENT ? 0 : -1;
  if (S_ISDIR(st.st_mode)) {
    d = opendir(path);
    if (!d)
      return -1;
    while ((de = readdir(d))) {
      char child[PATH_MAX];
      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
        continue;
      snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
      rm_rf(child);
    }
    closedir(d);
    return rmdir(path);
  }
  return unlink(path);
}

// remove path and now-empty parent directories under root (best effort)
static void prune_empty_parents(const char *rel) {
  char buf[PATH_MAX], full[PATH_MAX];
  char *p;

  strlcpy(buf, rel, sizeof(buf));
  for (;;) {
    p = strrchr(buf, '/');
    if (!p || p == buf)
      break;
    *p = 0;
    snprintf(full, sizeof(full), "%s/%s", g_root[0] ? g_root : "", buf);
    if (rmdir(full) < 0)
      break;
  }
}

// ---- meta (key=value lines) ----

static void meta_reset(struct pkg *p) {
  memset(p, 0, sizeof(*p));
  strlcpy(p->arch, MACHINE_ARCH, sizeof(p->arch));
}

static int meta_load(const char *path, struct pkg *p) {
  FILE *f;
  char line[LINE_MAX_LEN];
  char *v;

  meta_reset(p);
  if ((f = fopen(path, "r")) == 0)
    return -1;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = 0;
    if (line[0] == '#' || line[0] == 0)
      continue;
    v = strchr(line, '=');
    if (!v)
      continue;
    *v++ = 0;
    if (strcmp(line, "name") == 0)
      strlcpy(p->name, v, sizeof(p->name));
    else if (strcmp(line, "version") == 0)
      strlcpy(p->version, v, sizeof(p->version));
    else if (strcmp(line, "arch") == 0)
      strlcpy(p->arch, v, sizeof(p->arch));
    else if (strcmp(line, "desc") == 0 || strcmp(line, "description") == 0)
      strlcpy(p->desc, v, sizeof(p->desc));
    else if (strcmp(line, "depends") == 0) {
      char *d, *save;
      p->ndeps = 0;
      for (d = strtok_r(v, ",", &save); d && p->ndeps < DEPS_MAX;
           d = strtok_r(0, ",", &save)) {
        while (*d == ' ')
          d++;
        if (*d && strcmp(d, "-") != 0)
          strlcpy(p->depends[p->ndeps++], d, sizeof(p->depends[0]));
      }
    }
  }
  fclose(f);
  return p->name[0] ? 0 : -1;
}

// ---- index: name|ver|arch|file|sha256|size|depends|desc ----

static int index_parse_line(char *line, struct pkg *p) {
  char *f[8];
  int i;
  char *s = line;

  for (i = 0; i < 8; i++) {
    f[i] = s;
    if (i == 7)
      break;
    s = strchr(s, '|');
    if (!s)
      return -1;
    *s++ = 0;
  }
  meta_reset(p);
  strlcpy(p->name, f[0], sizeof(p->name));
  strlcpy(p->version, f[1], sizeof(p->version));
  strlcpy(p->arch, f[2], sizeof(p->arch));
  strlcpy(p->file, f[3], sizeof(p->file));
  strlcpy(p->sha, f[4], sizeof(p->sha));
  p->size = strtol(f[5], 0, 10);
  {
    char *d, *save;
    p->ndeps = 0;
    for (d = strtok_r(f[6], ",", &save); d && p->ndeps < DEPS_MAX;
         d = strtok_r(0, ",", &save)) {
      while (*d == ' ')
        d++;
      if (*d && strcmp(d, "-") != 0)
        strlcpy(p->depends[p->ndeps++], d, sizeof(p->depends[0]));
    }
  }
  strlcpy(p->desc, f[7], sizeof(p->desc));
  return 0;
}

// search all cached indexes; first match by name wins; returns repo or -1
static FILE *open_index(int i);

static int index_find2(const char *name, struct pkg *out, int *repo_out) {
  char line[LINE_MAX_LEN];
  FILE *f;
  int i;

  for (i = 0; i < g_nrepos; i++) {
    if ((f = open_index(i)) == 0)
      continue;
    while (fgets(line, sizeof(line), f)) {
      line[strcspn(line, "\n")] = 0;
      if (line[0] == '#' || line[0] == 0)
        continue;
      if (index_parse_line(line, out) == 0 && strcmp(out->name, name) == 0) {
        fclose(f);
        if (repo_out)
          *repo_out = i;
        return 0;
      }
    }
    fclose(f);
  }
  return -1;
}

// ---- fetch: local path or http:// ----

static int http_get(const char *host, int port, const char *path, const char *dst,
                    long expect_size) {
  struct sockaddr_in to;
  char req[1024], buf[8192];
  char *acc = 0, *body, *hdr_end;
  size_t accn = 0, accap = 0, hdrlen, body_len;
  int s, n, code, out;
  long content_len = -1;
  uint8_t addr[4];
  char *cl;

  if (fnu_resolve(host, addr, 5000) < 0)
    return -1;
  memset(&to, 0, sizeof(to));
  to.sin_len = sizeof(to);
  to.sin_family = AF_INET;
  to.sin_port = htons((uint16_t)port);
  memcpy(&to.sin_addr.s_addr, addr, 4);
  if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    return -1;
  if (connect(s, (struct sockaddr *)&to, sizeof(to)) < 0) {
    close(s);
    return -1;
  }
  snprintf(req, sizeof(req),
      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: fnupkg\r\n"
      "Connection: close\r\n\r\n",
      path, host);
  if (send(s, req, strlen(req), 0) != (ssize_t)strlen(req)) {
    close(s);
    return -1;
  }
  while ((n = (int)recv(s, buf, sizeof(buf), 0)) > 0) {
    if (accn + (size_t)n + 1 > accap) {
      size_t ncap = (accn + (size_t)n + 1) * 2;
      char *nacc = realloc(acc, ncap);
      if (!nacc) {
        free(acc);
        close(s);
        errno = ENOMEM;
        return -1;
      }
      acc = nacc;
      accap = ncap;
    }
    memcpy(acc + accn, buf, (size_t)n);
    accn += (size_t)n;
  }
  close(s);
  if (!acc || accn == 0) {
    free(acc);
    return -1;
  }
  acc[accn] = 0;
  hdr_end = strstr(acc, "\r\n\r\n");
  if (!hdr_end) {
    free(acc);
    errno = EBADMSG;
    return -1;
  }
  body = hdr_end + 4;
  hdrlen = (size_t)(body - acc);
  code = atoi(acc + 9);
  if (code != 200) {
    free(acc);
    errno = ENOENT;
    return -1;
  }
  // optional Content-Length
  {
    char *line_end = strstr(acc, "\r\n");
    cl = 0;
    if (line_end && line_end < hdr_end) {
      // scan headers for Content-Length
      char *h = line_end + 2;
      while (h < hdr_end) {
        char *e = strstr(h, "\r\n");
        size_t hl;
        if (!e)
          break;
        hl = (size_t)(e - h);
        if (hl > 15 && strncasecmp(h, "Content-Length:", 15) == 0) {
          content_len = strtol(h + 15, 0, 10);
          break;
        }
        h = e + 2;
      }
    }
    (void)cl;
  }
  body_len = accn - hdrlen;
  if (content_len >= 0 && (size_t)content_len != body_len) {
    free(acc);
    errno = EBADMSG;
    return -1;
  }
  if (expect_size > 0 && (long)body_len != expect_size) {
    free(acc);
    errno = EMSGSIZE;
    return -1;
  }
  if ((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
    free(acc);
    return -1;
  }
  if (body_len > 0 && write(out, body, body_len) != (ssize_t)body_len) {
    close(out);
    free(acc);
    return -1;
  }
  close(out);
  free(acc);
  return 0;
}

// fetch rel from repo base into dst; base is path or http://host[:port]/prefix
static int fetch_rel(const char *base, const char *rel, const char *dst,
                     long expect_size) {
  char url[PATH_MAX];

  if (strncmp(base, "http://", 7) == 0) {
    char host[256], path[PATH_MAX];
    int port = 80;
    const char *p = base + 7;
    const char *slash;

    slash = strchr(p, '/');
    if (slash) {
      size_t hl = (size_t)(slash - p);
      if (hl >= sizeof(host))
        return -1;
      memcpy(host, p, hl);
      host[hl] = 0;
      strlcpy(path, slash, sizeof(path));
    } else {
      strlcpy(host, p, sizeof(host));
      path[0] = 0;
    }
    {
      char *c = strchr(host, ':');
      if (c) {
        *c = 0;
        port = atoi(c + 1);
        if (port <= 0)
          port = 80;
      }
    }
    if (path[0] && path[strlen(path) - 1] == '/')
      path[strlen(path) - 1] = 0;
    if (rel[0] == '/')
      snprintf(url, sizeof(url), "%s%s", path, rel);
    else
      snprintf(url, sizeof(url), "%s/%s", path, rel);
    return http_get(host, port, url, dst, expect_size);
  }
  // local directory
  if (base[0] && base[strlen(base) - 1] == '/')
    snprintf(url, sizeof(url), "%s%s", base, rel);
  else
    snprintf(url, sizeof(url), "%s/%s", base, rel);
  if (expect_size > 0) {
    struct stat st;
    if (stat(url, &st) == 0 && (long)st.st_size != expect_size) {
      errno = EMSGSIZE;
      return -1;
    }
  }
  return copy_file(url, dst);
}

// ---- config ----
// line: <path|file://path|http://host[/prefix]> [sha256=<hex>]
// sha256= is optional for every transport; when present it is verified.
// file:// and plain paths are local — pin not expected.

static int base_is_http(const char *b) {
  return strncmp(b, "http://", 7) == 0;
}

// file:///abs/path → /abs/path; file://localhost/abs → /abs; else unchanged
static void normalize_base(char *base, size_t n) {
  char tmp[PATH_MAX];
  const char *p;

  if (strncmp(base, "file://", 7) != 0)
    return;
  p = base + 7;
  if (strncmp(p, "localhost", 9) == 0)
    p += 9;
  if (*p != '/')
    return; /* file:relative — leave as-is */
  strlcpy(tmp, p, sizeof(tmp));
  strlcpy(base, tmp, n);
}

static int pin_is_hex64(const char *pin) {
  size_t i, n = strlen(pin);
  if (n != 64)
    return 0;
  for (i = 0; i < n; i++) {
    char c = pin[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')))
      return 0;
  }
  return 1;
}

static void load_repos(void) {
  char path[PATH_MAX], line[LINE_MAX_LEN];
  FILE *f;
  int lineno = 0;

  apath(path, sizeof(path), CONF);
  if ((f = fopen(path, "r")) == 0)
    return;
  g_nrepos = 0;
  while (fgets(line, sizeof(line), f) && g_nrepos < REPOS_MAX) {
    char *sp, *pin;
    line[strcspn(line, "\n")] = 0;
    lineno++;
    if (line[0] == '#' || line[0] == 0 || line[0] == ' ' || line[0] == '\t')
      continue;
    pin = strstr(line, " sha256=");
    if (pin) {
      *pin = 0;
      pin += 8;
      strlcpy(g_repos[g_nrepos].pin, pin, sizeof(g_repos[0].pin));
      sp = g_repos[g_nrepos].pin + strlen(g_repos[g_nrepos].pin);
      while (sp > g_repos[g_nrepos].pin && (sp[-1] == ' ' || sp[-1] == '\t'))
        *--sp = 0;
      if (!pin_is_hex64(g_repos[g_nrepos].pin)) {
        warnx("%s:%d: sha256= must be 64 hex digits (ignored)", CONF, lineno);
        g_repos[g_nrepos].pin[0] = 0;
      }
    } else {
      g_repos[g_nrepos].pin[0] = 0;
    }
    strlcpy(g_repos[g_nrepos].base, line, sizeof(g_repos[0].base));
    normalize_base(g_repos[g_nrepos].base, sizeof(g_repos[0].base));
    /* pin optional on http:// and file://; never required for local paths */
    g_nrepos++;
  }
  fclose(f);
}

// Open index for repo i without requiring a prior `fnupkg update`:
//  1) cache  /var/db/fnupkg/repos/<i>/index
//  2) local  <base>/index          (image ships this)
//  3) http:// fetch into cache on first use
static FILE *open_index(int i) {
  char path[PATH_MAX], tmp[PATH_MAX], direct[PATH_MAX];
  FILE *f;

  strlcpy(tmp, INDEX_FMT, sizeof(tmp));
  snprintf(path, sizeof(path), tmp, i);
  apath(path, sizeof(path), path);
  f = fopen(path, "r");
  if (f)
    return f;

  if (!base_is_http(g_repos[i].base)) {
    if (g_repos[i].base[0] &&
        g_repos[i].base[strlen(g_repos[i].base) - 1] == '/')
      snprintf(direct, sizeof(direct), "%sindex", g_repos[i].base);
    else
      snprintf(direct, sizeof(direct), "%s/index", g_repos[i].base);
    apath(direct, sizeof(direct), direct);
    return fopen(direct, "r");
  }

  /* remote: fetch once into the cache, honour pin if set */
  {
    char dst[PATH_MAX], dir[PATH_MAX], got[SHA256_DIGEST_STRING_LENGTH];

    strlcpy(tmp, DB "/repos/%d", sizeof(tmp));
    snprintf(dir, sizeof(dir), tmp, i);
    apath(dir, sizeof(dir), dir);
    mkdir_p(dir, 0755);

    strlcpy(tmp, INDEX_FMT, sizeof(tmp));
    snprintf(dst, sizeof(dst), tmp, i);
    apath(dst, sizeof(dst), dst);
    if (fetch_rel(g_repos[i].base, "index", dst, -1) < 0)
      return NULL;
    if (g_repos[i].pin[0]) {
      if (file_sha(dst, got, sizeof(got)) < 0 || !hex_eq(got, g_repos[i].pin)) {
        warnx("repo %d (%s): index sha256 mismatch (auto-fetch)", i,
            g_repos[i].base);
        unlink(dst);
        return NULL;
      }
    }
    return fopen(dst, "r");
  }
}

// ---- path safety ----

// strip ./ ; reject empty, absolute, and any .. component
static int safe_rel(const char *in, char *out, size_t outn) {
  const char *p = in;
  size_t len;

  while (p[0] == '.' && p[1] == '/')
    p += 2;
  if (p[0] == 0 || p[0] == '/')
    return -1;
  len = strlen(p);
  if (len == 0 || len >= outn)
    return -1;
  // reject ".." as a whole component anywhere, including trailing
  {
    const char *s = p;
    while (*s) {
      const char *e = strchr(s, '/');
      size_t clen = e ? (size_t)(e - s) : strlen(s);
      if (clen == 2 && s[0] == '.' && s[1] == '.')
        return -1;
      if (!e)
        break;
      s = e + 1;
    }
  }
  strlcpy(out, p, outn);
  return 0;
}

// symlink target: relative only, and must not escape via ..
static int safe_link(const char *target) {
  if (!target || !target[0])
    return -1;
  if (target[0] == '/')
    return -1;
  return safe_rel(target, (char[PATH_MAX]){0}, PATH_MAX);
}

// ---- tar (ustar) extract into staging dir ----

struct tar_hdr {
  char name[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char size[12];
  char mtime[12];
  char chksum[8];
  char type;
  char linkname[100];
  char magic[6];
  char version[2];
  char uname[32];
  char gname[32];
  char devmajor[8];
  char devminor[8];
  char prefix[155];
  char pad[12];
};

static long oct2(const char *s, int n) {
  long v = 0;
  int i;
  for (i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++)
    v = v * 8 + (s[i] - '0');
  return v;
}

static int tar_checksum_ok(const struct tar_hdr *h) {
  const unsigned char *p = (const unsigned char *)h;
  long sum = 0;
  int i;
  for (i = 0; i < 512; i++)
    sum += (i >= 148 && i < 156) ? ' ' : p[i];
  return sum == oct2(h->chksum, 8);
}

static int extract_parent_mkdir(const char *dst) {
  char tmp[PATH_MAX];
  char *slash;

  strlcpy(tmp, dst, sizeof(tmp));
  slash = strrchr(tmp, '/');
  if (!slash)
    return 0;
  *slash = 0;
  return mkdir_p(tmp, 0755);
}

// extract ustar from fpkg_path into stage dir; returns 0
static int tar_extract(const char *fpkg_path, const char *stage) {
  FILE *f;
  struct tar_hdr h;
  char rel[PATH_MAX], dst[PATH_MAX], buf[8192];
  long size, rem;
  int zeros = 0;

  if ((f = fopen(fpkg_path, "rb")) == 0)
    return -1;
  while (fread(&h, 1, 512, f) == 512) {
    if (h.name[0] == 0) {
      if (++zeros >= 2)
        break;
      continue;
    }
    zeros = 0;
    if (!tar_checksum_ok(&h)) {
      fclose(f);
      errno = EBADMSG;
      return -1;
    }
    size = oct2(h.size, 12);
    if (size < 0) {
      fclose(f);
      errno = EBADMSG;
      return -1;
    }
    if (h.prefix[0]) {
      char full[256];
      snprintf(full, sizeof(full), "%.*s/%.*s",
          (int)strcspn(h.prefix, "\0"), h.prefix,
          (int)strcspn(h.name, "\0"), h.name);
      if (safe_rel(full, rel, sizeof(rel)) < 0) {
        fseek(f, (size + 511) & ~511L, SEEK_CUR);
        continue;
      }
    } else if (safe_rel(h.name, rel, sizeof(rel)) < 0) {
      fseek(f, (size + 511) & ~511L, SEEK_CUR);
      continue;
    }
    snprintf(dst, sizeof(dst), "%s/%s", stage, rel);

    if (h.type == '5' || (h.type == 0 && rel[strlen(rel) - 1] == '/')) {
      mkdir_p(dst, 0755);
      continue;
    }
    if (h.type == '2') { // symlink
      char linkt[100];
      size_t ll = strcspn(h.linkname, "\0");
      if (ll >= sizeof(linkt))
        ll = sizeof(linkt) - 1;
      memcpy(linkt, h.linkname, ll);
      linkt[ll] = 0;
      if (safe_link(linkt) < 0) {
        fseek(f, (size + 511) & ~511L, SEEK_CUR);
        continue;
      }
      extract_parent_mkdir(dst);
      unlink(dst);
      symlink(linkt, dst);
      continue;
    }
    if (h.type == '1') { // hardlink
      char relsrc[PATH_MAX], src[PATH_MAX];
      if (safe_rel(h.linkname, relsrc, sizeof(relsrc)) < 0) {
        fseek(f, (size + 511) & ~511L, SEEK_CUR);
        continue;
      }
      snprintf(src, sizeof(src), "%s/%s", stage, relsrc);
      extract_parent_mkdir(dst);
      unlink(dst);
      if (link(src, dst) < 0) {
        if (copy_file(src, dst) < 0) {
          fclose(f);
          return -1;
        }
      }
      continue;
    }
    if (h.type != 0 && h.type != '0' && h.type != '\0') {
      fseek(f, (size + 511) & ~511L, SEEK_CUR);
      continue;
    }
    extract_parent_mkdir(dst);
    {
      int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
          (mode_t)(oct2(h.mode, 8) & 07777));
      if (out < 0) {
        fclose(f);
        return -1;
      }
      rem = size;
      while (rem > 0) {
        size_t want = rem > (long)sizeof(buf) ? sizeof(buf) : (size_t)rem;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) {
          close(out);
          fclose(f);
          return -1;
        }
        if (write(out, buf, got) != (ssize_t)got) {
          close(out);
          fclose(f);
          return -1;
        }
        rem -= (long)got;
      }
      close(out);
    }
    {
      long pad = ((size + 511) & ~511L) - size;
      if (pad)
        fseek(f, pad, SEEK_CUR);
    }
  }
  fclose(f);
  return 0;
}

// walk stage dir; write "sha  path" lines to list and/or install into -C root.
// skips top-level +* metadata entries. records installed paths for rollback.
struct walk_ctx {
  FILE *list;
  int install;
  FILE *journal; // paths written under root, for rollback
};

static int walk_install(const char *stage, const char *relbase, struct walk_ctx *ctx) {
  char dirpath[PATH_MAX], full[PATH_MAX];
  DIR *d;
  struct dirent *de;

  snprintf(dirpath, sizeof(dirpath), "%s/%s", stage, relbase[0] ? relbase : ".");
  if ((d = opendir(dirpath)) == 0)
    return 0;
  while ((de = readdir(d))) {
    char rel[PATH_MAX], dst[PATH_MAX];
    struct stat st;

    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    if (relbase[0] == 0 && de->d_name[0] == '+')
      continue;
    if (relbase[0])
      snprintf(rel, sizeof(rel), "%s/%s", relbase, de->d_name);
    else
      strlcpy(rel, de->d_name, sizeof(rel));
    snprintf(full, sizeof(full), "%s/%s", stage, rel);
    snprintf(dst, sizeof(dst), "%s/%s", g_root[0] ? g_root : "", rel);
    if (lstat(full, &st) < 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      if (ctx->install)
        mkdir_p(dst, 0755);
      if (walk_install(stage, rel, ctx) < 0) {
        closedir(d);
        return -1;
      }
    } else if (S_ISREG(st.st_mode)) {
      char sha[SHA256_DIGEST_STRING_LENGTH];
      if (ctx->list) {
        if (file_sha(full, sha, sizeof(sha)) < 0) {
          closedir(d);
          return -1;
        }
        fprintf(ctx->list, "%s  %s\n", sha, rel);
      }
      if (ctx->install) {
        if (ctx->journal)
          fprintf(ctx->journal, "%s\n", rel);
        extract_parent_mkdir(dst);
        // write via temp name then rename for a bit of atomicity
        {
          char tmpdst[PATH_MAX];
          snprintf(tmpdst, sizeof(tmpdst), "%s.fnupkg-tmp", dst);
          unlink(tmpdst);
          if (copy_file(full, tmpdst) < 0) {
            closedir(d);
            return -1;
          }
          chmod(tmpdst, st.st_mode & 07777);
          if (rename(tmpdst, dst) < 0) {
            closedir(d);
            return -1;
          }
        }
      }
    } else if (S_ISLNK(st.st_mode)) {
      char target[PATH_MAX];
      ssize_t n = readlink(full, target, sizeof(target) - 1);
      if (n < 0)
        continue;
      target[n] = 0;
      if (safe_link(target) < 0)
        continue;
      if (ctx->list)
        fprintf(ctx->list, "-  %s -> %s\n", rel, target);
      if (ctx->install) {
        if (ctx->journal)
          fprintf(ctx->journal, "%s\n", rel);
        extract_parent_mkdir(dst);
        unlink(dst);
        symlink(target, dst);
      }
    }
  }
  closedir(d);
  return 0;
}

static int is_installed(const char *name) {
  char path[PATH_MAX], tmp[PATH_MAX];
  struct stat st;

  strlcpy(tmp, INSTALLED "/%s", sizeof(tmp));
  snprintf(path, sizeof(path), tmp, name);
  apath(path, sizeof(path), path);
  return stat(path, &st) == 0;
}

// run script with /bin/sh via fork/exec — no shell metacharacter hazards
static int run_script(const char *stage, const char *script) {
  char path[PATH_MAX];
  struct stat st;
  pid_t pid;
  int status;

  snprintf(path, sizeof(path), "%s/%s", stage, script);
  if (stat(path, &st) < 0)
    return 0;
  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
      dup2(fd, 0);
      if (fd != 0)
        close(fd);
    }
    execl("/bin/sh", "sh", path, (char *)0);
    _exit(127);
  }
  if (waitpid(pid, &status, 0) < 0)
    return -1;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    warnx("%s failed", script);
    return -1;
  }
  return 0;
}

// delete files listed in an open files-db (or journal) — rollback helper
static void rollback_journal(const char *journal_path) {
  FILE *f;
  char line[LINE_MAX_LEN];

  if ((f = fopen(journal_path, "r")) == 0)
    return;
  while (fgets(line, sizeof(line), f)) {
    char full[PATH_MAX];
    line[strcspn(line, "\n")] = 0;
    if (!line[0])
      continue;
    snprintf(full, sizeof(full), "%s/%s", g_root[0] ? g_root : "", line);
    unlink(full);
    prune_empty_parents(line);
  }
  fclose(f);
  unlink(journal_path);
}

// ---- commands ----

static int cmd_update(void) {
  char dst[PATH_MAX], tmp[PATH_MAX], idx[PATH_MAX];
  int i, ok = 0;

  if (g_nrepos == 0)
    errx(1, "no repositories in %s", CONF);
  for (i = 0; i < g_nrepos; i++) {
    char got[SHA256_DIGEST_STRING_LENGTH];
    struct stat st;

    snprintf(tmp, sizeof(tmp), DB "/repos/%d", i);
    apath(dst, sizeof(dst), tmp);
    if (mkdir_p(dst, 0755) < 0)
      err(1, "%s", dst);
    snprintf(tmp, sizeof(tmp), INDEX_FMT, i);
    apath(idx, sizeof(idx), tmp);
    if (fetch_rel(g_repos[i].base, "index", idx, -1) < 0) {
      warnx("repo %d (%s): cannot fetch index", i, g_repos[i].base);
      continue;
    }
    if (file_sha(idx, got, sizeof(got)) < 0) {
      warnx("repo %d: cannot hash index", i);
      continue;
    }
    /* pin optional; verify only when configured (http and file both allowed) */
    if (g_repos[i].pin[0] && !hex_eq(got, g_repos[i].pin)) {
      warnx("repo %d (%s): index sha256 mismatch\n  got  %s\n  want %s",
          i, g_repos[i].base, got, g_repos[i].pin);
      unlink(idx);
      continue;
    }
    snprintf(tmp, sizeof(tmp), PIN_FMT, i);
    apath(dst, sizeof(dst), tmp);
    {
      FILE *f = fopen(dst, "w");
      if (f) {
        fprintf(f, "%s\n", got);
        fclose(f);
      }
    }
    snprintf(tmp, sizeof(tmp), BASE_FMT, i);
    apath(dst, sizeof(dst), tmp);
    {
      FILE *f = fopen(dst, "w");
      if (f) {
        fprintf(f, "%s\n", g_repos[i].base);
        fclose(f);
      }
    }
    (void)st;
    printf("updated repo %d (%s) index=%s%s\n", i, g_repos[i].base, got,
        g_repos[i].pin[0] ? " (pin ok)" : " (no pin)");
    ok++;
  }
  return ok > 0 ? 0 : 1;
}

static int cmd_list(void) {
  char path[PATH_MAX], tmp[PATH_MAX];
  DIR *d;
  struct dirent *de;
  char **names = 0;
  size_t n = 0, cap = 0;
  size_t i;

  strlcpy(tmp, INSTALLED, sizeof(tmp));
  apath(path, sizeof(path), tmp);
  if ((d = opendir(path)) == 0)
    return 0;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.')
      continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 32;
      names = realloc(names, cap * sizeof(*names));
      if (!names)
        err(1, "realloc");
    }
    names[n++] = strdup(de->d_name);
  }
  closedir(d);
  // sort by name
  for (i = 0; i + 1 < n; i++) {
    size_t j;
    for (j = i + 1; j < n; j++)
      if (strcmp(names[j], names[i]) < 0) {
        char *t = names[i];
        names[i] = names[j];
        names[j] = t;
      }
  }
  for (i = 0; i < n; i++) {
    struct pkg p;
    char mpath[PATH_MAX];
    snprintf(mpath, sizeof(mpath), "%s/%s", path, names[i]);
    if (meta_load(mpath, &p) == 0)
      printf("%s-%s  %s\n", p.name, p.version, p.desc);
    free(names[i]);
  }
  free(names);
  return 0;
}

static int cmd_search(const char *pat) {
  char line[LINE_MAX_LEN];
  FILE *f;
  int i, any = 0;

  for (i = 0; i < g_nrepos; i++) {
    if ((f = open_index(i)) == 0)
      continue;
    while (fgets(line, sizeof(line), f)) {
      struct pkg p;
      line[strcspn(line, "\n")] = 0;
      if (line[0] == '#' || line[0] == 0)
        continue;
      if (index_parse_line(line, &p) < 0)
        continue;
      if (contains_ci(p.name, pat) || contains_ci(p.desc, pat)) {
        printf("%s-%s  %s\n", p.name, p.version, p.desc);
        any = 1;
      }
    }
    fclose(f);
  }
  if (!any)
    warnx("no matches for %s", pat);
  return 0; // search itself succeeded
}

static void print_deps(const struct pkg *p) {
  int k;
  if (!p->ndeps) {
    printf("depends: (none)\n");
    return;
  }
  printf("depends:");
  for (k = 0; k < p->ndeps; k++)
    printf(" %s", p->depends[k]);
  printf("\n");
}

static int cmd_info(const char *name) {
  struct pkg p;
  int repo;
  char path[PATH_MAX], tmp[PATH_MAX];

  strlcpy(tmp, INSTALLED "/%s", sizeof(tmp));
  snprintf(path, sizeof(path), tmp, name);
  apath(path, sizeof(path), path);
  if (meta_load(path, &p) == 0) {
    printf("name: %s\nversion: %s\narch: %s\ndesc: %s\nstate: installed\n",
        p.name, p.version, p.arch, p.desc);
    print_deps(&p);
    return 0;
  }
  load_repos();
  if (index_find2(name, &p, &repo) < 0)
    errx(1, "%s: not found", name);
  printf("name: %s\nversion: %s\narch: %s\ndesc: %s\nfile: %s\nsha256: %s\n",
      p.name, p.version, p.arch, p.desc, p.file, p.sha);
  print_deps(&p);
  printf("state: available (repo %d)\n", repo);
  return 0;
}

static int cmd_files(const char *name) {
  char path[PATH_MAX], tmp[PATH_MAX];
  FILE *f;
  char line[LINE_MAX_LEN];

  strlcpy(tmp, FILES "/%s", sizeof(tmp));
  snprintf(path, sizeof(path), tmp, name);
  apath(path, sizeof(path), path);
  if ((f = fopen(path, "r")) == 0)
    err(1, "%s", path);
  while (fgets(line, sizeof(line), f))
    fputs(line, stdout);
  fclose(f);
  return 0;
}

static int cmd_check(const char *only) {
  char path[PATH_MAX], tmp[PATH_MAX];
  DIR *d;
  struct dirent *de;
  int bad = 0, nfiles = 0, nok = 0;

  strlcpy(tmp, FILES, sizeof(tmp));
  apath(path, sizeof(path), tmp);
  if ((d = opendir(path)) == 0)
    return 0;
  while ((de = readdir(d))) {
    FILE *f;
    char line[LINE_MAX_LEN];
    char fpath[PATH_MAX];

    if (de->d_name[0] == '.' || (only && strcmp(de->d_name, only) != 0))
      continue;
    snprintf(fpath, sizeof(fpath), "%s/%s", path, de->d_name);
    if ((f = fopen(fpath, "r")) == 0)
      continue;
    while (fgets(line, sizeof(line), f)) {
      char sha[SHA256_DIGEST_STRING_LENGTH], want[SHA256_DIGEST_STRING_LENGTH];
      char *p, *rel;
      struct stat st;

      line[strcspn(line, "\n")] = 0;
      if (line[0] == '-' || line[0] == 0)
        continue; // symlink marker: existence only
      p = strchr(line, ' ');
      if (!p)
        continue;
      *p = 0;
      strlcpy(want, line, sizeof(want));
      rel = p + 1;
      while (*rel == ' ')
        rel++;
      if (*rel == 0)
        continue;
      {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", g_root[0] ? g_root : "", rel);
        if (lstat(full, &st) < 0) {
          warnx("%s: missing", rel);
          bad = 1;
          continue;
        }
        if (file_sha(full, sha, sizeof(sha)) < 0) {
          warnx("%s: cannot hash", rel);
          bad = 1;
          continue;
        }
        if (!hex_eq(sha, want)) {
          warnx("%s: checksum mismatch", rel);
          bad = 1;
        } else
          nok++;
        nfiles++;
      }
    }
    fclose(f);
  }
  closedir(d);
  if (bad)
    warnx("check: %d/%d files bad", bad, nfiles);
  else
    printf("all checksums ok (%d files)\n", nok);
  return bad;
}

// true if any other installed package depends on `name`
static int reverse_deps(const char *name, char *out, size_t outn) {
  char path[PATH_MAX], tmp[PATH_MAX];
  DIR *d;
  struct dirent *de;
  int found = 0;

  strlcpy(tmp, INSTALLED, sizeof(tmp));
  apath(path, sizeof(path), tmp);
  if ((d = opendir(path)) == 0)
    return 0;
  while ((de = readdir(d))) {
    struct pkg p;
    char mpath[PATH_MAX];
    int i;
    if (de->d_name[0] == '.' || strcmp(de->d_name, name) == 0)
      continue;
    snprintf(mpath, sizeof(mpath), "%s/%s", path, de->d_name);
    if (meta_load(mpath, &p) < 0)
      continue;
    for (i = 0; i < p.ndeps; i++) {
      if (strcmp(p.depends[i], name) == 0) {
        if (out && outn)
          strlcpy(out, p.name, outn);
        found = 1;
        break;
      }
    }
    if (found)
      break;
  }
  closedir(d);
  return found;
}

static int install_one(const char *name, int depth);

static void stage_cleanup(const char *stage) { rm_rf(stage); }

static int install_fpkg(const char *fpkg, int depth) {
  char stage[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
  struct pkg meta;
  char listpath[PATH_MAX], jpath[PATH_MAX];
  FILE *list, *journal;
  int k;

  (void)depth;
  snprintf(tmp, sizeof(tmp), "%s/stage.XXXXXX", DB);
  apath(stage, sizeof(stage), tmp);
  {
    char *slash = strrchr(stage, '/');
    if (slash) {
      char save = *slash;
      *slash = 0;
      mkdir_p(stage, 0755);
      *slash = save;
    }
  }
  if (mkdtemp(stage) == 0)
    err(1, "mkdtemp");

  if (tar_extract(fpkg, stage) < 0) {
    warn("%s: extract", fpkg);
    goto fail;
  }
  snprintf(path, sizeof(path), "%s/+META", stage);
  if (meta_load(path, &meta) < 0) {
    warnx("%s: bad +META", fpkg);
    goto fail;
  }
  if (strcmp(meta.arch, MACHINE_ARCH) != 0 && strcmp(meta.arch, "any") != 0)
    warnx("%s: built for %s, running on %s", meta.name, meta.arch, MACHINE_ARCH);

  for (k = 0; k < meta.ndeps; k++) {
    if (!is_installed(meta.depends[k]) &&
        install_one(meta.depends[k], depth + 1) < 0) {
      warnx("%s: dependency %s failed", meta.name, meta.depends[k]);
      goto fail;
    }
  }
  if (is_installed(meta.name)) {
    // idempotent success for plain install; caller may remove first to force
    warnx("%s: already installed (use upgrade or remove first)", meta.name);
    stage_cleanup(stage);
    return 0;
  }

  if (run_script(stage, "+PREINSTALL") < 0)
    goto fail;

  // keep +PREREMOVE for later removal
  {
    char src[PATH_MAX], dst[PATH_MAX], dir[PATH_MAX];
    struct stat st;
    snprintf(src, sizeof(src), "%s/+PREREMOVE", stage);
    if (stat(src, &st) == 0) {
      snprintf(dir, sizeof(dir), "%s/scripts", DB);
      apath(dst, sizeof(dst), dir);
      mkdir_p(dst, 0755);
      snprintf(dst, sizeof(dst), "%s/scripts/%s", DB, meta.name);
      apath(dst, sizeof(dst), dst);
      copy_file(src, dst);
    }
  }

  snprintf(tmp, sizeof(tmp), FILES "/%s", meta.name);
  apath(listpath, sizeof(listpath), tmp);
  snprintf(tmp, sizeof(tmp), "%s/journal.%s", DB, meta.name);
  apath(jpath, sizeof(jpath), tmp);
  {
    char *slash;
    strlcpy(tmp, listpath, sizeof(tmp));
    slash = strrchr(tmp, '/');
    if (slash) {
      char save = *slash;
      *slash = 0;
      mkdir_p(tmp, 0755);
      *slash = save;
    }
  }
  if ((list = fopen(listpath, "w")) == 0) {
    warn("%s", listpath);
    goto fail;
  }
  if ((journal = fopen(jpath, "w")) == 0) {
    fclose(list);
    warn("%s", jpath);
    goto fail;
  }
  {
    struct walk_ctx ctx;
    ctx.list = list;
    ctx.install = 1;
    ctx.journal = journal;
    if (walk_install(stage, "", &ctx) < 0) {
      fclose(list);
      fclose(journal);
      warnx("%s: install failed, rolling back", meta.name);
      rollback_journal(jpath);
      unlink(listpath);
      stage_cleanup(stage);
      return -1;
    }
  }
  fclose(list);
  fclose(journal);

  // installed meta — only after files are in place
  snprintf(tmp, sizeof(tmp), INSTALLED "/%s", meta.name);
  apath(path, sizeof(path), tmp);
  {
    char *slash = strrchr(path, '/');
    if (slash) {
      char save = *slash;
      *slash = 0;
      mkdir_p(path, 0755);
      *slash = save;
    }
  }
  {
    char msrc[PATH_MAX];
    snprintf(msrc, sizeof(msrc), "%s/+META", stage);
    if (copy_file(msrc, path) < 0) {
      FILE *o = fopen(path, "w");
      if (o) {
        fprintf(o, "name=%s\nversion=%s\narch=%s\ndesc=%s\n", meta.name,
            meta.version, meta.arch, meta.desc);
        if (meta.ndeps) {
          int d;
          fprintf(o, "depends=");
          for (d = 0; d < meta.ndeps; d++)
            fprintf(o, "%s%s", d ? "," : "", meta.depends[d]);
          fprintf(o, "\n");
        }
        fclose(o);
      }
    }
  }
  unlink(jpath);

  if (run_script(stage, "+POSTINSTALL") < 0) {
    // package is registered; leave files — scripts failed after commit
    warnx("%s: +POSTINSTALL failed after install", meta.name);
  }
  printf("installed %s-%s\n", meta.name, meta.version);
  stage_cleanup(stage);
  return 0;

fail:
  stage_cleanup(stage);
  return -1;
}

static int install_one(const char *name, int depth) {
  struct pkg p;
  int repo;
  char dst[PATH_MAX], tmp[PATH_MAX];

  if (depth > MAX_DEPTH) {
    warnx("%s: dependency depth exceeded", name);
    return -1;
  }
  if (strchr(name, '/') || (strlen(name) > 5 &&
          strcmp(name + strlen(name) - 5, ".fpkg") == 0))
    return install_fpkg(name, depth);

  if (is_installed(name))
    return 0;

  load_repos();
  if (index_find2(name, &p, &repo) < 0) {
    warnx("%s: not in any repository (run fnupkg update?)", name);
    return -1;
  }
  if (p.arch[0] && strcmp(p.arch, "any") != 0 &&
      strcmp(p.arch, MACHINE_ARCH) != 0) {
    warnx("%s: package arch %s does not match %s", name, p.arch, MACHINE_ARCH);
    return -1;
  }
  snprintf(tmp, sizeof(tmp), CACHE "/%s", p.file);
  apath(dst, sizeof(dst), tmp);
  {
    char *slash = strrchr(dst, '/');
    if (slash) {
      char save = *slash;
      *slash = 0;
      mkdir_p(dst, 0755);
      *slash = save;
    }
  }
  if (fetch_rel(g_repos[repo].base, p.file, dst, p.size) < 0) {
    warnx("%s: cannot fetch %s%s", name, p.file,
        errno == EMSGSIZE ? " (size mismatch)" : "");
    return -1;
  }
  {
    struct stat st;
    if (stat(dst, &st) == 0 && p.size > 0 && (long)st.st_size != p.size) {
      warnx("%s: size mismatch (got %lld want %ld)", p.file,
          (long long)st.st_size, p.size);
      return -1;
    }
  }
  {
    char sha[SHA256_DIGEST_STRING_LENGTH];
    if (file_sha(dst, sha, sizeof(sha)) < 0) {
      warn("%s", dst);
      return -1;
    }
    if (!hex_eq(sha, p.sha)) {
      warnx("%s: sha256 mismatch (got %s want %s)", p.file, sha, p.sha);
      return -1;
    }
  }
  return install_fpkg(dst, depth);
}

static int cmd_install(int argc, char **argv) {
  int i, rc = 0;

  if (argc < 1)
    usage();
  for (i = 0; i < argc; i++)
    if (install_one(argv[i], 0) < 0)
      rc = 1;
  return rc;
}

static int remove_one(const char *name) {
  char path[PATH_MAX], tmp[PATH_MAX], line[LINE_MAX_LEN];
  FILE *f;
  struct pkg p;
  char dependent[64];

  strlcpy(tmp, INSTALLED "/%s", sizeof(tmp));
  snprintf(path, sizeof(path), tmp, name);
  apath(path, sizeof(path), path);
  if (meta_load(path, &p) < 0) {
    warnx("%s: not installed", name);
    return -1;
  }
  if (reverse_deps(name, dependent, sizeof(dependent))) {
    warnx("%s: required by %s", name, dependent);
    return -1;
  }

  // saved +PREREMOVE, if any
  {
    char script[PATH_MAX];
    struct stat st;
    snprintf(script, sizeof(script), "%s/scripts/%s", DB, name);
    apath(script, sizeof(script), script);
    if (stat(script, &st) == 0) {
      pid_t pid = fork();
      if (pid == 0) {
        execl("/bin/sh", "sh", script, (char *)0);
        _exit(127);
      } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
          warnx("+PREREMOVE failed for %s", name);
      }
      unlink(script);
    }
  }

  // delete files
  strlcpy(tmp, FILES "/%s", sizeof(tmp));
  snprintf(path, sizeof(path), tmp, name);
  apath(path, sizeof(path), path);
  if ((f = fopen(path, "r")) == 0) {
    warn("%s", path);
    // still drop meta so the package does not stick forever
  } else {
    while (fgets(line, sizeof(line), f)) {
      char *rel, *psep;
      line[strcspn(line, "\n")] = 0;
      if (line[0] == 0)
        continue;
      if (line[0] == '-') {
        rel = line + 1;
        while (*rel == ' ')
          rel++;
        psep = strstr(rel, " -> ");
        if (psep)
          *psep = 0;
      } else {
        psep = strchr(line, ' ');
        if (!psep)
          continue;
        rel = psep;
        while (*rel == ' ')
          rel++;
      }
      if (rel[0] == 0)
        continue;
      {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", g_root[0] ? g_root : "", rel);
        if (unlink(full) < 0 && errno != ENOENT)
          warn("%s", full);
        prune_empty_parents(rel);
      }
    }
    fclose(f);
    unlink(path);
  }

  strlcpy(tmp, INSTALLED "/%s", sizeof(tmp));
  snprintf(path, sizeof(path), tmp, name);
  apath(path, sizeof(path), path);
  unlink(path);
  printf("removed %s-%s\n", p.name, p.version);
  return 0;
}

static int cmd_remove(int argc, char **argv) {
  int i, rc = 0;

  if (argc < 1)
    usage();
  for (i = 0; i < argc; i++)
    if (remove_one(argv[i]) < 0)
      rc = 1;
  return rc;
}

// reinstall from repo when index version differs from installed
static int upgrade_one(const char *name) {
  struct pkg inst, avail;
  int repo;
  char path[PATH_MAX], tmp[PATH_MAX];

  strlcpy(tmp, INSTALLED "/%s", sizeof(tmp));
  snprintf(path, sizeof(path), tmp, name);
  apath(path, sizeof(path), path);
  if (meta_load(path, &inst) < 0) {
    warnx("%s: not installed", name);
    return -1;
  }
  load_repos();
  if (index_find2(name, &avail, &repo) < 0) {
    warnx("%s: not in any repository", name);
    return -1;
  }
  if (strcmp(inst.version, avail.version) == 0) {
    printf("%s-%s is up to date\n", name, inst.version);
    return 0;
  }
  printf("upgrading %s %s -> %s\n", name, inst.version, avail.version);
  if (remove_one(name) < 0)
    return -1;
  return install_one(name, 0);
}

static int cmd_upgrade(int argc, char **argv) {
  int rc = 0;

  if (argc == 0) {
    char path[PATH_MAX], tmp[PATH_MAX];
    DIR *d;
    struct dirent *de;
    char **names = 0;
    size_t n = 0, cap = 0, i;

    strlcpy(tmp, INSTALLED, sizeof(tmp));
    apath(path, sizeof(path), tmp);
    if ((d = opendir(path)) == 0)
      return 0;
    while ((de = readdir(d))) {
      if (de->d_name[0] == '.')
        continue;
      if (n == cap) {
        cap = cap ? cap * 2 : 32;
        names = realloc(names, cap * sizeof(*names));
        if (!names)
          err(1, "realloc");
      }
      names[n++] = strdup(de->d_name);
    }
    closedir(d);
    for (i = 0; i < n; i++) {
      if (upgrade_one(names[i]) < 0)
        rc = 1;
      free(names[i]);
    }
    free(names);
    return rc;
  }
  {
    int i;
    for (i = 0; i < argc; i++)
      if (upgrade_one(argv[i]) < 0)
        rc = 1;
  }
  return rc;
}

static int cmd_repo(int argc, char **argv) {
  int i;

  if (argc < 1 || strcmp(argv[0], "list") != 0)
    usage();
  load_repos();
  printf("%s\n", CONF);
  if (g_nrepos == 0) {
    printf("  (no repositories — add one path or http:// URL per line)\n");
    return 0;
  }
  for (i = 0; i < g_nrepos; i++) {
    printf("%d  %s%s", i, g_repos[i].base,
        base_is_http(g_repos[i].base) ? "" : "  (local)");
    if (g_repos[i].pin[0])
      printf("  pin=sha256:%s", g_repos[i].pin);
    else if (base_is_http(g_repos[i].base))
      printf("  (no pin — optional for http://)");
    printf("\n");
  }
  return 0;
}

int main(int argc, char **argv) {
  int ch, rc = 0;

  while ((ch = getopt(argc, argv, "C:h")) != -1) {
    switch (ch) {
    case 'C':
      strlcpy(g_root, optarg, sizeof(g_root));
      {
        size_t n = strlen(g_root);
        while (n > 1 && g_root[n - 1] == '/')
          g_root[--n] = 0;
        if (strcmp(g_root, "/") == 0)
          g_root[0] = 0;
      }
      break;
    default:
      usage();
    }
  }
  argc -= optind;
  argv += optind;
  if (argc < 1)
    usage();

  // only root (via doas/su or directly) may change the system or talk to repos
  if (geteuid() != 0) {
    warnx("must be root (use doas)");
    return 1;
  }

  if (strcmp(argv[0], "update") == 0) {
    load_repos();
    rc = cmd_update();
  } else if (strcmp(argv[0], "search") == 0) {
    if (argc < 2)
      usage();
    load_repos();
    rc = cmd_search(argv[1]);
  } else if (strcmp(argv[0], "info") == 0) {
    if (argc < 2)
      usage();
    rc = cmd_info(argv[1]);
  } else if (strcmp(argv[0], "list") == 0) {
    rc = cmd_list();
  } else if (strcmp(argv[0], "install") == 0) {
    rc = cmd_install(argc - 1, argv + 1);
  } else if (strcmp(argv[0], "remove") == 0) {
    rc = cmd_remove(argc - 1, argv + 1);
  } else if (strcmp(argv[0], "upgrade") == 0) {
    rc = cmd_upgrade(argc - 1, argv + 1);
  } else if (strcmp(argv[0], "files") == 0) {
    if (argc < 2)
      usage();
    rc = cmd_files(argv[1]);
  } else if (strcmp(argv[0], "check") == 0) {
    rc = cmd_check(argc > 1 ? argv[1] : 0);
  } else if (strcmp(argv[0], "repo") == 0) {
    rc = cmd_repo(argc - 1, argv + 1);
  } else
    usage();
  return rc;
}
