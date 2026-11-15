#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int alphasort(const struct dirent **a, const struct dirent **b) {
  return strcmp((*a)->d_name, (*b)->d_name);
}

int scandir(const char *dirname, struct dirent ***namelist,
            int (*select)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
  struct dirent *d, *p, **names = 0, **grown;
  size_t nitems = 0, arraysz = 32;
  DIR *dirp;

  if ((dirp = opendir(dirname)) == 0)
    return -1;
  if ((names = calloc(arraysz, sizeof(*names))) == 0)
    goto fail;
  while ((d = readdir(dirp)) != 0) {
    if (select != 0 && !(*select)(d))
      continue;
    if ((p = malloc(d->d_reclen)) == 0)
      goto fail;
    memcpy(p, d, d->d_reclen);
    if (nitems >= arraysz) {
      arraysz *= 2;
      if ((grown = reallocarray(names, arraysz, sizeof(*names))) == 0) {
        free(p);
        goto fail;
      }
      names = grown;
    }
    names[nitems++] = p;
  }
  closedir(dirp);
  if (nitems && compar)
    qsort(names, nitems, sizeof(*names),
          (int (*)(const void *, const void *))compar);
  *namelist = names;
  return (int)nitems;

fail:
  while (nitems > 0)
    free(names[--nitems]);
  free(names);
  closedir(dirp);
  return -1;
}
