#include <gui.h>
#include <kaly/theme.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#define SCROLL_LINES 512
#define LINE_COLUMNS 240
static unsigned cells[SCROLL_LINES][LINE_COLUMNS];
static unsigned line, column, view;
static unsigned utf_code, utf_left, utf_min;
static int output_escape;
static void put_code(unsigned c, unsigned columns) {
    if (c == '\n') {
        line++;
        column = 0;
        memset(cells[line % SCROLL_LINES], 0, sizeof(cells[0]));
        view = 0;
    } else if (c == '\r') {
        column = 0;
    } else if (c == '\b') {
        if (column) {
            cells[line % SCROLL_LINES][--column] = 0;
        }
    } else if (c == '\t') {
        unsigned n = 8 - column % 8;
        while (n--) {
            put_code(' ', columns);
        }
    } else if (c >= 32 && c != 127) {
        if (column >= columns) {
            put_code('\n', columns);
        }
        cells[line % SCROLL_LINES][column++] = c;
        view = 0;
    }
}
static void output(unsigned c, unsigned columns) {
    if (output_escape) {
        if (output_escape == 1 && c == '[') {
            output_escape = 2;
        } else if (output_escape == 2 && (c < 0x40 || c > 0x7e)) {
            return;
        } else {
            output_escape = 0;
        }
        return;
    }
    if (c == 27) {
        output_escape = 1;
        return;
    }
    if (utf_left) {
        if ((c & 0xc0) == 0x80) {
            utf_code = (utf_code << 6) | (c & 63);
            if (--utf_left) {
                return;
            }
            c = utf_code;
            if (c < utf_min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) {
                c = '?';
            }
        } else {
            utf_left = 0;
            put_code('?', columns);
        }
    } else if (c >= 0xc2 && c <= 0xf4) {
        utf_left = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        utf_min = utf_left == 1 ? 0x80 : utf_left == 2 ? 0x800 : 0x10000;
        utf_code = c & (utf_left == 1 ? 31 : utf_left == 2 ? 15 : 7);
        return;
    } else if (c >= 128) {
        c = '?';
    }
    put_code(c, columns);
}
static int draw(struct gui *g, int exited) {
    gui_rect(g, 0, 0, g->width, g->height, THEME_TERM_BG);
    unsigned rows = (g->height - 30) / g->font_height;
    if (!rows) {
        rows = 1;
    }
    unsigned columns = (g->width - 16) / g->font_width;
    if (columns > LINE_COLUMNS) {
        columns = LINE_COLUMNS;
    }
    unsigned end = line >= view ? line - view : 0;
    unsigned first = end >= rows - 1 ? end - rows + 1 : 0;
    if (line >= SCROLL_LINES && first <= line - SCROLL_LINES) {
        first = line - SCROLL_LINES + 1;
    }
    for (unsigned j = first; j <= end; j++) {
        for (unsigned i = 0; i < columns; i++) {
            unsigned code = cells[j % SCROLL_LINES][i];
            if (code) {
                gui_glyph(g, 8 + i * g->font_width, 7 + (j - first) * g->font_height, code,
                          THEME_TERM_FG);
            }
        }
    }
    if (!view && !exited) {
        gui_rect(g, 8 + column * g->font_width, 7 + (line - first) * g->font_height, g->font_width,
                 g->font_height, THEME_TERM_CURSOR);
    }
    gui_rect(g, 0, g->height - 23, g->width, 23, THEME_TERM_STATUS_BG);
    gui_text(g, 8, g->height - 20,
             exited ? "Process exited - close this window" : "KalyBox / sh | mouse wheel: scroll",
             THEME_TERM_STATUS_FG);
    return gui_present(g);
}
static void execute_program(char *const args[]) {
    execve(args[0], args, environ);
    if (errno != 8) {
        return;
    }
    int fd = open(args[0], O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    char header[512];
    ssize_t n = read(fd, header, sizeof(header) - 1);
    int saved = errno;
    close(fd);
    if (n < 0) {
        errno = saved;
        return;
    }
    if (n < 2 || header[0] != '#' || header[1] != '!') {
        errno = 8;
        return;
    }
    header[n] = 0;
    char *end = strchr(header, '\n');
    if (!end && n == (ssize_t)sizeof(header) - 1) {
        errno = 8;
        return;
    }
    if (end) {
        *end = 0;
    }
    char *interpreter = header + 2;
    while (*interpreter == ' ' || *interpreter == '\t') {
        interpreter++;
    }
    char *option = interpreter;
    while (*option && *option != ' ' && *option != '\t' && *option != '\r') {
        option++;
    }
    if (option == interpreter) {
        errno = 8;
        return;
    }
    if (*option) {
        *option++ = 0;
        while (*option == ' ' || *option == '\t') {
            option++;
        }
        char *tail = option + strlen(option);
        while (tail > option && (tail[-1] == ' ' || tail[-1] == '\t' || tail[-1] == '\r')) {
            *--tail = 0;
        }
    }
    char *script[65];
    unsigned count = 0;
    script[count++] = interpreter;
    if (*option) {
        script[count++] = option;
    }
    for (unsigned i = 0; args[i]; i++) {
        if (count == 64) {
            errno = 7;
            return;
        }
        script[count++] = args[i];
    }
    script[count] = NULL;
    execve(interpreter, script, environ);
}
int main(int argc, char **argv) {
    struct gui g;
    if (gui_open(&g, "KalyBox terminal", 36, 118, 600, 370)) {
        perror("terminal window");
        return 1;
    }
    if (argc > 1) {
        const char *name = strrchr(argv[1], '/');
        gui_title(&g, name ? name + 1 : argv[1]);
    }
    int input[2], output_pipe[2];
    if (pipe(input)) {
        perror("terminal pipe");
        gui_close(&g);
        return 1;
    }
    if (pipe(output_pipe)) {
        close(input[0]);
        close(input[1]);
        gui_close(&g);
        return 1;
    }
    if (getpid() == 4 && gui_reserve_pid(5)) {
        perror("terminal system shell");
        close(input[0]);
        close(input[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        gui_close(&g);
        return 1;
    }
    int child = fork();
    if (!child) {
        if (dup2(input[0], 0) < 0 || dup2(output_pipe[1], 1) < 0 || dup2(output_pipe[1], 2) < 0) {
            _exit(126);
        }
        close(input[0]);
        close(input[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        close(g.fd);
        if (argc > 1) {
            execute_program(argv + 1);
            perror(argv[1]);
        } else {
            char *shell[] = {"/bin/sh", NULL};
            execve(shell[0], shell, environ);
            perror(shell[0]);
        }
        _exit(127);
    }
    close(input[0]);
    close(output_pipe[1]);
    if (child < 0 || fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) ||
        fcntl(input[1], F_SETFL, O_NONBLOCK)) {
        perror("terminal setup");
        close(input[1]);
        close(output_pipe[0]);
        gui_close(&g);
        return 1;
    }
    printf("Terminal: window %lu owner PID %d shell PID %d\n", g.window, getpid(), child);
    int changed = 1, running = 1, exited = 0;
    char pending[4096];
    size_t pending_count = 0;
    while (running) {
        struct kwm_event e;
        unsigned processed = 0;
        while (processed++ < 128 && !gui_event(&g, &e)) {
            if (e.type == KWM_CLOSE) {
                running = 0;
            } else if (e.type == KWM_SIZE) {
                if (gui_resize_buffer(&g, e.width, e.height)) {
                    perror("terminal resize");
                    running = 0;
                }
                changed = 1;
            } else if (e.type == KWM_KEY && !exited) {
                char sequence[3];
                size_t n = 1;
                sequence[0] = e.code;
                if (e.code >= KWM_UP && e.code <= KWM_RIGHT) {
                    sequence[0] = 27;
                    sequence[1] = '[';
                    sequence[2] = e.code == KWM_UP      ? 'A'
                                  : e.code == KWM_DOWN  ? 'B'
                                  : e.code == KWM_RIGHT ? 'C'
                                                        : 'D';
                    n = 3;
                }
                if (n <= sizeof(pending) - pending_count) {
                    memcpy(pending + pending_count, sequence, n);
                    pending_count += n;
                } else {
                    const char *message = "\nInput buffer full\n";
                    while (*message) {
                        output(*message++, (g.width - 16) / g.font_width);
                    }
                    changed = 1;
                }
            } else if (e.type == KWM_WHEEL) {
                int next = (int)view - (int)e.code * 3;
                unsigned maximum = line < SCROLL_LINES - 1 ? line : SCROLL_LINES - 1;
                view = next < 0 ? 0 : (unsigned)next > maximum ? maximum : (unsigned)next;
                changed = 1;
            } else if (e.type == KWM_OVERFLOW) {
                pending_count = 0;
                const char *message = "\nInput event overflow; type command again\n";
                while (*message) {
                    output(*message++, (g.width - 16) / g.font_width);
                }
                changed = 1;
            }
        }
        if (pending_count && !exited) {
            ssize_t n = write(input[1], pending, pending_count);
            if (n > 0) {
                pending_count -= n;
                memmove(pending, pending + n, pending_count);
            } else if (n < 0 && errno != EAGAIN) {
                exited = 1;
                changed = 1;
            }
        }
        char buffer[2048];
        for (unsigned i = 0; i < 8; i++) {
            ssize_t n = read(output_pipe[0], buffer, sizeof(buffer));
            if (n <= 0) {
                if ((!n || errno != EAGAIN) && !exited) {
                    exited = 1;
                    changed = 1;
                }
                break;
            }
            unsigned columns = (g.width - 16) / g.font_width;
            if (columns > LINE_COLUMNS) {
                columns = LINE_COLUMNS;
            }
            for (ssize_t j = 0; j < n; j++) {
                output((uint8_t)buffer[j], columns);
            }
            changed = 1;
        }
        if (waitpid(child, NULL, WNOHANG) == child && !exited) {
            exited = 1;
            changed = 1;
        }
        if (changed) {
            if (draw(&g, exited)) {
                perror("terminal present");
                running = 0;
            }
            changed = 0;
        }
        gui_pause();
    }
    close(input[1]);
    close(output_pipe[0]);
    gui_close(&g);
    return 0;
}
