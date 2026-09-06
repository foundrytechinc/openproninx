#include "user/user.h"
#include "inc/product.h"

char current_dir[64] = "/";
char current_user[USER_NAME_MAX] = "user";

/* ---- FNU / PSH command history. A simple ring buffer: HIST_LEN entries,
 * each at most HIST_CMD bytes per line. */
#define HIST_LEN 32
#define HIST_CMD 256

static char  history[HIST_LEN][HIST_CMD];
static int   hist_count;   /* 0 .. HIST_LEN */
static int   hist_next;   /* slot that will be overwritten on append */
static int   hist_cursor;  /* -1 = typing new line; 0..hist_count-1 = browsing */

static void history_init(void) {
  int i;
  for (i = 0; i < HIST_LEN; i++)
    history[i][0] = 0;
  hist_count  = 0;
  hist_next   = 0;
  hist_cursor = -1;
}

static void history_append(const char *line) {
  int len;
  /* Trim trailing whitespace and skip empty lines. */
  while (*line == ' ' || *line == '\t') line++;
  if (*line == 0 || *line == '\n' || *line == '\r')
    return;
  /* Drop duplicates of the most recent command. */
  if (hist_count > 0) {
    int last = (hist_next + HIST_LEN - 1) % HIST_LEN;
    if (strcmp(history[last], line) == 0)
      return;
  }
  len = strlen(line);
  if (len >= HIST_CMD) len = HIST_CMD - 1;
  memmove(history[hist_next], line, len);
  history[hist_next][len] = 0;
  hist_next = (hist_next + 1) % HIST_LEN;
  if (hist_count < HIST_LEN) hist_count++;
  hist_cursor = -1;
}

static const char *history_navigate(int direction) {
  if (hist_count == 0) return 0;
  if (hist_cursor < 0) {
    hist_cursor = (hist_next + HIST_LEN - 1) % HIST_LEN;
  } else {
    if (direction > 0) { /* UP -- older */
      int newest = (hist_next + HIST_LEN - 1) % HIST_LEN;
      int oldest = (hist_next + HIST_LEN - hist_count) % HIST_LEN;
      if (hist_cursor == oldest) return 0; /* cannot go older */
      hist_cursor = (hist_cursor + HIST_LEN - 1) % HIST_LEN;
    } else {          /* DOWN -- newer */
      int newest = (hist_next + HIST_LEN - 1) % HIST_LEN;
      if (hist_cursor == newest) { hist_cursor = -1; return 0; }
      hist_cursor = (hist_cursor + 1) % HIST_LEN;
    }
  }
  return history[hist_cursor];
}

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

int fork1(void);
void panic(char*) __attribute__((noreturn));
struct cmd *parsecmd(char*);
void runcmd(struct cmd*) __attribute__((noreturn));

static struct procinfo status_processes[64];

