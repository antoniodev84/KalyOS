#include "gfx.h"
void gfx_clip(struct surface *s, int x, int y, int w, int h) {
    s->left = MAX(0, x);
    s->top = MAX(0, y);
    s->right = MIN(s->width, x + w);
    s->bottom = MIN(s->height, y + h);
}
void gfx_rect(struct surface *s, int x, int y, int w, int h, uint32_t color) {
    int right = MIN(s->right, x + w), bottom = MIN(s->bottom, y + h);
    for (int j = MAX(y, s->top); j < bottom; j++) {
        for (int i = MAX(x, s->left); i < right; i++) {
            s->pixels[j * s->width + i] = color;
        }
    }
}
static void dot(struct surface *s, int x, int y, uint32_t c) {
    if (x >= s->left && x < s->right && y >= s->top && y < s->bottom) {
        s->pixels[y * s->width + x] = c;
    }
}
void gfx_line(struct surface *s, int x, int y, int end_x, int end_y, uint32_t c) {
    int dx = end_x > x ? end_x - x : x - end_x;
    int dy = end_y > y ? y - end_y : end_y - y;
    int sx = x < end_x ? 1 : -1, sy = y < end_y ? 1 : -1, error = dx + dy;
    for (;;) {
        dot(s, x, y, c);
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
void gfx_text(struct surface *s, int x, int y, const char *text, uint32_t c) {
    unsigned fw = framebuffer_font_width(), fh = framebuffer_font_height();
    int start = x;
    while (*text) {
        unsigned code = (uint8_t)*text++;
        if (code == '\n') {
            x = start;
            y += fh;
            continue;
        }
        if (code >= 0xc2 && code <= 0xf4) {
            unsigned n = code < 0xe0 ? 1 : code < 0xf0 ? 2 : 3;
            code &= n == 1 ? 31 : n == 2 ? 15 : 7;
            while (n-- && ((uint8_t)*text & 0xc0) == 0x80) {
                code = (code << 6) | ((uint8_t)*text++ & 63);
            }
        }
        const uint8_t *g = framebuffer_glyph(code);
        for (unsigned j = 0; j < fh; j++) {
            for (unsigned i = 0; i < fw; i++) {
                if (g[j * ((fw + 7) / 8) + i / 8] & (128 >> (i % 8))) {
                    dot(s, x + i, y + j, c);
                }
            }
        }
        x += fw;
    }
}
void gfx_blit(struct surface *s, int x, int y, const uint32_t *p, int w, int h) {
    int right = MIN(s->right, x + w), bottom = MIN(s->bottom, y + h);
    int left = MAX(s->left, x);
    if (right <= left) {
        return;
    }
    for (int j = MAX(s->top, y); j < bottom; j++) {
        memcpy(s->pixels + j * s->width + left, p + (j - y) * w + left - x,
               (size_t)(right - left) * 4);
    }
}
