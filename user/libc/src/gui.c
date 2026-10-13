#include <gui.h>
#include <kaly/theme.h>
#include <kaly/system.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
extern const uint8_t gui_font[], gui_font_end[];
static const uint8_t *glyphs;
static unsigned glyph_count, glyph_size, fw, fh;
static uint16_t mapping[65536];
static int initialized;
static void font_init(void) {
    if (initialized) {
        return;
    }
    initialized = 1;
    int first = gui_font[0] == 0x36 && gui_font[1] == 4, unicode;
    if (first) {
        fw = 8;
        fh = glyph_size = gui_font[3];
        glyph_count = gui_font[2] & 1 ? 512 : 256;
        unicode = gui_font[2] & 2;
        glyphs = gui_font + 4;
    } else {
        const uint32_t *header = (const void *)gui_font;
        fw = header[7];
        fh = header[6];
        glyph_size = header[5];
        glyph_count = header[4];
        unicode = header[3] & 1;
        glyphs = gui_font + header[2];
    }
    memset(mapping, 255, sizeof(mapping));
    if (!unicode) {
        for (unsigned i = 0; i < glyph_count && i < 65536; i++) {
            mapping[i] = i;
        }
        return;
    }
    const uint8_t *p = glyphs + glyph_count * glyph_size;
    unsigned index = 0;
    int sequence = 0;
    while (p < gui_font_end && index < glyph_count) {
        unsigned code;
        if (first) {
            if (gui_font_end - p < 2) {
                break;
            }
            code = p[0] | ((unsigned)p[1] << 8);
            p += 2;
        } else {
            code = *p++;
        }
        if (code == (first ? 0xffffU : 0xffU)) {
            index++;
            sequence = 0;
            continue;
        }
        if (code == (first ? 0xfffeU : 0xfeU)) {
            sequence = 1;
            continue;
        }
        if (!first && code >= 0xc2 && code <= 0xf4) {
            unsigned n = code < 0xe0 ? 1 : code < 0xf0 ? 2 : 3;
            code &= n == 1 ? 31 : n == 2 ? 15 : 7;
            if ((unsigned)(gui_font_end - p) < n) {
                break;
            }
            while (n--) {
                code = (code << 6) | (*p++ & 63);
            }
        }
        if (!sequence && code < 65536) {
            mapping[code] = index;
        }
    }
}
int gui_request(struct gui *g, struct kwm_request *r) {
    r->version = KWM_VERSION;
    r->window = g->window;
    return ioctl(g->fd, KWM_IOCTL, r);
}
int gui_resize_buffer(struct gui *g, int width, int height) {
    if (width < 1 || height < 1 || width > 1920 || height > 1200) {
        errno = EINVAL;
        return -1;
    }
    uint32_t *p = calloc((size_t)width * height, 4);
    if (!p) {
        return -1;
    }
    int w = width < g->width ? width : g->width;
    int h = height < g->height ? height : g->height;
    for (int j = 0; j < h; j++) {
        memcpy(p + j * width, g->pixels + j * g->width, (size_t)w * 4);
    }
    free(g->pixels);
    g->pixels = p;
    g->width = width;
    g->height = height;
    return 0;
}
int gui_open(struct gui *g, const char *title, int x, int y, int width, int height) {
    memset(g, 0, sizeof(*g));
    g->fd = open("/dev/wm", O_RDWR | O_CLOEXEC);
    if (g->fd < 0) {
        return -1;
    }
    struct kwm_request r = {.operation = KWM_INFO};
    if (gui_request(g, &r)) {
        close(g->fd);
        g->fd = -1;
        return -1;
    }
    font_init();
    g->font_width = fw;
    g->font_height = fh;
    if (width > r.width - 4) {
        width = r.width - 4;
    }
    if (height > r.height - 68) {
        height = r.height - 68;
    }
    if (gui_resize_buffer(g, width, height)) {
        close(g->fd);
        g->fd = -1;
        return -1;
    }
    r = (struct kwm_request){
        .operation = KWM_CREATE, .x = x, .y = y, .width = width, .height = height};
    strncpy(r.title, title, sizeof(r.title) - 1);
    if (gui_request(g, &r)) {
        free(g->pixels);
        close(g->fd);
        g->fd = -1;
        return -1;
    }
    g->window = r.window;
    return 0;
}
void gui_close(struct gui *g) {
    if (g->fd >= 0) {
        struct kwm_request r = {.operation = KWM_DESTROY};
        gui_request(g, &r);
        close(g->fd);
        g->fd = -1;
    }
    free(g->pixels);
    g->pixels = NULL;
}
int gui_event(struct gui *g, struct kwm_event *e) {
    struct kwm_request r = {.operation = KWM_EVENT, .data = (uintptr_t)e};
    for (;;) {
        if (g->has_pending_key) {
            g->has_pending_key = 0;
            *e = (struct kwm_event){.type = KWM_KEY, .code = g->pending_key};
            return 0;
        }
        if (gui_request(g, &r)) {
            if (errno == EAGAIN && g->escape && ++g->escape_wait >= 3) {
                g->escape = g->escape_wait = 0;
                *e = (struct kwm_event){.type = KWM_KEY, .code = 27};
                return 0;
            }
            return -1;
        }
        if (e->type == KWM_OVERFLOW && e->width > 0 && e->height > 0 &&
            (g->width != e->width || g->height != e->height) &&
            gui_resize_buffer(g, e->width, e->height)) {
            return -1;
        }
        if (e->type != KWM_KEY) {
            return 0;
        }
        if (g->escape == 1) {
            g->escape = e->code == '[' ? 2 : 0;
            if (g->escape) {
                continue;
            }
            g->pending_key = e->code;
            g->has_pending_key = 1;
            e->code = 27;
            return 0;
        } else if (g->escape == 2) {
            g->escape = 0;
            unsigned key = e->code;
            e->code = key == 'A'   ? KWM_UP
                      : key == 'B' ? KWM_DOWN
                      : key == 'C' ? KWM_RIGHT
                      : key == 'D' ? KWM_LEFT
                                   : key;
        } else if (e->code == 27) {
            g->escape = 1;
            g->escape_wait = 0;
            continue;
        }
        return 0;
    }
}
int gui_present(struct gui *g) {
    struct kwm_request r = {.operation = KWM_BLIT,
                            .width = g->width,
                            .height = g->height,
                            .data = (uintptr_t)g->pixels,
                            .length = (size_t)g->width * g->height * 4,
                            .argument = g->width};
    return gui_request(g, &r);
}
int gui_read_pixels(struct gui *g, int x, int y, int width, int height, uint32_t *p) {
    struct kwm_request r = {.operation = KWM_READ,
                            .x = x,
                            .y = y,
                            .width = width,
                            .height = height,
                            .data = (uintptr_t)p,
                            .length = (size_t)width * height * 4,
                            .argument = width};
    return gui_request(g, &r);
}
int gui_move(struct gui *g, int x, int y) {
    struct kwm_request r = {.operation = KWM_MOVE, .x = x, .y = y};
    return gui_request(g, &r);
}
int gui_resize(struct gui *g, int width, int height) {
    struct kwm_request r = {.operation = KWM_RESIZE, .width = width, .height = height};
    return gui_request(g, &r);
}
int gui_title(struct gui *g, const char *title) {
    struct kwm_request r = {.operation = KWM_TITLE};
    strncpy(r.title, title, sizeof(r.title) - 1);
    return gui_request(g, &r);
}
void gui_rect(struct gui *g, int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0 || x < -4096 || y < -4096 || x > 4096 || y > 4096 || w > 4096 ||
        h > 4096) {
        return;
    }
    int right = x + w < g->width ? x + w : g->width;
    int bottom = y + h < g->height ? y + h : g->height;
    for (int j = y < 0 ? 0 : y; j < bottom; j++) {
        for (int i = x < 0 ? 0 : x; i < right; i++) {
            g->pixels[j * g->width + i] = color;
        }
    }
}
void gui_line(struct gui *g, int x, int y, int end_x, int end_y, uint32_t color) {
    if (x < -4096 || x > 4096 || y < -4096 || y > 4096 || end_x < -4096 || end_x > 4096 ||
        end_y < -4096 || end_y > 4096) {
        return;
    }
    int dx = end_x > x ? end_x - x : x - end_x;
    int dy = end_y > y ? y - end_y : end_y - y;
    int sx = x < end_x ? 1 : -1, sy = y < end_y ? 1 : -1, error = dx + dy;
    for (;;) {
        gui_rect(g, x, y, 1, 1, color);
        if (x == end_x && y == end_y) {
            break;
        }
        int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x += sx;
        }
        if (twice <= dx) {
            error += dx;
            y += sy;
        }
    }
}
void gui_glyph(struct gui *g, int x, int y, unsigned code, uint32_t color) {
    unsigned index = code < 65536 ? mapping[code] : 0xffff;
    if (index == 0xffff || index >= glyph_count) {
        index = mapping['?'];
    }
    if (index == 0xffff || index >= glyph_count) {
        index = 0;
    }
    const uint8_t *p = glyphs + index * glyph_size;
    for (unsigned j = 0; j < fh; j++) {
        for (unsigned i = 0; i < fw; i++) {
            if (p[j * ((fw + 7) / 8) + i / 8] & (128 >> (i % 8))) {
                gui_rect(g, x + i, y + j, 1, 1, color);
            }
        }
    }
}
void gui_text(struct gui *g, int x, int y, const char *s, uint32_t color) {
    int start = x;
    while (*s) {
        unsigned code = (uint8_t)*s++;
        if (code == '\n') {
            x = start;
            y += fh;
            continue;
        }
        if (code >= 0xc2 && code <= 0xf4) {
            unsigned n = code < 0xe0 ? 1 : code < 0xf0 ? 2 : 3;
            code &= n == 1 ? 31 : n == 2 ? 15 : 7;
            while (n-- && ((uint8_t)*s & 0xc0) == 0x80) {
                code = (code << 6) | ((uint8_t)*s++ & 63);
            }
        }
        gui_glyph(g, x, y, code, color);
        x += fw;
    }
}
void gui_button(struct gui *g, int x, int y, int width, const char *label, int active) {
    gui_rect(g, x, y, width, 26, active ? THEME_BUTTON_BG_ACTIVE : THEME_BUTTON_BG);
    gui_text(g, x + 6, y + 5, label, THEME_BUTTON_TEXT);
}
void gui_pause(void) {
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
}
int gui_launch(const char *program, const char *argument) {
    int pid = fork();
    if (!pid) {
        char *argv[] = {(char *)program, (char *)argument, NULL};
        execve(program, argv, environ);
        _exit(127);
    }
    return pid;
}

int gui_reserve_pid(unsigned pid) {
    int fd = open("/dev/system", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct ksystem_request r = {
        .version = KSYSTEM_VERSION, .operation = KSYSTEM_RESERVE_PID, .pid = pid};
    int result = ioctl(fd, KSYSTEM_IOCTL, &r), saved = errno;
    close(fd);
    errno = saved;
    return result;
}
int gui_launch_system(const char *program, const char *argument, unsigned pid) {
    if (gui_reserve_pid(pid)) {
        return -1;
    }
    return gui_launch(program, argument);
}
