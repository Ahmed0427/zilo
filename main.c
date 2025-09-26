#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CTRL_KEY(c) (c & 0x1F)

#define ESC_CLEAR_SCREEN "\x1b[2J"
#define ESC_CURSOR_HOME "\x1b[H"
#define ESC_MOVE_BOTTOM_RIGHT "\x1b[999C\x1b[999B"
#define ESC_CURSOR_POS_REQ "\x1b[6n"

struct editor_config_t {
    int term_rows, term_cols;
    struct termios orig_term;
};

struct editor_config_t e_config;

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

char editor_read_keypress() {
    char c = 0;
    ssize_t n = 0;
    while ((n = read(STDIN_FILENO, &c, 1)) != 1) {
        if (n == -1 && errno != EAGAIN) {
            die("read");
        }
    }
    return c;
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
    if (buf[0] != 0x1B || buf[1] != '[')
        return -1;

    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

void editor_draw_rows() {
    for (int i = 0; i < e_config.term_rows; i++) {
        write(STDOUT_FILENO, "~", 1);
        if (i != e_config.term_rows - 1) {
            write(STDOUT_FILENO, "\r\n", 2);
        }
    }
}

void editor_reset_screen() {
    esc_clear_screen();
    esc_cursor_home();
    editor_draw_rows();
    esc_cursor_home();
}

void editor_process_keypress() {
    char c = editor_read_keypress();

    switch (c) {
    case CTRL_KEY('q'):
        esc_clear_screen();
        esc_cursor_home();
        exit(0);
        break;
    }
}

void init_editor() {
    if (get_term_size(&e_config.term_rows, &e_config.term_cols) == -1) {
        die("get_term_size");
    }
}

int main() {
    enable_raw_mode();
    init_editor();

    while (1) {
        editor_reset_screen();
        editor_process_keypress();
    }

    return 0;
}
