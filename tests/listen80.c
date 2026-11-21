// bind/listen smoke: 0.0.0.0:80 like nginx
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

int main(void) {
  int s, one = 1;
  struct sockaddr_in sin;

  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    perror("socket");
    return 1;
  }
  if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0)
    perror("setsockopt REUSEADDR");
  memset(&sin, 0, sizeof sin);
  sin.sin_len = sizeof sin;
  sin.sin_family = AF_INET;
  sin.sin_port = htons(80);
  sin.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (struct sockaddr *)&sin, sizeof sin) < 0) {
    perror("bind 0.0.0.0:80");
    return 1;
  }
  if (listen(s, 511) < 0) {
    perror("listen 511");
    return 1;
  }
  printf("listen80: ok on 0.0.0.0:80\n");
  close(s);
  return 0;
}
