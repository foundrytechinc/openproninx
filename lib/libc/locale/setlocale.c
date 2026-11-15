// C and POSIX only
#include <locale.h>
#include <stdlib.h>
#include <string.h>

static int is_c(const char *name) {
  return name == 0 || name[0] == 0 || strcmp(name, "C") == 0 ||
         strcmp(name, "POSIX") == 0;
}

static const char *category_name[] = {"LC_ALL", "LC_COLLATE", "LC_CTYPE",
                                      "LC_MONETARY", "LC_NUMERIC", "LC_TIME",
                                      "LC_MESSAGES"};

char *setlocale(int category, const char *locale) {
  static char name[] = "C";

  if (category < LC_ALL || category > LC_MESSAGES)
    return 0;
  if (locale != 0 && locale[0] == 0) {
    locale = getenv("LC_ALL");
    if (locale == 0 || locale[0] == 0)
      locale = getenv(category_name[category]);
    if (locale == 0 || locale[0] == 0)
      locale = getenv("LANG");
  }
  return is_c(locale) ? name : 0;
}
