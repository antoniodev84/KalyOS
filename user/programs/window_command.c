#include <gui.h>
#include <kaly/system.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
static char *word(char **line) {
    char *s = *line;
    while (*s == ' ') {
        s++;
    }
    char *start = s;
    while (*s && *s != ' ') {
        s++;
    }
    if (*s) {
        *s++ = 0;
    }
    *line = s;
    return start;
}
static int number(char **line, long *value) {
    char *s = word(line), *end;
    if (!*s) {
        return -1;
    }
    *value = strtol(s, &end, 0);
    return *end ? -1 : 0;
}
static void help(void) {
    puts("create x y w h title | destroy id | focus id | title id title");
    puts("move id x y | resize id w h | fill id x y w h rgb");
    puts("line id x y end_x end_y rgb | text id x y text | read id x y | quit");
}
static int execute(struct gui *g, char *line, uint64_t owned[8]) {
    char *op = word(&line);
    if (!strcmp(op, "quit")) {
        return 1;
    }
    if (!strcmp(op, "help")) {
        help();
        return 0;
    }
    struct kwm_request r = {0};
    long values[6] = {0};
    int n = 0;
    if (!strcmp(op, "create")) {
        r.operation = KWM_CREATE;
        n = 4;
    } else if (!strcmp(op, "destroy")) {
        r.operation = KWM_DESTROY;
    } else if (!strcmp(op, "focus")) {
        r.operation = KWM_FOCUS;
    } else if (!strcmp(op, "title")) {
        r.operation = KWM_TITLE;
    } else if (!strcmp(op, "move")) {
        r.operation = KWM_MOVE;
        n = 2;
    } else if (!strcmp(op, "resize")) {
        r.operation = KWM_RESIZE;
        n = 2;
    } else if (!strcmp(op, "fill")) {
        r.operation = KWM_FILL;
        n = 5;
    } else if (!strcmp(op, "line")) {
        r.operation = KWM_LINE;
        n = 5;
    } else if (!strcmp(op, "text")) {
        r.operation = KWM_TEXT;
        n = 2;
    } else if (!strcmp(op, "read")) {
        r.operation = KWM_READ;
        n = 2;
    } else {
        help();
        return 0;
    }
    long id = 0;
    if (r.operation != KWM_CREATE && (number(&line, &id) || id <= 0)) {
        puts("Invalid window ID");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (number(&line, &values[i]) || values[i] < -4096 ||
            values[i] > (i == 4 ? 0xffffff : 4096)) {
            puts("Invalid coordinates or color (use 0xRRGGBB)");
            return 0;
        }
    }
    while (*line == ' ') {
        line++;
    }
    r.x = values[0];
    r.y = values[1];
    r.width = values[2];
    r.height = values[3];
    r.color = values[4];
    if (r.operation == KWM_RESIZE) {
        r.width = values[0];
        r.height = values[1];
    }
    if (r.operation == KWM_CREATE || r.operation == KWM_TITLE) {
        strncpy(r.title, line, sizeof(r.title) - 1);
    }
    if (r.operation == KWM_TEXT) {
        r.data = (uintptr_t)line;
        r.length = strlen(line);
        r.color = 0xe4f1ff;
    }
    uint32_t pixel;
    if (r.operation == KWM_READ) {
        r.width = r.height = 1;
        r.argument = 1;
        r.data = (uintptr_t)&pixel;
        r.length = 4;
    }
    g->window = id;
    if (gui_request(g, &r)) {
        perror(op);
        return 0;
    }
    if (r.operation == KWM_CREATE) {
        for (unsigned i = 0; i < 8; i++) {
            if (!owned[i]) {
                owned[i] = r.window;
                break;
            }
        }
        printf("window=%lu owner=%d\n", r.window, getpid());
    } else if (r.operation == KWM_DESTROY) {
        for (unsigned i = 0; i < 8; i++) {
            if (owned[i] == r.window) {
                owned[i] = 0;
            }
        }
    } else if (r.operation == KWM_READ) {
        printf("pixel=0x%06x\n", pixel);
    } else {
        puts("OK");
    }
    return 0;
}
int window_command(const char *command, int argc, char **argv) {
    if (!strcmp(command, "init")) {
        int fd = open("/dev/system", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            perror("init request");
            return 1;
        }
        struct ksystem_request r = {.version = KSYSTEM_VERSION, .operation = KSYSTEM_START_GUI};
        int result = ioctl(fd, KSYSTEM_IOCTL, &r);
        if (result) {
            perror("init request");
        }
        close(fd);
        return result ? 1 : 0;
    }
    if (strcmp(command, "window")) {
        return -1;
    }
    struct gui g = {.fd = open("/dev/wm", O_RDWR | O_CLOEXEC)};
    if (g.fd < 0) {
        perror("/dev/wm");
        return 1;
    }
    struct kwm_request r = {.operation = KWM_INFO};
    if (gui_request(&g, &r)) {
        perror("window info");
        close(g.fd);
        return 1;
    }
    printf("WM %dx%d PSF %dx%d IPC ABI %u\n", r.width, r.height, r.x, r.y, KWM_VERSION);
    if (argc > 1 && !strcmp(argv[1], "info")) {
        close(g.fd);
        return 0;
    }
    int flags = fcntl(0, F_GETFL);
    if (flags < 0 || fcntl(0, F_SETFL, flags | O_NONBLOCK)) {
        perror("window stdin");
        close(g.fd);
        return 1;
    }
    uint64_t owned[8] = {0};
    char line[1024];
    size_t length = 0;
    int running = 1, result = 0;
    help();
    printf("wm> ");
    while (running) {
        for (unsigned i = 0; i < 8; i++) {
            if (!owned[i]) {
                continue;
            }
            g.window = owned[i];
            struct kwm_event e;
            for (unsigned j = 0; j < 128 && !gui_event(&g, &e); j++) {
                if (e.type == KWM_CLOSE) {
                    r = (struct kwm_request){.operation = KWM_DESTROY};
                    if (gui_request(&g, &r)) {
                        perror("window close");
                    }
                    owned[i] = 0;
                    break;
                }
            }
        }
        char c;
        ssize_t n = read(0, &c, 1);
        if (n == 1) {
            if (c == '\n') {
                putchar('\n');
                line[length] = 0;
                running = !execute(&g, line, owned);
                length = 0;
                if (running) {
                    printf("wm> ");
                }
            } else if (c == '\b' || c == 127) {
                if (length) {
                    length--;
                    printf("\b \b");
                }
            } else if (c >= 32 && c < 127 && length + 1 < sizeof(line)) {
                line[length++] = c;
                putchar(c);
            }
        } else if (!n || errno != EAGAIN) {
            if (n < 0) {
                perror("window read");
                result = 1;
            }
            running = 0;
        } else {
            gui_pause();
        }
    }
    for (unsigned i = 0; i < 8; i++) {
        if (owned[i]) {
            g.window = owned[i];
            r = (struct kwm_request){.operation = KWM_DESTROY};
            if (gui_request(&g, &r)) {
                perror("window destroy");
                result = 1;
            }
        }
    }
    if (fcntl(0, F_SETFL, flags)) {
        perror("restore stdin flags");
        result = 1;
    }
    close(g.fd);
    return result;
}
