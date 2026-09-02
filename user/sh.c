// PSH (PRONINX SHELL)

#include "user/user.h"
#include "inc/product.h"

char current_dir[64] = "/";

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

int fork1(void);  // Fork but panics on failure.
void panic(char*) __attribute__((noreturn));
struct cmd *parsecmd(char*);
void runcmd(struct cmd*) __attribute__((noreturn));

static struct procinfo status_processes[64];

static int command_is(char *command, char *name) {
  int length = strlen(name);
  return strncmp(command, name, length) == 0 &&
         (command[length] == '\n' || command[length] == '\r' ||
          command[length] == 0);
}

static void print_file(char *path, char *missing) {
  char buffer[256];
  int count, fd = open(path, O_RDONLY);

  if (fd < 0) {
    printf("%s\n", missing);
    return;
  }
  while ((count = read(fd, buffer, sizeof(buffer))) > 0) {
    if (write(1, buffer, count) != count) {
      printf("PSH: console write failed\n");
      break;
    }
  }
  close(fd);
}

static void fnu_status(void) {
  struct info system;
  int count, i;

  if (info(&system) < 0 ||
      (count = procinfo(status_processes, 64)) < 0) {
    printf("PSH: cannot read system state\n");
    return;
  }
  printf("%s %s\n", FNU_PRODUCT_NAME, FNU_PRODUCT_VERSION);
  printf("uptime=%d ticks processes=%d free=%d KB\n", system.uptime,
         system.nprocs, system.free_ram / 1024);
  for (i = 0; i < count; i++)
    printf("  %d  %s  state=%d  memory=%d KB\n", status_processes[i].pid,
           status_processes[i].name, status_processes[i].state,
           status_processes[i].memory_size / 1024);
}

static void service_command(char *command) {
  int fd = open("/fnusvc.command", O_WRONLY);
  int length = strlen(command);

  if (fd < 0) {
    printf("PSH: service supervisor is unavailable\n");
    return;
  }
  if (write(fd, command, length) == length)
    printf("PSH: command queued\n");
  else
    printf("PSH: cannot submit service command\n");
  close(fd);
}

// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;

  if(cmd == 0)
    exit();

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0)
      exit();
    
    // Try to run from current directory
    exec(ecmd->argv[0], ecmd->argv);

    // If failed and not an absolute path, try running from root (/)
    if(ecmd->argv[0][0] != '/'){
      char path[64];
      path[0] = '/';
      int i = 0;
      while(ecmd->argv[0][i] && i < 62){
        path[i+1] = ecmd->argv[0][i];
        i++;
      }
      path[i+1] = 0;
      exec(path, ecmd->argv);

      // FNU system images keep executable payloads in /bin. Legacy images
      // still work through the root-level fallback above.
      path[0] = '/';
      path[1] = 'b';
      path[2] = 'i';
      path[3] = 'n';
      path[4] = '/';
      i = 0;
      while(ecmd->argv[0][i] && i < 58){
        path[i+5] = ecmd->argv[0][i];
        i++;
      }
      path[i+5] = 0;
      exec(path, ecmd->argv);
    }

    dprintf(2, "exec %s failed, perhaps this command does not exist\n", ecmd->argv[0]);
    exit();
    break;

  case REDIR:
    dprintf(2, "redir: not supported in current ABI (no dup2/pipe)\n");
    exit();
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    if(fork1() == 0)
      runcmd(lcmd->left);
    wait();
    runcmd(lcmd->right);
    break;

  case PIPE:
    dprintf(2, "pipe: not supported in current ABI (no dup2/pipe)\n");
    exit();
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    if(fork1() == 0)
      runcmd(bcmd->cmd);
    break;

  default:
    panic("runcmd: unknown command type");
  }
  exit();
}

int
getcmd(char *buf, int nbuf)
{
  dprintf(2, "\033[32mPSH \033[34m[%s] \033[32m$ \033[0m", current_dir); 
  
  memset(buf, 0, nbuf);
  gets(buf, nbuf);
  if(buf[0] == 0) return -1;
  return 0;
}

