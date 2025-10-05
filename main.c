#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CTRL_KEY(c) (c & 0x1F)

#define ABUF_INIT {NULL, 0}

#define ZILO_TAB_STOP 8

#define ESC_CLEAR_LINE "\x1b[K"
#define ESC_CURSOR_HIDE "\x1b[?25l"
#define ESC_CURSOR_SHOW "\x1b[?25h"
#define ESC_CLEAR_SCREEN "\x1b[2J"
#define ESC_CURSOR_HOME "\x1b[H"
#define ESC_MOVE_BOTTOM_RIGHT "\x1b[999C\x1b[999B"
#define ESC_CURSOR_POS_REQ "\x1b[6n"

enum editor_key {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

void esc_cursor_home() {
  write(STDOUT_FILENO, ESC_CURSOR_HOME, strlen(ESC_CURSOR_HOME));
}

void esc_clear_screen() {
  write(STDOUT_FILENO, ESC_CLEAR_SCREEN, strlen(ESC_CLEAR_SCREEN));
}

void die(const char *s) {
  esc_clear_screen();
  esc_cursor_home();
  perror(s);
  exit(1);
}

typedef struct {
  char *chars;
  int size;
  char *rchars;
  int rsize;
} editor_row_t;

typedef struct {
  int cursor_x;
  int cursor_y;
  int term_rows;
  int term_cols;
  int row_off, col_off;
  int rows_size;
  int rows_cap;
  editor_row_t *rows;
  struct termios orig_term;
  char *filename;
  char status_msg[128];
  time_t status_msg_time;
} editor_config_t;

editor_config_t e_config;

typedef struct {
  char *buf;
  int size;
} abuf_t;

void ab_append(abuf_t *ab, char *buf, int size) {
  char *new_buf = realloc(ab->buf, ab->size + size);
  if (new_buf == NULL)
    die("ab_append realloc");

  memcpy(new_buf + ab->size, buf, size);
  ab->buf = new_buf;
  ab->size += size;
}

void ab_free(abuf_t *ab) { free(ab->buf); }

void disenable_raw_mode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &e_config.orig_term) == -1) {
    die("tcsetattr");
  }
}

void enable_raw_mode() {
  if (tcgetattr(STDIN_FILENO, &e_config.orig_term) == -1) {
    die("tcgetattr");
  }

  atexit(disenable_raw_mode);

  struct termios term = e_config.orig_term;

  term.c_cflag |= (CS8);
  term.c_oflag &= ~(OPOST);
  term.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  term.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  term.c_cc[VTIME] = 1;
  term.c_cc[VMIN] = 0;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &term) == -1) {
    die("tcsetattr");
  }
}

int get_term_size(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col != 0) {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }

  int i = strlen(ESC_MOVE_BOTTOM_RIGHT);
  if (write(STDOUT_FILENO, ESC_MOVE_BOTTOM_RIGHT, i) == -1)
    return -1;

  i = strlen(ESC_CURSOR_POS_REQ);
  if (write(STDOUT_FILENO, ESC_CURSOR_POS_REQ, i) == -1)
    return -1;

  // Read the response: "ESC [ rows ; cols R"

  i = 0;
  char buf[32] = {0};
  while (i < (int)(sizeof(buf) - 1)) {
    ssize_t n = read(STDIN_FILENO, &buf[i], 1);
    if (n <= 0 || buf[i] == 'R')
      break;
    i++;
  }
  if (buf[0] != 0x1b || buf[1] != '[')
    return -1;

  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;

  return 0;
}

void editor_scroll() {
  if (e_config.row_off > e_config.cursor_y) {
    e_config.row_off = e_config.cursor_y;
  }

  if (e_config.cursor_y >= e_config.row_off + e_config.term_rows) {
    e_config.row_off = e_config.cursor_y - e_config.term_rows + 1;
  }

  if (e_config.col_off > e_config.cursor_x) {
    e_config.col_off = e_config.cursor_x;
  }

  if (e_config.cursor_x >= e_config.col_off + e_config.term_cols) {
    e_config.col_off = e_config.cursor_x - e_config.term_cols + 1;
  }
}

