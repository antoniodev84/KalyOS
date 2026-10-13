#ifndef GFX_H
#define GFX_H
#include "kaly.h"
struct surface {
    uint32_t *pixels;
    int width, height;
    int left, top, right, bottom;
};
void gfx_clip(struct surface *, int, int, int, int);
void gfx_rect(struct surface *, int, int, int, int, uint32_t);
void gfx_line(struct surface *, int, int, int, int, uint32_t);
void gfx_text(struct surface *, int, int, const char *, uint32_t);
void gfx_blit(struct surface *, int, int, const uint32_t *, int, int);
unsigned framebuffer_font_width(void);
unsigned framebuffer_font_height(void);
const uint8_t *framebuffer_glyph(unsigned);
void framebuffer_graphics(bool);
bool framebuffer_available(void);
void framebuffer_present(const uint32_t *, int, int, int, int);
#endif