int
main(void)
{
  static char buf[100];
  int fd;

  // Ensure that three file descriptors are open.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  printf("\033[33m");
  printf("       _/\\ \n");
  printf("     _/   \\_______ \n");
  printf("   _/  \\_ /  _    \\___ \n");
  printf("  / \\_   \\\\_/ \\       \\ \n");
  printf(" /    \\   \\|< >|      _\\ \n");
  printf("|      \\   \\\\_/      / | \n");
  printf("|       \\   \\        \\_| \n");
  printf(" \\       \\   \\        / \n");
  printf("  \\       \\   \\______/ \n");
  printf("   \\_______\\__/ \n");
  printf("\033[0m\n");
  printf("Welcome to %s %s!\n", FNU_PRODUCT_NAME, FNU_PRODUCT_VERSION);
  printf("Type 'help' for built-in commands.\n\n");

  // Read and run input commands.
  while(getcmd(buf, sizeof(buf)) >= 0){
    char *cmd = buf;
    while (*cmd == ' ' || *cmd == '\t')
      cmd++;
    if (*cmd == '\n' || *cmd == 0)
      continue;

    // Built-in command: help
    if(cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'l' && cmd[3] == 'p' && (cmd[4] == ' ' || cmd[4] == '\n' || cmd[4] == '\r' || cmd[4] == 0)){
      printf("PSH Built-in commands:\n");
      printf("  cd <dir>   - Change directory\n");
      printf("  clear      - Clear screen\n");
      printf("  info       - Show system info\n");
      printf("  status     - Show node and process state\n");
      printf("  services   - Show managed service state\n");
      printf("  health     - Show latest local health observation\n");
      printf("  start|stop|restart health - Manage health service\n");
      printf("  reboot     - Restart this node\n");
      printf("  help       - Show this message\n");
      continue;
    }

    if(command_is(cmd, "status")) {
      fnu_status();
      continue;
    }

    if(command_is(cmd, "services")) {
      print_file("/fnusvc.status", "PSH: service status is unavailable");
      continue;
    }

    if(command_is(cmd, "health")) {
      print_file("/.fnuhealth", "PSH: health observation is unavailable");
      continue;
    }

    if(command_is(cmd, "start health")) {
      service_command("start fnu-health\n");
      continue;
    }

    if(command_is(cmd, "stop health")) {
      service_command("stop fnu-health\n");
      continue;
    }

    if(command_is(cmd, "restart health")) {
      service_command("restart fnu-health\n");
      continue;
    }

    if(command_is(cmd, "reboot")) {
      reboot();
      continue;
    }

    // Built-in command: clear
    if(cmd[0] == 'c' && cmd[1] == 'l' && cmd[2] == 'e' && cmd[3] == 'a' && cmd[4] == 'r' && (cmd[5] == ' ' || cmd[5] == '\n' || cmd[5] == '\r' || cmd[5] == 0)){
      printf("\033[2J\033[H");
      continue;
    }

    // Built-in command: info
    if(cmd[0] == 'i' && cmd[1] == 'n' && cmd[2] == 'f' && cmd[3] == 'o' && (cmd[4] == ' ' || cmd[4] == '\n' || cmd[4] == '\r' || cmd[4] == 0)){
      printf("\033[33m");
      printf("       _/\\ \n");
      printf("     _/   \\_______ \n");
      printf("   _/  \\_ /  _    \\___     \033[0mOS: %s\n\033[33m", FNU_PRODUCT_NAME);
      printf("  / \\_   \\\\_/ \\       \\    \033[0mShell: PSH\n\033[33m");
      printf(" /    \\   \\|< >|      _\\   \033[0mArch: x86_64\n\033[33m");
      printf("|      \\   \\\\_/      / | \n");
      printf("|       \\   \\        \\_| \n");
      printf(" \\       \\   \\        / \n");
      printf("  \\       \\   \\______/ \n");
      printf("   \\_______\\__/ \n");
      printf("\033[0m\n");
      continue;
    }

    // Chdir must be called by the parent, not the child.
     if(cmd[0] == 'c' && cmd[1] == 'd' && (cmd[2] == ' ' || cmd[2] == '\n' || cmd[2] == '\r' || cmd[2] == 0)){
       char *dir = cmd + 2;
       while(*dir == ' ') dir++;  // Skip spaces
       
       // Handle empty cd (go to root)
       if(*dir == '\n' || *dir == '\r' || *dir == 0){
         dir = "/";
       } else {
         // Remove trailing newline
         char *end = dir;
         while(*end && *end != '\n' && *end != '\r') end++;
         *end = 0;
       }
       
       if(chdir(dir) < 0){
         dprintf(2, "cd: cannot change to %s\n", dir);
       } else {
         // Update current_dir display with path normalization
         if(strncmp(dir, "..", 3) == 0){
           // Go to parent directory
           int len = strlen(current_dir);
           if(len > 1){
             // Find last /
             int i;
             for(i = len - 1; i >= 0; i--){
               if(current_dir[i] == '/'){
                 current_dir[i] = 0;
                 if(i == 0){
                   safestrcpy(current_dir, "/", sizeof(current_dir));
                 }
                 break;
               }
             }
           }
         } else if(dir[0] == '/'){
           // Absolute path - normalize ..
           safestrcpy(current_dir, dir, sizeof(current_dir));
           
           // Simple .. resolution - remove /.. from end
           int len = strlen(current_dir);
           if(len >= 3 && current_dir[len-3] == '/' && current_dir[len-2] == '.' && current_dir[len-1] == '.'){
             current_dir[len-3] = 0;
             // Find previous /
             int i;
             for(i = len - 4; i >= 0; i--){
               if(current_dir[i] == '/'){
                 current_dir[i] = 0;
                 if(i == 0){
                   safestrcpy(current_dir, "/", sizeof(current_dir));
                 }
                 break;
               }
             }
           }
         } else {
           // Simple relative path
           int len = strlen(current_dir);
           if(len > 0 && current_dir[len-1] != '/' && len < 62){
             current_dir[len++] = '/';
             current_dir[len] = 0;
           }
           int i = 0;
           while(dir[i] && len < 63){
             current_dir[len++] = dir[i++];
           }
           current_dir[len] = 0;
         }
       }
       continue;
     }

    if(fork1() == 0)
      runcmd(parsecmd(buf));
    wait();
  }
  exit();
}

void
panic(char *s)
{
  dprintf(2, "%s\n", s);
  exit();
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}

// Parsing
char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    dprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE|O_TRUNC, 1);
      break;
    case '+':  // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