void editor_draw_rows(abuf_t *ab) {
  for (int i = 0; i < e_config.term_rows; i++) {
    int r = i + e_config.row_off;
    if (r < e_config.rows_size) {
      int len = e_config.rows[r].rsize - e_config.col_off;
      if (len < 0)
        len = 0;
      if (len > e_config.term_cols)
        len = e_config.term_cols;
      ab_append(ab, &e_config.rows[r].rchars[e_config.col_off], len);
    } else {
      ab_append(ab, "~", 1);
    }
    ab_append(ab, ESC_CLEAR_LINE, strlen(ESC_CLEAR_LINE));
    ab_append(ab, "\r\n", 2);
  }
}

void editor_draw_status_bar(abuf_t *ab) {
  ab_append(ab, "\x1b[7m", 4);

  char status[80] = {0};
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
                     e_config.filename ? e_config.filename : "[No Name]",
                     e_config.rows_size);

  char rstatus[80] = {0};
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", e_config.cursor_y + 1,
                      e_config.rows_size);

  if (len > e_config.term_cols) {
    len = e_config.term_cols;
    return;
  }

  ab_append(ab, status, len);
  while (len < e_config.term_cols) {
    if (e_config.term_cols - len == rlen) {
      ab_append(ab, rstatus, rlen);
      break;
    } else {
      ab_append(ab, " ", 1);
      len++;
    }
  }

  ab_append(ab, "\x1b[m", 3);
  ab_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(abuf_t *ab) {
  ab_append(ab, ESC_CLEAR_LINE, strlen(ESC_CLEAR_LINE));
  int msg_len = strlen(e_config.status_msg);
  if (msg_len > e_config.term_cols)
    msg_len = e_config.term_cols;
  if (msg_len && time(NULL) - e_config.status_msg_time < 5)
    ab_append(ab, e_config.status_msg, msg_len);
}

void editor_set_status_message(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e_config.status_msg, sizeof(e_config.status_msg), fmt, ap);
  va_end(ap);
  e_config.status_msg_time = time(NULL);
}

void editor_reset_screen() {
  editor_scroll();
  abuf_t ab = ABUF_INIT;
  ab_append(&ab, ESC_CURSOR_HOME, strlen(ESC_CURSOR_HOME));
  ab_append(&ab, ESC_CURSOR_HIDE, strlen(ESC_CURSOR_HIDE));

  editor_draw_rows(&ab);
  editor_draw_status_bar(&ab);
  editor_draw_message_bar(&ab);

  char buf[32] = {0};

  snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
           e_config.cursor_y - e_config.row_off + 1,
           e_config.cursor_x - e_config.col_off + 1);

  ab_append(&ab, buf, strlen(buf));

  ab_append(&ab, ESC_CURSOR_SHOW, strlen(ESC_CURSOR_SHOW));

  write(STDOUT_FILENO, ab.buf, ab.size);
  ab_free(&ab);
}

