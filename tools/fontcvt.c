// fontcvt: BDF faces to the kernel's console font table
// the glyphs in code point order, the map in runs
//
// fontcvt -l license [-w min_screen_width name file.bdf] ... > kern/font_spleen.c
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXFACES 8
#define MAXGLYPHS 65536

struct glyph {
  uint32_t cp;
  uint8_t *bits;
};

struct face {
  const char *name;
  unsigned min_width;
  unsigned width, height, stride;
  struct glyph *glyphs;
  unsigned nglyphs;
};

static void die(const char *what, const char *where) {
  fprintf(stderr, "fontcvt: %s: %s\n", where, what);
  exit(1);
}

static int by_cp(const void *a, const void *b) {
  uint32_t x = ((const struct glyph *)a)->cp, y = ((const struct glyph *)b)->cp;

  return x < y ? -1 : x > y;
}

static void read_bdf(struct face *f, const char *path) {
  char line[512];
  FILE *fp = fopen(path, "r");
  int fw = 0, fh = 0, fx = 0, fy = 0, w = 0, h = 0, x = 0, y = 0;
  int row = -1, top = 0, enc = -1;
  struct glyph *g = 0;

  if (fp == 0)
    die("cannot open", path);
  if ((f->glyphs = calloc(MAXGLYPHS, sizeof(*f->glyphs))) == 0)
    die("out of memory", path);
  while (fgets(line, sizeof(line), fp) != 0) {
    if (sscanf(line, "FONTBOUNDINGBOX %d %d %d %d", &fw, &fh, &fx, &fy) == 4) {
      f->width = (unsigned)fw;
      f->height = (unsigned)fh;
      f->stride = (f->width + 7) / 8;
    } else if (sscanf(line, "ENCODING %d", &enc) == 1) {
      ;
    } else if (sscanf(line, "BBX %d %d %d %d", &w, &h, &x, &y) == 4) {
      if (fw == 0 || w > fw || h > fh || x < fx || x - fx + w > fw ||
          y < fy || y - fy + h > fh)
        die("glyph outside the bounding box", path);
    } else if (strncmp(line, "BITMAP", 6) == 0) {
      if (enc < 0) {
        g = 0;
        continue;
      }
      if (f->nglyphs == MAXGLYPHS)
        die("too many glyphs", path);
      g = &f->glyphs[f->nglyphs++];
      g->cp = (uint32_t)enc;
      if ((g->bits = calloc(f->height, f->stride)) == 0)
        die("out of memory", path);
      top = (fh + fy) - (h + y);
      row = 0;
    } else if (strncmp(line, "ENDCHAR", 7) == 0) {
      g = 0;
      row = -1;
      enc = -1;
    } else if (g != 0 && row >= 0 && row < h) {
      unsigned long v = strtoul(line, 0, 16);
      int nbits = (int)(strspn(line, "0123456789abcdefABCDEF") * 4), i;

      for (i = 0; i < w; i++)
        if (v >> (nbits - 1 - i) & 1) {
          int col = x - fx + i;

          g->bits[(top + row) * (int)f->stride + col / 8] |= (uint8_t)(0x80 >> (col % 8));
        }
      row++;
    }
  }
  fclose(fp);
  if (f->nglyphs == 0)
    die("no glyphs", path);
  qsort(f->glyphs, f->nglyphs, sizeof(*f->glyphs), by_cp);
}

static void emit(const struct face *f, unsigned n) {
  unsigned i, j, k, runs = 0, start;

  printf("static const uchar glyphs_%u[%u * %u * %u] = {\n", n, f->nglyphs,
         f->height, f->stride);
  for (i = 0; i < f->nglyphs; i++) {
    printf("    // U+%04X\n   ", f->glyphs[i].cp);
    for (j = 0, k = 0; j < f->height * f->stride; j++, k++) {
      if (k == 12) {
        printf("\n   ");
        k = 0;
      }
      printf(" 0x%02x,", f->glyphs[i].bits[j]);
    }
    printf("\n");
  }
  printf("};\n\n");

  for (i = 0; i < f->nglyphs; i = j, runs++)
    for (j = i + 1; j < f->nglyphs && f->glyphs[j].cp == f->glyphs[j - 1].cp + 1; j++)
      ;
  printf("static const struct font_map map_%u[%u] = {\n", n, runs);
  for (i = 0; i < f->nglyphs; i = j) {
    start = i;
    for (j = i + 1; j < f->nglyphs && f->glyphs[j].cp == f->glyphs[j - 1].cp + 1; j++)
      ;
    printf("    {0x%04x, %u, %u},\n", f->glyphs[start].cp, start, j - start);
  }
  printf("};\n\n");
}

static unsigned count_runs(const struct face *f) {
  unsigned i, runs = 1;

  for (i = 1; i < f->nglyphs; i++)
    if (f->glyphs[i].cp != f->glyphs[i - 1].cp + 1)
      runs++;
  return runs;
}

int main(int argc, char **argv) {
  struct face faces[MAXFACES];
  const char *license = 0;
  char line[512];
  unsigned nfaces = 0, i;
  FILE *fp;
  int a;

  memset(faces, 0, sizeof(faces));
  for (a = 1; a < argc; a++) {
    if (strcmp(argv[a], "-l") == 0 && a + 1 < argc)
      license = argv[++a];
    else if (strcmp(argv[a], "-w") == 0 && a + 3 < argc && nfaces < MAXFACES) {
      faces[nfaces].min_width = (unsigned)strtoul(argv[a + 1], 0, 10);
      faces[nfaces].name = argv[a + 2];
      read_bdf(&faces[nfaces++], argv[a + 3]);
      a += 3;
    } else {
      fprintf(stderr, "usage: fontcvt -l license [-w min_screen_width name file.bdf] ...\n");
      return 1;
    }
  }
  if (license == 0 || nfaces == 0) {
    fprintf(stderr, "usage: fontcvt -l license [-w min_screen_width name file.bdf] ...\n");
    return 1;
  }

  if ((fp = fopen(license, "r")) == 0)
    die("cannot open", license);
  printf("/*\n");
  while (fgets(line, sizeof(line), fp) != 0) {
    line[strcspn(line, "\n")] = 0;
    printf(line[0] != 0 ? " * %s\n" : " *\n", line);
  }
  fclose(fp);
  printf(" */\n\n// made by tools/fontcvt from the BDF faces; do not edit\n");
  printf("#include \"font.h\"\n\n");
  for (i = 0; i < nfaces; i++)
    emit(&faces[i], i);

  printf("// widest first, as rasops_init picks\n");
  printf("const struct console_font console_fonts[] = {\n");
  for (i = 0; i < nfaces; i++)
    printf("    {\"%s\", glyphs_%u, map_%u, %u, %u, %u, %u, %u, %u},\n", faces[i].name,
           i, i, count_runs(&faces[i]), faces[i].nglyphs, faces[i].width,
           faces[i].height, faces[i].stride, faces[i].min_width);
  printf("};\n\nconst uint console_font_count =\n"
         "    sizeof(console_fonts) / sizeof(console_fonts[0]);\n");
  return 0;
}
