#include "user/user.h"

#define MAX_LINES 1000
#define MAX_LINE_LEN 256

#define CTRL_KEY(k) ((k) & 0x1f)

// Keycodes from kern/kbd.h
#define KEY_UP 0xE2
#define KEY_DN 0xE3
#define KEY_LF 0xE4
#define KEY_RT 0xE5
#define KEY_DEL 0xE9

char (*lines)[MAX_LINE_LEN];
int num_lines = 0;
char filename[64];
int cursor_x = 0;
int cursor_y = 0;
int row_off = 0; // Vertical scroll offset
int screen_rows = 18;
struct termios orig_termios;
int dirty = 0;

void cooked_mode() {
  ioctl(0, TCSETS, (uint64_t)&orig_termios);
}

void raw_mode() {
  struct termios raw;
  if (ioctl(0, TCGETS, (uint64_t)&orig_termios) < 0) return;
  raw = orig_termios;
  raw.c_lflag &= ~(ICANON | ECHO);
  ioctl(0, TCSETS, (uint64_t)&raw);
}

void draw_screen() {
  // Move cursor to home (1,1) without clearing the whole screen to avoid flicker
  printf("\033[H");

  // Header
  printf("\033[7m PRONINX Nano 0.0.1   File: %-30s %s \033[0m\033[K\n", filename, dirty ? "(modified)" : "");

  // Content
  for (int i = 0; i < screen_rows; i++) {
    int file_row = i + row_off;
    if (file_row < num_lines) {
      // Print line
      // We need to be careful with the length to not overflow 80 chars
      // Print line and clear to end of row
      char temp[MAX_LINE_LEN];
      safestrcpy(temp, lines[file_row], MAX_LINE_LEN);
      // Remove newline for printing if it exists
      int len = strlen(temp);
      if (len > 0 && temp[len-1] == '\n') temp[len-1] = 0;
      
      printf("%s\033[K\n", temp);
    } else {
      printf("~\033[K\n");
    }
  }

  // Footer (Shortcuts)
  // Clear any remaining space before footer if necessary
  // But our screen_rows + header + footer should fill 24-25 lines
  printf("\033[23;1H\033[7m ^X \033[0m Exit      \033[7m ^O \033[0m Write Out\033[K\n");
  printf("\033[24;1H\033[7m ^K \033[0m Cut Line  \033[7m ^U \033[0m Uncut Line\033[K");
  
  // Move cursor back to editing position
  printf("\033[%d;%dH", (cursor_y - row_off) + 2, cursor_x + 1);
}

void save_file() {
  int fd;
  if ((fd = open(filename, O_WRONLY | O_CREATE | O_TRUNC)) < 0) {
    return;
  }
  for (int i = 0; i < num_lines; i++) {
    write(fd, lines[i], strlen(lines[i]));
  }
  close(fd);
  dirty = 0;
}

void scroll() {
  if (cursor_y < row_off) {
    row_off = cursor_y;
  }
  if (cursor_y >= row_off + screen_rows) {
    row_off = cursor_y - screen_rows + 1;
  }
}

void insert_char(int c) {
  int len = strlen(lines[cursor_y]);
  if (len >= MAX_LINE_LEN - 2) return;
  
  // Shift characters to the right
  for (int i = len; i >= cursor_x; i--) {
    lines[cursor_y][i+1] = lines[cursor_y][i];
  }
  lines[cursor_y][cursor_x] = c;
  cursor_x++;
  dirty = 1;
}

void delete_char() {
  int len = strlen(lines[cursor_y]);
  if (cursor_x == 0) {
    if (cursor_y == 0) return;
    // Join with previous line
    int prev_len = strlen(lines[cursor_y-1]);
    // Remove newline from previous line if it exists
    if (prev_len > 0 && lines[cursor_y-1][prev_len-1] == '\n') {
      prev_len--;
      lines[cursor_y-1][prev_len] = 0;
    }
    
    if (prev_len + len < MAX_LINE_LEN) {
      // Manual strcat
      for (int i = 0; i <= len; i++) {
        lines[cursor_y-1][prev_len + i] = lines[cursor_y][i];
      }
      // Shift all lines up
      for (int i = cursor_y; i < num_lines - 1; i++) {
        safestrcpy(lines[i], lines[i+1], MAX_LINE_LEN);
      }
      num_lines--;
      cursor_y--;
      cursor_x = prev_len;
      dirty = 1;
    }
  } else {
    // Delete character at cursor_x - 1
    for (int i = cursor_x - 1; i < len; i++) {
      lines[cursor_y][i] = lines[cursor_y][i+1];
    }
    cursor_x--;
    dirty = 1;
  }
}

void split_line() {
  if (num_lines >= MAX_LINES) return;
  
  // Shift all lines down
  for (int i = num_lines; i > cursor_y + 1; i--) {
    safestrcpy(lines[i], lines[i-1], MAX_LINE_LEN);
  }
  
  // Copy rest of line to next line
  safestrcpy(lines[cursor_y + 1], &lines[cursor_y][cursor_x], MAX_LINE_LEN);
  // Truncate current line
  lines[cursor_y][cursor_x] = '\n';
  lines[cursor_y][cursor_x + 1] = 0;
  
  num_lines++;
  cursor_y++;
  cursor_x = 0;
  dirty = 1;
}

char cut_buffer[MAX_LINE_LEN] = {0};