int editor_read_keypress() {
  char c = 0;
  ssize_t n = 0;
  while ((n = read(STDIN_FILENO, &c, 1)) != 1) {
    if (n == -1 && errno != EAGAIN) {
      die("read");
    }
  }

  if (c == '\x1b') {
    char seq[3] = {0};

    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return c;
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return c;

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return c;
        if (seq[2] == '~') {
          switch (seq[1]) {
          case '1':
            return HOME_KEY;
          case '3':
            return DEL_KEY;
          case '4':
            return END_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return HOME_KEY;
          case '8':
            return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }
    return '\x1b';
  }

  return c;
}

void editor_move_cursor(int k) {
  editor_row_t *row = NULL;
  if (e_config.cursor_y < e_config.rows_size) {
    row = &e_config.rows[e_config.cursor_y];
  }
  switch (k) {
  case ARROW_LEFT:
    if (e_config.cursor_x != 0) {
      e_config.cursor_x--;
    } else if (e_config.cursor_y > 0) {
      e_config.cursor_y--;
      e_config.cursor_x = e_config.rows[e_config.cursor_y].rsize;
    }
    break;
  case ARROW_RIGHT:
    if (row && e_config.cursor_x < row->rsize) {
      e_config.cursor_x++;
    } else if (row && e_config.cursor_x == row->rsize) {
      e_config.cursor_y++;
      e_config.cursor_x = 0;
    }
    break;
  case ARROW_DOWN:
    if (e_config.cursor_y < e_config.rows_size) {
      e_config.cursor_y++;
    }
    break;
  case ARROW_UP:
    if (e_config.cursor_y > 0) {
      e_config.cursor_y--;
    }
    break;
  }
  if (e_config.cursor_y < e_config.rows_size) {
    row = &e_config.rows[e_config.cursor_y];
  }
  int row_size = (row) ? row->rsize : 0;
  if (e_config.cursor_x >= row_size) {
    e_config.cursor_x = row_size;
  }
}

void editor_process_keypress() {
  int k = editor_read_keypress();

  switch (k) {
  case CTRL_KEY('q'):
    esc_clear_screen();
    esc_cursor_home();
    exit(0);
    break;
  case HOME_KEY:
    e_config.cursor_x = 0;
    break;
  case END_KEY:
    int row_size = 0;
    if (e_config.cursor_y < e_config.rows_size) {
      row_size = e_config.rows[e_config.cursor_y].rsize;
    }
    e_config.cursor_x = row_size;
    break;
  case PAGE_UP:
  case PAGE_DOWN: {
    for (int i = 0; i < e_config.term_rows / 2; i++) {
      editor_move_cursor(k == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;
  }
  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editor_move_cursor(k);
    break;
  }
}

void init_editor() {
  e_config.cursor_x = 0;
  e_config.cursor_y = 0;
  e_config.row_off = 0;
  e_config.col_off = 0;
  e_config.rows_size = 0;
  e_config.rows_cap = 8;
  e_config.filename = NULL;
  e_config.status_msg[0] = '\0';
  e_config.status_msg_time = 0;
  e_config.rows = calloc(e_config.rows_cap, sizeof(editor_row_t));
  if (get_term_size(&e_config.term_rows, &e_config.term_cols) == -1) {
    die("get_term_size");
  }
  e_config.term_rows -= 2;
}

void editor_update_row(editor_row_t *row) {
  free(row->rchars);

  int j;
  int tabs = 0;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t')
      tabs++;

  row->rchars = malloc(row->size + tabs * (ZILO_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->rchars[idx++] = ' ';
      while (idx % 8 != 0)
        row->rchars[idx++] = ' ';
    } else {
      row->rchars[idx++] = row->chars[j];
    }
  }

  row->rchars[idx] = '\0';
  row->rsize = idx;
}

void editor_append_row(char *s, size_t len) {
  if (e_config.rows_cap <= e_config.rows_size) {
    e_config.rows_cap *= 2;
    e_config.rows =
        realloc(e_config.rows, sizeof(editor_row_t) * e_config.rows_cap);
    if (!e_config.rows) {
      die("realloc");
    }
  }
  int at = e_config.rows_size;
  e_config.rows[at].size = len;
  e_config.rows[at].chars = malloc(len + 1);
  memcpy(e_config.rows[at].chars, s, len);
  e_config.rows[at].chars[len] = '\0';

  e_config.rows[at].rsize = 0;
  e_config.rows[at].rchars = NULL;
  editor_update_row(&e_config.rows[at]);

  e_config.rows_size++;
}

void editor_open(char *filename) {
  FILE *fd = fopen(filename, "r");
  if (!fd) {
    die("fopen");
  }

  free(e_config.filename);
  e_config.filename = strdup(filename);

  char *line = NULL;
  size_t size = 0;
  ssize_t nread;

  while ((nread = getline(&line, &size, fd)) != -1) {
    while (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r'))
      nread--;
    if (nread > 0)
      editor_append_row(line, nread);
  }

  free(line);
  fclose(fd);
}

int main(int argc, char **argv) {
  enable_raw_mode();
  init_editor();

  if (argc >= 2) {
    editor_open(argv[1]);
  }

  editor_set_status_message("HELP: Ctrl-Q = quit");

  while (1) {
    editor_reset_screen();
    editor_process_keypress();
  }

  return 0;
}
