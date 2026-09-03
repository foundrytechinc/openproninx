#include "user/user.h"

int
main(int argc, char *argv[])
{
  char buf[512];
  int fd_src, fd_dst, r, w;

  if(argc != 3){
    printf("Usage: cp <source> <destination>\n");
    exit();
  }

  if((fd_src = open(argv[1], O_RDONLY)) < 0){
    printf("cp: cannot open %s\n", argv[1]);
    exit();
  }

  if((fd_dst = open(argv[2], O_WRONLY|O_CREATE|O_TRUNC)) < 0){
    printf("cp: cannot create %s\n", argv[2]);
    close(fd_src);
    exit();
  }

  while((r = read(fd_src, buf, sizeof(buf))) > 0){
    w = write(fd_dst, buf, r);
    if(w != r){
      printf("cp: write error\n");
      break;
    }
  }

  if(r < 0){
    printf("cp: read error\n");
  }

  close(fd_src);
  close(fd_dst);
  exit();
}
