#include "kaly.h"
extern const uint8_t console_font[], console_font_end[];
static uint8_t *pixels;
static unsigned width, height, pitch, depth, red, green, blue, rbits, gbits, bbits;
static unsigned columns, rows, column, line, fw, fh, glyph_size, glyph_count;
static const uint8_t *glyphs;
static uint16_t unicode_map[65536];
static uint32_t utf8_codepoint, utf8_minimum;
static unsigned utf8_remaining;
static int pointer_x, pointer_y;
static uint32_t saved[16 * 16];
static bool pointer_visible;
static struct lock fb_lock;
static uint32_t pixel_get(unsigned x, unsigned y) {
    uint8_t *p = pixels + y * pitch + x * depth;
    uint32_t v = 0;
    for (unsigned i = 0; i < depth; i++) {
        v |= (uint32_t)p[i] << (i * 8);
    }
    return v;
}
static void pixel(unsigned x, unsigned y, uint32_t c) {
    uint8_t *p = pixels + y * pitch + x * depth;
    for (unsigned i = 0; i < depth; i++) {
        p[i] = c >> (i * 8);
    }
}
static uint32_t color(unsigned r, unsigned g, unsigned b) {
    return ((r >> (8 - rbits)) << red) | ((g >> (8 - gbits)) << green) |
           ((b >> (8 - bbits)) << blue);
}
static void pointer_hide(void) {
    if (!pointer_visible) {
        return;
    }
    for (unsigned y = 0; y < 16 && (unsigned)pointer_y + y < height; y++) {
        for (unsigned x = 0; x < 16 && (unsigned)pointer_x + x < width; x++) {
            pixel(pointer_x + x, pointer_y + y, saved[y * 16 + x]);
        }
    }
    pointer_visible = false;
}
static void pointer_show(void) {
    for (unsigned y = 0; y < 16 && (unsigned)pointer_y + y < height; y++) {
        for (unsigned x = 0; x < 16 && (unsigned)pointer_x + x < width; x++) {
            saved[y * 16 + x] = pixel_get(pointer_x + x, pointer_y + y);
            if (x <= y / 2 && y < 14) {
                pixel(pointer_x + x, pointer_y + y,
                      x == 0 || x == y / 2 ? 0 : color(255, 255, 255));
            }
        }
    }
    pointer_visible = true;
}
unsigned framebuffer_width(void) {
    return pixels ? width : 1024;
}
unsigned framebuffer_height(void) {
    return pixels ? height : 768;
}
void framebuffer_pointer(int x, int y) {
    if (!pixels) {
        return;
    }
    lock(&fb_lock);
    pointer_hide();
    pointer_x = MAX(0, MIN(x, (int)width - 1));
    pointer_y = MAX(0, MIN(y, (int)height - 1));
    pointer_show();
    unlock(&fb_lock);
}
static void draw(unsigned codepoint) {
    unsigned c = codepoint < 65536 ? unicode_map[codepoint] : 0xffff;
    if (c == 0xffff || c >= glyph_count) {
        c = unicode_map['?'];
        if (c == 0xffff) {
            c = 0;
        }
    }
    const uint8_t *g = glyphs + c * glyph_size;
    for (unsigned y = 0; y < fh; y++) {
        for (unsigned x = 0; x < fw; x++) {
            pixel(column * fw + x, line * fh + y,
                  g[y * ((fw + 7) / 8) + x / 8] & (128 >> (x % 8)) ? color(224, 232, 240)
                                                                   : color(16, 24, 32));
        }
    }
}
void framebuffer_putc(char c) {
    if (!pixels) {
        return;
    }
    lock(&fb_lock);
    unsigned codepoint = (uint8_t)c;
    if (utf8_remaining) {
        if ((codepoint & 0xc0) == 0x80) {
            utf8_codepoint = (utf8_codepoint << 6) | (codepoint & 63);
            if (--utf8_remaining) {
                unlock(&fb_lock);
                return;
            }
            codepoint = utf8_codepoint;
            if (codepoint < utf8_minimum || codepoint > 0x10ffff ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
                codepoint = '?';
            }
        } else {
            utf8_remaining = 0;
            codepoint = '?';
        }
    } else if (codepoint >= 0xc2 && codepoint <= 0xf4) {
        utf8_remaining = codepoint < 0xe0 ? 1 : codepoint < 0xf0 ? 2 : 3;
        utf8_minimum = utf8_remaining == 1 ? 0x80 : utf8_remaining == 2 ? 0x800 : 0x10000;
        utf8_codepoint = codepoint & (utf8_remaining == 1 ? 31 : utf8_remaining == 2 ? 15 : 7);
        unlock(&fb_lock);
        return;
    } else if (codepoint >= 128) {
        codepoint = '?';
    }
    bool cursor = pointer_visible;
    pointer_hide();
    if (codepoint == '\n') {
        column = 0;
        line++;
    } else if (codepoint == '\r') {
        column = 0;
    } else if (codepoint == '\b') {
        if (column) {
            column--;
            draw(' ');
        }
    } else if (codepoint == '\t') {
        unsigned spaces = 8 - column % 8;
        while (spaces-- && column < columns) {
            draw(' ');
            column++;
        }
    } else if (codepoint >= 32) {
        draw(codepoint);
        column++;
    }
    if (column >= columns) {
        column = 0;
        line++;
    }
    if (line >= rows) {
        memmove(pixels, pixels + fh * pitch, (rows - 1) * fh * pitch);
        memset(pixels + (rows - 1) * fh * pitch, 0, fh * pitch);
        line = rows - 1;
    }
    if (cursor) {
        pointer_show();
    }
    unlock(&fb_lock);
}
static void font_map(bool first_format, bool has_unicode) {
    memset(unicode_map, 255, sizeof(unicode_map));
    if (!has_unicode) {
        for (unsigned i = 0; i < MIN(glyph_count, 65536U); i++) {
            unicode_map[i] = i;
        }
        return;
    }
    const uint8_t *p = glyphs + glyph_count * glyph_size;
    unsigned glyph = 0;
    bool sequence = false;
    while (p < console_font_end && glyph < glyph_count) {
        uint32_t codepoint;
        if (first_format) {
            if (console_font_end - p < 2) {
                break;
            }
            codepoint = p[0] | ((unsigned)p[1] << 8);
            p += 2;
            if (codepoint == 0xffff) {
                glyph++;
                sequence = false;
                continue;
            }
            if (codepoint == 0xfffe) {
                sequence = true;
                continue;
            }
        } else {
            codepoint = *p++;
            if (codepoint == 0xff) {
                glyph++;
                sequence = false;
                continue;
            }
            if (codepoint == 0xfe) {
                sequence = true;
                continue;
            }
            if (codepoint >= 0xc2 && codepoint <= 0xf4) {
                unsigned count = codepoint < 0xe0 ? 1 : codepoint < 0xf0 ? 2 : 3;
                codepoint &= count == 1 ? 31 : count == 2 ? 15 : 7;
                if ((unsigned)(console_font_end - p) < count) {
                    break;
                }
                bool valid = true;
                for (unsigned i = 0; i < count; i++) {
                    if ((p[i] & 0xc0) != 0x80) {
                        valid = false;
                    }
                    codepoint = (codepoint << 6) | (p[i] & 63);
                }
                p += count;
                if (!valid) {
                    continue;
                }
            } else if (codepoint >= 128) {
                continue;
            }
        }
        if (!sequence && codepoint < 65536) {
            unicode_map[codepoint] = glyph;
        }
    }
}
void framebuffer_init(uint64_t info) {
    const uint8_t *font = console_font;
    size_t length = console_font_end - console_font;
    bool first_format = false, has_unicode = false;
    if (length >= 4 && font[0] == 0x36 && font[1] == 4) {
        first_format = true;
        has_unicode = font[2] & 2;
        fw = 8;
        fh = glyph_size = font[3];
        glyph_count = font[2] & 1 ? 512 : 256;
        glyphs = font + 4;
    } else if (length >= 32 && *(const uint32_t *)font == 0x864ab572) {
        const uint32_t *h = (const void *)font;
        if (h[2] < 32 || h[2] > length) {
            return;
        }
        has_unicode = h[3] & 1;
        glyph_count = h[4];
        glyph_size = h[5];
        fh = h[6];
        fw = h[7];
        glyphs = font + h[2];
    } else {
        return;
    }
    if (!fw || fw > 32 || !fh || fh > 64 || glyph_count < 256 || glyph_size < fh * ((fw + 7) / 8) ||
        glyph_count > (length - (size_t)(glyphs - font)) / glyph_size) {
        return;
    }
    font_map(first_format, has_unicode);
    uint32_t total = *(uint32_t *)info;
    for (uint64_t t = info + 8; t + 8 <= info + total;) {
        uint32_t type = *(uint32_t *)t, size = *(uint32_t *)(t + 4);
        if (size < 8 || size > info + total - t || !type) {
            break;
        }
        if (type == 8 && size >= 38) {
            const uint8_t *p = (void *)(t + 8);
            uint64_t address = *(const uint64_t *)p;
            pitch = *(const uint32_t *)(p + 8);
            width = *(const uint32_t *)(p + 12);
            height = *(const uint32_t *)(p + 16);
            depth = p[20] / 8;
            red = p[24];
            rbits = p[25];
            green = p[26];
            gbits = p[27];
            blue = p[28];
            bbits = p[29];
            if (p[21] != 1 || (p[20] != 24 && p[20] != 32) || width < fw || width > 8192 ||
                height < fh || height > 8192 || pitch < width * depth || pitch > 65536 || !rbits ||
                rbits > 8 || !gbits || gbits > 8 || !bbits || bbits > 8 || red + rbits > p[20] ||
                green + gbits > p[20] || blue + bbits > p[20]) {
                return;
            }
            pixels = mmio_map(address, (size_t)pitch * height);
            if (!pixels) {
                return;
            }
            columns = width / fw;
            rows = height / fh;
            memset(pixels, 0, (size_t)pitch * height);
            kprintf("Framebuffer %ux%u PSF %ux%u\n", (uint64_t)width, (uint64_t)height,
                    (uint64_t)fw, (uint64_t)fh);
            return;
        }
        t += ALIGN_UP(size, 8);
    }
}
