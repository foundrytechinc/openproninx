// cksumtest: cksum(1) and the sha programs against FIPS 180-4 and POSIX vectors
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void ok(const char *what, int good) {
  printf("cksumtest: %-46s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

// what the command printed is exactly this line
static int says(const char *cmd, const char *want) {
  char out[512];
  size_t n;
  FILE *fp;

  if ((fp = popen(cmd, "r")) == 0)
    return 0;
  n = fread(out, 1, sizeof(out) - 1, fp);
  out[n] = 0;
  if (pclose(fp) != 0)
    return 0;
  out[strcspn(out, "\n")] = 0;
  return strcmp(out, want) == 0;
}

static int put(const char *path, const char *text, size_t repeat) {
  FILE *fp = fopen(path, "w");
  size_t i;

  if (fp == 0)
    return -1;
  for (i = 0; i < repeat; i++)
    fputs(text, fp);
  return fclose(fp);
}

int main(void) {
  put("/tmp/cksum.abc", "abc", 1);
  put("/tmp/cksum.hello", "hello\n", 1);
  put("/tmp/cksum.empty", "", 1);
  put("/tmp/cksum.million", "a", 1000000);

  ok("POSIX crc of abc", says("cksum /tmp/cksum.abc", "1219131554 3 /tmp/cksum.abc"));
  ok("POSIX crc of nothing", says("cksum < /tmp/cksum.empty", "4294967295 0"));
  ok("POSIX crc of a line", says("cksum < /tmp/cksum.hello", "3015617425 6"));
  ok("sha224 abc", says("sha224 -q /tmp/cksum.abc",
                        "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"));
  ok("sha256 abc", says("sha256 -s abc",
                        "SHA256 (\"abc\") = "
                        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  ok("sha384 abc",
     says("sha384 -q /tmp/cksum.abc",
          "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded163"
          "1a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"));
  ok("sha512 abc",
     says("sha512 -q /tmp/cksum.abc",
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
  ok("sha512/256 abc", says("cksum -q -a sha512/256 /tmp/cksum.abc",
                            "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23"));
  ok("sha256 of a million a, many blocks",
     says("sha256 -q /tmp/cksum.million",
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
  ok("the BSD line", says("sha256 /tmp/cksum.hello",
                          "SHA256 (/tmp/cksum.hello) = "
                          "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03"));
  ok("-c takes its own lines",
     system("sha256 /tmp/cksum.abc /tmp/cksum.hello > /tmp/cksum.list && "
            "sha256 -c /tmp/cksum.list > /dev/null") == 0);
  ok("-c finds a changed file",
     put("/tmp/cksum.hello", "hullo\n", 1) == 0 &&
         system("sha256 -c /tmp/cksum.list > /dev/null 2>&1") != 0);
  ok("-r lines check with the algorithm named",
     system("sha512 -r /tmp/cksum.abc > /tmp/cksum.list && "
            "sha512 -c /tmp/cksum.list > /dev/null") == 0);

  unlink("/tmp/cksum.abc");
  unlink("/tmp/cksum.hello");
  unlink("/tmp/cksum.empty");
  unlink("/tmp/cksum.million");
  unlink("/tmp/cksum.list");
  printf("cksumtest: %s\n", failures ? "FAILED" : "ok");
  return failures != 0;
}