void cut_line() {
  if (num_lines == 0) return;
  safestrcpy(cut_buffer, lines[cursor_y], MAX_LINE_LEN);
  
  // Shift all lines up
  for (int i = cursor_y; i < num_lines - 1; i++) {
    safestrcpy(lines[i], lines[i+1], MAX_LINE_LEN);
  }
  num_lines--;
  if (num_lines == 0) {
    num_lines = 1;
    lines[0][0] = '\n';
    lines[0][1] = 0;
  }
  if (cursor_y >= num_lines) cursor_y = num_lines - 1;
  cursor_x = 0;
  dirty = 1;
}

void uncut_line() {
  if (num_lines >= MAX_LINES || cut_buffer[0] == 0) return;
  
  // Shift all lines down
  for (int i = num_lines; i > cursor_y; i--) {
    safestrcpy(lines[i], lines[i-1], MAX_LINE_LEN);
  }
  safestrcpy(lines[cursor_y], cut_buffer, MAX_LINE_LEN);
  num_lines++;
  cursor_x = 0;
  dirty = 1;
}

void delete_char_at_cursor() {
  int len = strlen(lines[cursor_y]);
  if (cursor_x == len || (len > 0 && cursor_x == len - 1 && lines[cursor_y][cursor_x] == '\n')) {
    if (cursor_y == num_lines - 1) return;
    // Join with next line
    int next_len = strlen(lines[cursor_y+1]);
    if (len + next_len < MAX_LINE_LEN) {
      // Remove newline if it exists
      if (len > 0 && lines[cursor_y][len-1] == '\n') {
        len--;
        lines[cursor_y][len] = 0;
      }
      // Manual join
      for (int i = 0; i <= next_len; i++) {
        lines[cursor_y][len + i] = lines[cursor_y+1][i];
      }
      // Shift all lines up
      for (int i = cursor_y + 1; i < num_lines - 1; i++) {
        safestrcpy(lines[i], lines[i+1], MAX_LINE_LEN);
      }
      num_lines--;
      dirty = 1;
    }
  } else {
    // Delete character AT cursor_x
    for (int i = cursor_x; i < len; i++) {
      lines[cursor_y][i] = lines[cursor_y][i+1];
    }
    dirty = 1;
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Usage: ped filename\n");
    exit();
  }

  lines = malloc(MAX_LINES * MAX_LINE_LEN);
  if (lines == 0) {
    printf("ped: malloc failed\n");
    exit();
  }
  memset(lines, 0, MAX_LINES * MAX_LINE_LEN);

  safestrcpy(filename, argv[1], sizeof(filename));

  // Try to read existing file
  int fd;
  if ((fd = open(filename, O_RDONLY)) >= 0) {
    char c;
    int col = 0;
    while (read(fd, &c, 1) > 0 && num_lines < MAX_LINES) {
      if (col < MAX_LINE_LEN - 1) {
        lines[num_lines][col++] = c;
      }
      if (c == '\n') {
        lines[num_lines][col] = 0;
        num_lines++;
        col = 0;
      }
    }
    if (col > 0 && num_lines < MAX_LINES) {
      lines[num_lines][col] = 0;
      num_lines++;
    }
    close(fd);
  }

  if (num_lines == 0) {
    num_lines = 1;
    lines[0][0] = '\n';
    lines[0][1] = 0;
  }

  raw_mode();
  printf("\033[2J");

  while (1) {
    scroll();
    draw_screen();
    unsigned char c;
    if (read(0, &c, 1) <= 0) break;

    if (c == CTRL_KEY('x')) {
      break;
    } else if (c == CTRL_KEY('o')) {
      save_file();
    } else if (c == CTRL_KEY('k')) {
      cut_line();
    } else if (c == CTRL_KEY('u')) {
      uncut_line();
    } else if (c == KEY_DEL) {
      delete_char_at_cursor();
    } else if (c == KEY_UP) {
      if (cursor_y > 0) {
        cursor_y--;
        int len = strlen(lines[cursor_y]);
        if (cursor_x > len) cursor_x = len;
        // If the last char is \n, don't put cursor after it
        if (cursor_x > 0 && lines[cursor_y][cursor_x-1] == '\n') cursor_x--;
      }
    } else if (c == KEY_DN) {
      if (cursor_y < num_lines - 1) {
        cursor_y++;
        int len = strlen(lines[cursor_y]);
        if (cursor_x > len) cursor_x = len;
        if (cursor_x > 0 && lines[cursor_y][cursor_x-1] == '\n') cursor_x--;
      }
    } else if (c == KEY_LF) {
      if (cursor_x > 0) cursor_x--;
      else if (cursor_y > 0) {
        cursor_y--;
        cursor_x = strlen(lines[cursor_y]);
        if (cursor_x > 0 && lines[cursor_y][cursor_x-1] == '\n') cursor_x--;
      }
    } else if (c == KEY_RT) {
      int len = strlen(lines[cursor_y]);
      // Allow moving to the \n character but not past it
      int limit = len;
      if (len > 0 && lines[cursor_y][len-1] == '\n') limit--;
      
      if (cursor_x < limit) cursor_x++;
      else if (cursor_y < num_lines - 1) {
        cursor_y++;
        cursor_x = 0;
      }
    } else if (c == '\r' || c == '\n') {
      split_line();
    } else if (c == 127 || c == 8 || c == '\b') { // Backspace
      delete_char();
    } else if (c >= 32 && c <= 126) {
      insert_char(c);
    }
  }

  cooked_mode();
  printf("\033[2J\033[H"); // Clear on exit
  exit();
}
