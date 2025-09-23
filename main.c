#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** data ***/

struct termios orig_term;

/*** utils ***/

void die(const char *s) {
    perror(s);
    exit(1);
}

/*** terminal ***/

void disenable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term) == -1) {
        die("tcsetattr");
    }
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &orig_term) == -1) {
        die("tcgetattr");
    }

    atexit(disenable_raw_mode);

    struct termios term = orig_term;

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

/*** init ***/

int main() {
    enable_raw_mode();

    while (1) {
        char c = '\0';
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == -1 && errno != EAGAIN) {
            die("read");
        }

        if (c == '\0') {
            continue;
        } else if (!iscntrl(c)) {
            printf("%d ('%c')\r\n", c, c);
        } else {
            printf("%d\r\n", c);
        }

        if (c == 'q')
            break;
    }

    return 0;
}
