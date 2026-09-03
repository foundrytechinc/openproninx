#include "user.h"

int main(int argc, char **argv) {
  struct network_dns_request request;
  int i;
  if (argc != 2) { printf("usage: resolve hostname\n"); exit(); }
  for (i = 0; i < NETWORK_DNS_NAME_MAX; i++) {
    request.name[i] = argv[1][i];
    if (!argv[1][i]) break;
  }
  if (i == NETWORK_DNS_NAME_MAX) { printf("resolve: name is too long\n"); exit(); }
  request.timeout_ms = 5000;
  if (dns_resolve(&request) < 0)
    printf("resolve: no IPv4 DNS answer for %s\n", request.name);
  else
    printf("%s: %d.%d.%d.%d\n", request.name, request.address[0], request.address[1], request.address[2], request.address[3]);
  exit();
}
