#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
static KGFX_Window *windows;
static char error_text[160];
int kgfx_error(const char *action) {
    snprintf(error_text, sizeof(error_text), "%s: %s", action, strerror(errno));
    return -1;
}
const char *KGFX_GetError(void) {
    return error_text;
}
int KGFX_Init(void) {
    struct gui probe = {.fd = open("/dev/wm", O_RDWR | O_CLOEXEC)};
    if (probe.fd < 0) {
        return kgfx_error("open /dev/wm");
    }
    struct kwm_request request = {.operation = KWM_INFO};
    int result = gui_request(&probe, &request);
    close(probe.fd);
    return result ? kgfx_error("WM information") : 0;
}
void KGFX_Quit(void) {
    while (windows) {
        KGFX_DestroyWindow(windows);
    }
}
static void surface_update(KGFX_Window *window) {
    window->surface = (KGFX_Surface){window->native.width, window->native.height,
                                     window->native.width * 4, window->native.pixels};
}
KGFX_Window *KGFX_CreateWindow(const char *title, int width, int height) {
    KGFX_Window *window = calloc(1, sizeof(*window));
    if (!window) {
        kgfx_error("window allocation");
        return NULL;
    }
    if (gui_open(&window->native, title, 72, 90, width, height)) {
        kgfx_error("create window (start the desktop with init)");
        free(window);
        return NULL;
    }
    surface_update(window);
    window->next = windows;
    windows = window;
    return window;
}
void KGFX_DestroyWindow(KGFX_Window *window) {
    if (!window) {
        return;
    }
    if (window->context && window->delete_context) {
        window->delete_context(window->context);
    }
    KGFX_Window **p = &windows;
    while (*p && *p != window) {
        p = &(*p)->next;
    }
    if (*p) {
        *p = window->next;
    }
    gui_close(&window->native);
    free(window);
}
void KGFX_GetWindowSize(KGFX_Window *w, int *width, int *height) {
    if (width) {
        *width = w->native.width;
    }
    if (height) {
        *height = w->native.height;
    }
}
int KGFX_SetWindowSize(KGFX_Window *w, int width, int height) {
    return gui_resize(&w->native, width, height) ? kgfx_error("resize window") : 0;
}
int KGFX_SetWindowPosition(KGFX_Window *w, int x, int y) {
    return gui_move(&w->native, x, y) ? kgfx_error("move window") : 0;
}
int KGFX_SetWindowTitle(KGFX_Window *w, const char *title) {
    return gui_title(&w->native, title) ? kgfx_error("window title") : 0;
}
int KGFX_PollEvent(KGFX_Window *window, KGFX_Event *event) {
    struct kwm_event input;
    if (gui_event(&window->native, &input)) {
        return errno == EAGAIN ? 0 : kgfx_error("window event");
    }
    if (input.type == KWM_SIZE) {
        if (gui_resize_buffer(&window->native, input.width, input.height)) {
            return kgfx_error("resize surface");
        }
    }
    int resized = window->surface.width != window->native.width ||
                  window->surface.height != window->native.height;
    if (resized || window->surface.pixels != window->native.pixels) {
        surface_update(window);
        if (resized && window->context && window->resize_context &&
            window->resize_context(window->context, window->native.width, window->native.height)) {
            return kgfx_error("resize GL buffers");
        }
    }
    *event = (KGFX_Event){.key = input.code,
                          .x = input.x,
                          .y = input.y,
                          .width = window->native.width,
                          .height = window->native.height,
                          .wheel = (int)input.code,
                          .buttons = input.buttons,
                          .timestamp = KGFX_GetTicks()};
    switch (input.type) {
    case KWM_CLOSE:
        event->type = KGFX_QUIT;
        break;
    case KWM_KEY:
        event->type = KGFX_KEYDOWN;
        break;
    case KWM_MOUSE:
        event->type = KGFX_MOUSE;
        break;
    case KWM_WHEEL:
        event->type = KGFX_MOUSEWHEEL;
        break;
    case KWM_SIZE:
        event->type = KGFX_RESIZED;
        break;
    case KWM_ACTIVATE:
        event->type = KGFX_FOCUS;
        break;
    default:
        event->type = KGFX_OVERFLOW;
        break;
    }
    return 1;
}
KGFX_Surface *KGFX_GetWindowSurface(KGFX_Window *window) {
    return &window->surface;
}
KGFX_Surface *KGFX_CreateSurface(int width, int height) {
    if (width < 1 || height < 1 || width > 4096 || height > 4096) {
        errno = EINVAL;
        kgfx_error("surface dimensions");
        return NULL;
    }
    KGFX_Surface *surface = malloc(sizeof(*surface));
    if (!surface) {
        kgfx_error("surface allocation");
        return NULL;
    }
    surface->pixels = calloc((size_t)width * height, 4);
    if (!surface->pixels) {
        free(surface);
        kgfx_error("pixel allocation");
        return NULL;
    }
    surface->width = width;
    surface->height = height;
    surface->pitch = width * 4;
    return surface;
}
void KGFX_FreeSurface(KGFX_Surface *s) {
    if (s) {
        free(s->pixels);
        free(s);
    }
}
int KGFX_FillRect(KGFX_Surface *s, const KGFX_Rect *rect, uint32_t color) {
    KGFX_Rect r = rect ? *rect : (KGFX_Rect){0, 0, s->width, s->height};
    if (r.x < -4096 || r.y < -4096 || r.x > 4096 || r.y > 4096 || r.width < 0 || r.height < 0 ||
        r.width > 4096 || r.height > 4096) {
        errno = EINVAL;
        return kgfx_error("fill rectangle");
    }
    int right = r.x + r.width < s->width ? r.x + r.width : s->width;
    int bottom = r.y + r.height < s->height ? r.y + r.height : s->height;
    for (int y = r.y < 0 ? 0 : r.y; y < bottom; y++) {
        uint32_t *row = (void *)((unsigned char *)s->pixels + (size_t)y * s->pitch);
        for (int x = r.x < 0 ? 0 : r.x; x < right; x++) {
            row[x] = color;
        }
    }
    return 0;
}
int KGFX_BlitSurface(const KGFX_Surface *source, const KGFX_Rect *rect, KGFX_Surface *dest, int x,
                     int y) {
    KGFX_Rect r = rect ? *rect : (KGFX_Rect){0, 0, source->width, source->height};
    if (r.x < 0 || r.y < 0 || r.width < 0 || r.height < 0 || r.width > source->width ||
        r.height > source->height || r.x > source->width - r.width ||
        r.y > source->height - r.height || x < -4096 || y < -4096 || x > 4096 || y > 4096) {
        errno = EINVAL;
        return kgfx_error("blit rectangle");
    }
    uint32_t *copy = malloc((size_t)r.width * r.height * 4);
    if (!copy && r.width && r.height) {
        return kgfx_error("blit allocation");
    }
    for (int j = 0; j < r.height; j++) {
        memcpy(copy + j * r.width,
               (const unsigned char *)source->pixels + (size_t)(r.y + j) * source->pitch + r.x * 4,
               r.width * 4);
    }
    for (int j = 0; j < r.height; j++) {
        if (y + j < 0 || y + j >= dest->height) {
            continue;
        }
        uint32_t *row = (void *)((unsigned char *)dest->pixels + (size_t)(y + j) * dest->pitch);
        for (int i = 0; i < r.width; i++) {
            if (x + i >= 0 && x + i < dest->width) {
                row[x + i] = copy[j * r.width + i];
            }
        }
    }
    free(copy);
    return 0;
}
int KGFX_DrawLine(KGFX_Surface *s, int x, int y, int end_x, int end_y, uint32_t color) {
    if (x < -4096 || x > 4096 || y < -4096 || y > 4096 || end_x < -4096 || end_x > 4096 ||
        end_y < -4096 || end_y > 4096) {
        errno = EINVAL;
        return kgfx_error("line coordinates");
    }
    int dx = abs(end_x - x), dy = -abs(end_y - y), sx = x < end_x ? 1 : -1, sy = y < end_y ? 1 : -1,
        error = dx + dy;
    for (;;) {
        if (x >= 0 && x < s->width && y >= 0 && y < s->height) {
            *(uint32_t *)((unsigned char *)s->pixels + (size_t)y * s->pitch + x * 4) = color;
        }
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
    return 0;
}
int KGFX_DrawText(KGFX_Window *w, int x, int y, const char *text, uint32_t color) {
    gui_text(&w->native, x, y, text, color);
    return 0;
}
int KGFX_UpdateWindowSurface(KGFX_Window *w) {
    return gui_present(&w->native) ? kgfx_error("present window") : 0;
}
uint64_t KGFX_GetTicks(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        kgfx_error("monotonic clock");
        return 0;
    }
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
void KGFX_Delay(unsigned milliseconds) {
    struct timespec delay = {milliseconds / 1000, (long)(milliseconds % 1000) * 1000000};
    if (nanosleep(&delay, NULL)) {
        kgfx_error("delay");
    }
}
