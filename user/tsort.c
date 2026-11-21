// tsort — topological sort of a partial order (POSIX)
// input: pairs of items, one pair per line (whitespace separated)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 4096
#define MAXE 65536

static char *names[MAXN];
static int nnames;
static int adj[MAXN][64];
static int nadj[MAXN];
static int indeg[MAXN];
static int have[MAXN];

static int id(const char *s) {
  int i;
  for (i = 0; i < nnames; i++)
    if (strcmp(names[i], s) == 0)
      return i;
  if (nnames >= MAXN) {
    fprintf(stderr, "tsort: too many items\n");
    exit(1);
  }
  names[nnames] = strdup(s);
  if (!names[nnames]) {
    fprintf(stderr, "tsort: out of memory\n");
    exit(1);
  }
  have[nnames] = 0;
  indeg[nnames] = 0;
  nadj[nnames] = 0;
  return nnames++;
}

static void edge(int a, int b) {
  int i;
  if (a == b)
    return; // ignore self-loop (POSIX allows it in input)
  for (i = 0; i < nadj[a]; i++)
    if (adj[a][i] == b)
      return;
  if (nadj[a] >= 64) {
    fprintf(stderr, "tsort: too many edges\n");
    exit(1);
  }
  adj[a][nadj[a]++] = b;
  indeg[b]++;
}

int main(void) {
  char w[256];
  int pending = -1;
  int have_pending = 0;
  int out = 0;
  int i, progress;

  while (scanf("%255s", w) == 1) {
    int idv = id(w);
    if (have_pending) {
      edge(pending, idv);
      have_pending = 0;
    } else {
      pending = idv;
      have_pending = 1;
    }
  }
  // odd word count: last item pairs with nothing — POSIX says error
  if (have_pending) {
    fprintf(stderr, "tsort: odd number of items\n");
    return 1;
  }

  for (;;) {
    progress = 0;
    for (i = 0; i < nnames; i++) {
      if (have[i] || indeg[i] != 0)
        continue;
      printf("%s\n", names[i]);
      have[i] = 1;
      out++;
      progress = 1;
      {
        int j;
        for (j = 0; j < nadj[i]; j++)
          indeg[adj[i][j]]--;
      }
    }
    if (!progress)
      break;
  }
  if (out != nnames) {
    fprintf(stderr, "tsort: loop detected\n");
    return 1;
  }
  return 0;
}
