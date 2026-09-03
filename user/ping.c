#include "user.h"

static int parse_address(const char *text, uchar address[4]) {
  int part = 0;
  int value = 0;
  if (text == 0)
    return -1;
  for (;;) {
    if (*text >= '0' && *text <= '9') {
      value = value * 10 + *text++ - '0';
      if (value > 255)
        return -1;
    } else if ((*text == '.' && part < 3) || (*text == 0 && part == 3)) {
      address[part++] = value;
      value = 0;
      if (*text++ == 0)
        return 0;
    } else {
      return -1;
    }
  }
}

int main(int argc, char **argv) {
  struct network_ping_request request;
  if (argc != 2 || parse_address(argv[1], request.address) < 0) {
    printf("usage: ping IPv4-address\n");
    exit();
  }
  request.timeout_ms = 2000;
  request.round_trip_ms = 0;
  if (ping(&request) < 0) {
    printf("ping %s: timeout or network unavailable\n", argv[1]);
    exit();
  }
  printf("ping %s: reply in %d ms\n", argv[1], request.round_trip_ms);
  exit();
}