static void resolve_current_user(void) {
  struct user_info info;
  int index;
  int uid = getuid();
  for (index = 0; users(&info, index) == 0; index++) {
    if (info.uid == (uint)uid) {
      safestrcpy(current_user, info.name, sizeof(current_user));
      return;
    }
  }
  safestrcpy(current_user, "unknown", sizeof(current_user));
}

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
    
    exec(ecmd->argv[0], ecmd->argv);

    if(ecmd->argv[0][0] != '/'){
      char path[64];
      path[0] = '/';
      path[1] = 'u';
      path[2] = 's';
      path[3] = 'r';
      path[4] = '/';
      path[5] = 'b';
      path[6] = 'i';
      path[7] = 'n';
      path[8] = '/';
      int i = 0;
      while(ecmd->argv[0][i] && i < 54){
        path[i+9] = ecmd->argv[0][i];
        i++;
      }
      path[i+9] = 0;
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

/* --------------------------------------------------------------------------
 * Custom line editor for PSH.  The console driver is switched to raw
 * (non-ICANON) mode so arrow keys arrive as single high-ASCII bytes from
 * kbd.c (KEY_UP=0xE2, KEY_DN=0xE3) or the standard VT100 ESC [ A / B escape
 * sequences on a serial line.  Explicit echoing keeps prompt redraws cheap.
 * -------------------------------------------------------------------------- */
static int psh_getch(void) {
  char c;
  int n = read(0, &c, 1);
  return n <= 0 ? -1 : (int)(unsigned char)c;
}

static void psh_bs_n(int n) {
  while (n-- > 0) {
    write(2, "\b \b", 3);
  }
}

static void psh_redraw_line(const char *buf, int len) {
  int i;
  psh_bs_n(len);
  for (i = 0; i < len; i++)
    write(2, &buf[i], 1);
  /* Clear anything after the new cursor position. */
  for (i = 0; i < 10; i++)
    write(2, " ", 1);
  for (i = 0; i < 10; i++)
    write(2, "\b", 1);
}

int
getcmd(char *buf, int nbuf)
{
  static struct termios cooked, raw;
  static int initialised;
  int len = 0, c, esc_state;

  if (!initialised) {
    history_init();
    initialised = 1;
  }
  if (ioctl(0, TCGETS, (uint64_t)&cooked) == 0) {
    raw = cooked;
    raw.c_lflag &= ~(ICANON | ECHO);
    ioctl(0, TCSETS, (uint64_t)&raw);
  }

  dprintf(2, "\033[32m%s@PSH \033[34m[%s] \033[32m$ \033[0m", current_user, current_dir);

  memset(buf, 0, nbuf);
  hist_cursor = -1;
  esc_state = 0;

  for (;;) {
    c = psh_getch();
    if (c < 0) break;
    if (esc_state == 1) {
      if (c == '[') { esc_state = 2; continue; }
      esc_state = 0;
      continue;
    }
    if (esc_state == 2) {
      if (c == 'A') c = 0xE2;  /* VT100 Up    -> KEY_UP   */
      else if (c == 'B') c = 0xE3;  /* VT100 Down  -> KEY_DN   */
      else if (c == 'C') c = 0xE5;  /* VT100 Right -> KEY_RT   */
      else if (c == 'D') c = 0xE4;  /* VT100 Left  -> KEY_LF   */
      esc_state = 0;
    }

    if (c == 0x1b) { esc_state = 1; continue; }

    switch (c) {
    case '\r':
    case '\n':
      write(2, "\r\n", 2);
      buf[len] = '\n';
      buf[len + 1] = 0;
      goto done;
    case 0x03:    /* ^C */
      write(2, "^C\r\n", 4);
      len = 0; buf[0] = 0;
      goto submit_empty;
    case 0x15:    /* ^U -- kill line */
      psh_bs_n(len);
      len = 0; buf[0] = 0;
      continue;
    case '\b':
    case 0x7f:    /* Backspace / DEL */
      if (len > 0) {
        buf[--len] = 0;
        write(2, "\b \b", 3);
      }
      continue;
    case 0xE2:    /* KEY_UP */
    {
      const char *h = history_navigate(+1);
      if (h == 0) continue;
      int hl = strlen(h);
      safestrcpy(buf, h, nbuf);
      buf[hl] = 0;
      psh_redraw_line(buf, len);
      len = hl;
      continue;
    }
    case 0xE3:    /* KEY_DN */
    {
      const char *h = history_navigate(-1);
      if (hist_cursor < 0) {
        /* Moved past newest -> empty input. */
        psh_redraw_line("", len);
        len = 0; buf[0] = 0;
        continue;
      }
      if (h == 0) continue;
      int hl = strlen(h);
      safestrcpy(buf, h, nbuf);
      buf[hl] = 0;
      psh_redraw_line(buf, len);
      len = hl;
      continue;
    }
    default:
      if (c < 0x20 || c >= 0x80)
        continue; /* ignore other non-printable / special kbd codes */
      if (len + 2 >= nbuf) {
        dprintf(2, "\nPSH: input line too long (> %d bytes)\n", nbuf - 2);
        len = 0; buf[0] = 0;
        goto submit_empty;
      }
      buf[len++] = (char)c;
      buf[len] = 0;
      write(2, &buf[len - 1], 1);
      continue;
    }
  }

done:
  /* Drop the trailing newline for history storage. */
  {
    char tmp[HIST_CMD];
    int i, hl;
    hl = len > HIST_CMD - 1 ? HIST_CMD - 1 : len;
    for (i = 0; i < hl; i++) {
      if (buf[i] == '\n' || buf[i] == '\r') break;
      tmp[i] = buf[i];
    }
    tmp[i] = 0;
    history_append(tmp);
  }
submit_empty:
  ioctl(0, TCSETS, (uint64_t)&cooked);
  if (buf[0] == 0) return -1;
  return 0;
}

int
main(void)
{
  static char buf[100];
  int fd, child;

  setforeground(0);
  resolve_current_user();

  while((fd = open("/dev/console", O_RDWR)) >= 0){
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
  printf("Shell: FNU/PSH (Proninx Shell) \n");
  printf("Type 'help' for built-in commands, or 'fnufetch' for system summary.\n\n");

  while(getcmd(buf, sizeof(buf)) >= 0){
    char *cmd = buf;
    while (*cmd == ' ' || *cmd == '\t')
      cmd++;
    if (*cmd == '\n' || *cmd == 0)
      continue;

    if(cmd[0] == 'h' && cmd[1] == 'i' && cmd[2] == 's' && cmd[3] == 't' && cmd[4] == 'o' && cmd[5] == 'r' && cmd[6] == 'y' && (cmd[7] == ' ' || cmd[7] == '\n' || cmd[7] == '\r' || cmd[7] == 0)){
      if (hist_count == 0) {
        printf("PSH: command history is empty (type some commands first; "
               "use Up/Down arrows to browse)\n");
      } else {
        int i, slot;
        int oldest = (hist_next + HIST_LEN - hist_count) % HIST_LEN;
        for (i = 0; i < hist_count; i++) {
          slot = (oldest + i) % HIST_LEN;
          if (history[slot][0] == 0) continue;
          printf(" %3d  %s\n", i + 1, history[slot]);
        }
      }
      continue;
    }

    if(cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'l' && cmd[3] == 'p' && (cmd[4] == ' ' || cmd[4] == '\n' || cmd[4] == '\r' || cmd[4] == 0)){
      printf("PSH (Proninx Shell) -- part of FNU userland\n");
      printf("Built-in commands:\n");
      printf("  cd <dir>   - Change directory\n");
      printf("  clear      - Clear screen\n");
      printf("  fnufetch   - Pretty system information (FNU fetch)\n");
      printf("  fnudf      - Disk usage summary (FNU df -h style)\n");
      printf("  fnudate    - Current RTC wall clock (FNU date)\n");
      printf("  info       - Alias for 'fnufetch'\n");
      printf("  history    - Show numbered PSH command history\n");
      printf("  status     - Show node and process state\n");
      printf("  services   - Show managed service state\n");
      printf("  health     - Show latest local health observation\n");
      printf("  start|stop|restart health - Manage health service\n");
      printf("  fbset      - Inspect or change screen resolution\n");
      printf("  man [topic]- Reference manual pages (fnuman)\n");
      printf("  reboot     - Restart this node\n");
      printf("  poweroff   - Shut this node down\n");
      printf("  logout     - End this session and return to login\n");
      printf("  help       - Show this message\n");
      printf("\nTip: use Up/Down arrows or PageUp/PageDown keys to browse history,\n");
      printf("     Ctrl-U to kill the line, Ctrl-C to interrupt input.\n");
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

    if(command_is(cmd, "poweroff") || command_is(cmd, "shutdown") || command_is(cmd, "halt")) {
      poweroff();
      continue;
    }

    // login execs this shell in its service process.  Exiting lets the
    // supervisor reap that process and start a fresh login prompt.
    if(command_is(cmd, "logout")) {
      printf("Logging out...\n");
      exit();
    }

    if(cmd[0] == 'c' && cmd[1] == 'l' && cmd[2] == 'e' && cmd[3] == 'a' && cmd[4] == 'r' && (cmd[5] == ' ' || cmd[5] == '\n' || cmd[5] == '\r' || cmd[5] == 0)){
      printf("\033[2J\033[H");
      continue;
    }

    if(command_is(cmd, "fnufetch") ||
       (cmd[0] == 'i' && cmd[1] == 'n' && cmd[2] == 'f' && cmd[3] == 'o' &&
        (cmd[4] == ' ' || cmd[4] == '\n' || cmd[4] == '\r' || cmd[4] == 0))) {
      char *argv[] = { "fnufetch", 0 };
      int child_pid = fork1();
      if (child_pid == 0) {
        exec("/fnufetch", argv);
        exec("/usr/bin/fnufetch", argv);
        dprintf(2, "PSH: fnufetch not found -- falling back to built-in info\n");
        printf("\033[33m");
        printf("       _/\\ \n");
        printf("     _/   \\_______ \n");
        printf("   _/  \\_ /  _    \\___     \033[0mOS: %s\n\033[33m", FNU_PRODUCT_NAME);
        printf("  / \\_   \\\\_/ \\       \\    \033[0mShell: PSH (FNU)\n\033[33m");
        printf(" /    \\   \\|< >|      _\\   \033[0mArch: x86_64\n\033[33m");
        printf("|      \\   \\\\_/      / | \n");
        printf("|       \\   \\        \\_| \n");
        printf(" \\       \\   \\        / \n");
        printf("  \\       \\   \\______/ \n");
        printf("   \\_______\\__/ \n");
        printf("\033[0m\n");
        exit();
      }
      setforeground(child_pid);
      wait();
      setforeground(0);
      continue;
    }

     if(cmd[0] == 'c' && cmd[1] == 'd' && (cmd[2] == ' ' || cmd[2] == '\n' || cmd[2] == '\r' || cmd[2] == 0)){
       char *dir = cmd + 2;
       while(*dir == ' ') dir++;
       
       if(*dir == '\n' || *dir == '\r' || *dir == 0){
         dir = "/";
       } else {
         char *end = dir;
         while(*end && *end != '\n' && *end != '\r') end++;
         *end = 0;
       }
       
       if(chdir(dir) < 0){
         dprintf(2, "cd: cannot change to %s\n", dir);
       } else {
         if(strncmp(dir, "..", 3) == 0){
           int len = strlen(current_dir);
           if(len > 1){
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
           safestrcpy(current_dir, dir, sizeof(current_dir));
           
           int len = strlen(current_dir);
           if(len >= 3 && current_dir[len-3] == '/' && current_dir[len-2] == '.' && current_dir[len-1] == '.'){
             current_dir[len-3] = 0;
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

    child = fork1();
    if(child == 0)
      runcmd(parsecmd(buf));
    setforeground(child);
    wait();
    setforeground(0);
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
