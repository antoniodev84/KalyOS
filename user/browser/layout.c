#include "browser.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
static uint32_t color(const char *s, uint32_t fallback) {
    if (*s == '#') {
        char *end;
        unsigned value = strtoul(s + 1, &end, 16);
        if (*end) {
            return fallback;
        }
        if (strlen(s) == 4) {
            return ((value & 0xf00) << 12) | ((value & 0xf00) << 8) | ((value & 0xf0) << 8) |
                   ((value & 0xf0) << 4) | ((value & 15) << 4) | (value & 15);
        }
        if (strlen(s) == 7) {
            return value;
        }
    }
    struct {
        const char *name;
        uint32_t value;
    } names[] = {{"black", 0},         {"white", 0xffffff},  {"red", 0xff0000},
                 {"green", 0x008000},  {"blue", 0x0000ff},   {"gray", 0x808080},
                 {"grey", 0x808080},   {"yellow", 0xffff00}, {"orange", 0xffa500},
                 {"purple", 0x800080}, {"navy", 0x000080},   {"teal", 0x008080}};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcasecmp(s, names[i].name)) {
            return names[i].value;
        }
    }
    if (!strncasecmp(s, "rgb(", 4)) {
        char *end;
        unsigned rgb[3];
        s += 4;
        for (int i = 0; i < 3; i++) {
            while (isspace((unsigned char)*s)) {
                s++;
            }
            rgb[i] = strtoul(s, &end, 10);
            if (end == s || rgb[i] > 255) {
                return fallback;
            }
            s = end;
            while (isspace((unsigned char)*s)) {
                s++;
            }
            if (*s++ != (i == 2 ? ')' : ',')) {
                return fallback;
            }
        }
        return (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
    }
    return fallback;
}
static int dimension(const char *s, int limit) {
    char *end;
    long n = strtol(s, &end, 10);
    if (s == end || (*end && strcmp(end, "px") && strcmp(end, "%")) || n < 0) {
        return -1;
    }
    return n > limit ? limit : n;
}
static void declarations(struct b_style *s, const char *text) {
    char copy[1024];
    b_copy(copy, sizeof(copy), text);
    char *save;
    for (char *property = strtok_r(copy, ";", &save); property;
         property = strtok_r(NULL, ";", &save)) {
        char *value = strchr(property, ':');
        if (!value) {
            continue;
        }
        *value++ = 0;
        while (isspace((unsigned char)*property)) {
            property++;
        }
        char *end = property + strlen(property);
        while (end > property && isspace((unsigned char)end[-1])) {
            *--end = 0;
        }
        while (isspace((unsigned char)*value)) {
            value++;
        }
        end = value + strlen(value);
        while (end > value && isspace((unsigned char)end[-1])) {
            *--end = 0;
        }
        if (!strcmp(property, "color")) {
            s->color = color(value, s->color);
        } else if (!strcmp(property, "background") || !strcmp(property, "background-color")) {
            s->has_background = strcmp(value, "transparent") != 0;
            s->background = color(value, s->background);
        } else if (!strcmp(property, "border-color")) {
            s->has_border = 1;
            s->border = color(value, s->color);
        } else if (!strcmp(property, "display")) {
            if (!strcmp(value, "none")) {
                s->display = -1;
            } else if (!strcmp(value, "block")) {
                s->display = 1;
            } else if (!strcmp(value, "inline")) {
                s->display = 0;
            }
        } else if (!strcmp(property, "width")) {
            int n = dimension(value, 4096);
            if (n >= 0) {
                s->width = n;
                s->percent = strchr(value, '%') != NULL;
            }
        } else if (!strcmp(property, "margin")) {
            int n = dimension(value, 128);
            if (n >= 0) {
                s->margin = n;
            }
        } else if (!strcmp(property, "padding")) {
            int n = dimension(value, 128);
            if (n >= 0) {
                s->padding = n;
            }
        } else if (!strcmp(property, "font-size")) {
            int n = dimension(value, 32);
            if (n > 0) {
                s->scale = n >= 24 ? 2 : 1;
            }
        } else if (!strcmp(property, "font-weight")) {
            s->bold = !strcmp(value, "bold") || atoi(value) >= 600;
        }
    }
}
static int word_member(const char *list, const char *word) {
    size_t n = strlen(word);
    while (*list) {
        while (isspace((unsigned char)*list)) {
            list++;
        }
        const char *end = list;
        while (*end && !isspace((unsigned char)*end)) {
            end++;
        }
        if ((size_t)(end - list) == n && !strncmp(list, word, n)) {
            return 1;
        }
        list = end;
    }
    return 0;
}
static int simple_match(struct b_document *d, int node, const char *selector) {
    if (node < 0) {
        return 0;
    }
    const char *p = selector;
    if (*p == '*') {
        p++;
    } else if (*p != '.' && *p != '#') {
        char tag[24];
        size_t n = 0;
        while (isalnum((unsigned char)*p) || *p == '-') {
            if (n + 1 < sizeof(tag)) {
                tag[n++] = *p;
            }
            p++;
        }
        tag[n] = 0;
        if (!n || strcmp(tag, d->nodes[node].tag)) {
            return 0;
        }
    }
    while (*p) {
        int kind = *p++;
        if (kind != '.' && kind != '#') {
            return 0;
        }
        char key[128];
        size_t n = 0;
        while (isalnum((unsigned char)*p) || *p == '-' || *p == '_') {
            if (n + 1 < sizeof(key)) {
                key[n++] = *p;
            }
            p++;
        }
        key[n] = 0;
        if (!n) {
            return 0;
        }
        if (kind == '#' ? strcmp(b_attr(d, node, "id"), key) != 0
                        : !word_member(b_attr(d, node, "class"), key)) {
            return 0;
        }
    }
    return 1;
}
static int matches(struct b_document *d, int node, const char *selector) {
    char copy[128];
    b_copy(copy, sizeof(copy), selector);
    char *parts[16], *save;
    int count = 0;
    for (char *p = strtok_r(copy, " \t\r\n", &save); p; p = strtok_r(NULL, " \t\r\n", &save)) {
        if (count == 16) {
            return 0;
        }
        parts[count++] = p;
    }
    if (!count || !simple_match(d, node, parts[--count])) {
        return 0;
    }
    while (count) {
        node = d->nodes[node].parent;
        while (node >= 0 && !simple_match(d, node, parts[count - 1])) {
            node = d->nodes[node].parent;
        }
        if (node < 0) {
            return 0;
        }
        count--;
    }
    return 1;
}
static int block_tag(const char *tag) {
    const char *tags[] = {"document", "html",   "body", "div",       "p",  "section", "article",
                          "header",   "footer", "main", "nav",       "h1", "h2",      "h3",
                          "h4",       "h5",     "h6",   "ul",        "ol", "li",      "pre",
                          "form",     "table",  "tr",   "blockquote"};
    for (unsigned i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        if (!strcmp(tag, tags[i])) {
            return 1;
        }
    }
    return 0;
}
static void styles(struct b_document *d) {
    int order[B_RULES];
    for (int i = 0; i < d->rule_count; i++) {
        int j = i;
        while (j > 0 && d->rules[order[j - 1]].specificity > d->rules[i].specificity) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    for (int i = 0; i < d->count; i++) {
        struct b_node *n = &d->nodes[i];
        struct b_style s = {.color = 0x202830, .scale = 1, .display = block_tag(n->tag)};
        if (n->parent >= 0) {
            struct b_style *p = &d->nodes[n->parent].style;
            s.color = p->color;
            s.scale = p->scale;
            s.bold = p->bold;
        }
        if (!strcmp(n->tag, "head") || n->hidden) {
            s.display = -1;
        }
        if (n->tag[0] == 'h' && n->tag[1] >= '1' && n->tag[1] <= '6' && !n->tag[2]) {
            s.bold = 1;
            s.margin = 8;
            s.scale = n->tag[1] <= '2' ? 2 : 1;
        }
        if (!strcmp(n->tag, "p") || !strcmp(n->tag, "ul") || !strcmp(n->tag, "ol")) {
            s.margin = 6;
        }
        if (!strcmp(n->tag, "li")) {
            s.padding = 8;
        }
        if (!strcmp(n->tag, "a")) {
            s.color = 0x1559b7;
        }
        if (!strcmp(n->tag, "b") || !strcmp(n->tag, "strong")) {
            s.bold = 1;
        }
        for (int j = 0; j < d->rule_count; j++) {
            if (matches(d, i, d->rules[order[j]].selector)) {
                declarations(&s, d->rules[order[j]].declarations);
            }
        }
        declarations(&s, b_attr(d, i, "style"));
        n->style = s;
    }
}
struct flow {
    int left, right, x, y, line, paint, link;
};
static void newline(struct flow *f) {
    f->y += f->line ? f->line : 20;
    f->x = f->left;
    f->line = 0;
}
static void hit_add(struct b_document *d, int node, int x, int y, int w, int h) {
    if (d->hit_count) {
        struct b_hit *last = &d->hits[d->hit_count - 1];
        if (last->node == node && last->y == y && last->height == h && last->x + last->width == x) {
            last->width += w;
            return;
        }
    }
    if (d->hit_count < B_HITS) {
        d->hits[d->hit_count++] = (struct b_hit){node, x, y, w, h};
    }
}
static void glyph(struct gui *g, int x, int y, unsigned code, const struct b_style *s) {
    if (s->scale == 1) {
        gui_glyph(g, x, y, code, s->color);
        if (s->bold) {
            gui_glyph(g, x + 1, y, code, s->color);
        }
        return;
    }
    uint32_t pixels[128] = {0};
    struct gui temp = *g;
    temp.pixels = pixels;
    temp.width = 8;
    temp.height = 16;
    gui_glyph(&temp, 0, 0, code, 1);
    for (int j = 0; j < 16; j++) {
        for (int i = 0; i < 8; i++) {
            if (pixels[j * 8 + i]) {
                gui_rect(g, x + i * 2, y + j * 2, 2 + s->bold, 2, s->color);
            }
        }
    }
}
static unsigned codepoint(const char **p) {
    unsigned c = (unsigned char)*(*p)++;
    if (c >= 0xc2 && c <= 0xdf && ((unsigned char)(*p)[0] & 0xc0) == 0x80) {
        c = ((c & 31) << 6) | ((unsigned char)*(*p)++ & 63);
    } else if (c >= 0xe0 && c <= 0xef && (*p)[0] && (*p)[1] &&
               ((unsigned char)(*p)[0] & 0xc0) == 0x80 && ((unsigned char)(*p)[1] & 0xc0) == 0x80) {
        c = ((c & 15) << 12) | (((unsigned char)(*p)[0] & 63) << 6) | ((unsigned char)(*p)[1] & 63);
        *p += 2;
    }
    return c;
}
static void text_flow(struct b_document *d, struct gui *g, struct flow *f, const char *text,
                      const struct b_style *s) {
    while (*text) {
        int whitespace = 0;
        while (isspace((unsigned char)*text)) {
            whitespace = 1;
            text++;
        }
        if (!*text) {
            break;
        }
        const char *end = text;
        int count = 0;
        while (*end && !isspace((unsigned char)*end) && count < 256) {
            codepoint(&end);
            count++;
        }
        int width = count * 8 * s->scale, space = whitespace && f->x > f->left ? 8 * s->scale : 0;
        if (f->x + space + width > f->right && f->x > f->left) {
            newline(f);
            space = 0;
        }
        if (f->paint && f->link >= 0 && space) {
            hit_add(d, f->link, f->x, f->y, space, 18 * s->scale);
        }
        f->x += space;
        const char *p = text;
        while (p < end) {
            if (f->x + 8 * s->scale > f->right && f->x > f->left) {
                newline(f);
            }
            unsigned code = codepoint(&p);
            int visible = f->y - d->scroll + 48;
            if (f->paint && visible + 16 * s->scale >= 48 && visible < g->height - 24) {
                glyph(g, f->x, visible, code, s);
                if (f->link >= 0) {
                    gui_rect(g, f->x, visible + 16 * s->scale, 8 * s->scale, 1, s->color);
                }
            }
            if (f->paint && f->link >= 0) {
                hit_add(d, f->link, f->x, f->y, 8 * s->scale, 18 * s->scale);
            }
            f->x += 8 * s->scale;
            if (f->line < 18 * s->scale + 2) {
                f->line = 18 * s->scale + 2;
            }
        }
        text = end;
    }
}
static void layout(struct b_document *d, struct gui *g, int node, struct flow *f, unsigned depth) {
    if (depth > 64) {
        return;
    }
    struct b_node *n = &d->nodes[node];
    struct b_style *s = &n->style;
    if (s->display < 0) {
        return;
    }
    if (!strcmp(n->tag, "br")) {
        newline(f);
        return;
    }
    if (!strcmp(n->tag, "hr")) {
        if (f->x > f->left) {
            newline(f);
        }
        if (f->paint) {
            gui_rect(g, f->left, f->y - d->scroll + 48, f->right - f->left, 1, 0xc0c6cc);
        }
        f->y += 12;
        return;
    }
    if (!strcmp(n->tag, "input") || !strcmp(n->tag, "button")) {
        if (!strcmp(b_attr(d, node, "type"), "hidden")) {
            return;
        }
        char text[256] = {0};
        int input = !strcmp(n->tag, "input");
        if (input) {
            b_copy(text, sizeof(text), b_attr(d, node, "value"));
        } else {
            b_collect_text(d, node, text, sizeof(text));
        }
        int width = s->width ? s->width : input ? 200 : (int)strlen(text) * 8 + 24;
        if (width < 40) {
            width = 40;
        }
        if (width > f->right - f->left) {
            width = f->right - f->left;
        }
        if (f->x + width > f->right && f->x > f->left) {
            newline(f);
        }
        n->x = f->x;
        n->y = f->y;
        n->width = width;
        n->height = 30;
        if (f->paint) {
            int y = f->y - d->scroll + 48;
            gui_rect(g, f->x, y, width, 28, node == d->focused ? 0x62a0d8 : 0x7d8e9e);
            gui_rect(g, f->x + 1, y + 1, width - 2, 26, input ? 0xffffff : 0xe6edf5);
            int chars = (width - 12) / 8;
            if (chars < 0) {
                chars = 0;
            }
            if ((unsigned)chars < strlen(text)) {
                text[chars] = 0;
            }
            gui_text(g, f->x + 6, y + 6, text, 0x16283c);
            hit_add(d, node, f->x, f->y, width, 30);
        }
        f->x += width + 8;
        if (f->line < 34) {
            f->line = 34;
        }
        return;
    }
    if (!strcmp(n->tag, "img")) {
        char label[256];
        snprintf(label, sizeof(label), "[Image: %s]", b_attr(d, node, "alt"));
        text_flow(d, g, f, label, s);
        return;
    }
    int oldlink = f->link;
    if (!strcmp(n->tag, "a") || *b_attr(d, node, "onclick")) {
        f->link = node;
    }
    if (s->display == 1) {
        if (f->x > f->left || f->line) {
            newline(f);
        }
        int x = f->left + s->margin, y = f->y + s->margin, width = f->right - x - s->margin;
        if (s->width) {
            int wanted = s->percent ? width * s->width / 100 : s->width;
            if (wanted < width) {
                width = wanted;
            }
        }
        if (width < 32) {
            width = 32;
        }
        if (f->paint) {
            if (s->has_background) {
                gui_rect(g, x, y - d->scroll + 48, width, n->height, s->background);
            }
            if (s->has_border) {
                int top = y - d->scroll + 48;
                gui_rect(g, x, top, width, 1, s->border);
                gui_rect(g, x, top + n->height - 1, width, 1, s->border);
                gui_rect(g, x, top, 1, n->height, s->border);
                gui_rect(g, x + width - 1, top, 1, n->height, s->border);
            }
        }
        struct flow child = {.left = x + s->padding,
                             .right = x + width - s->padding,
                             .x = x + s->padding,
                             .y = y + s->padding,
                             .paint = f->paint,
                             .link = f->link};
        if (child.right <= child.left) {
            child.right = child.left + 8;
        }
        if (!strcmp(n->tag, "li")) {
            text_flow(d, g, &child, "* ", s);
        }
        text_flow(d, g, &child, n->text, s);
        for (int c = n->first; c >= 0; c = d->nodes[c].next) {
            layout(d, g, c, &child, depth + 1);
        }
        if (child.line || child.x > child.left) {
            newline(&child);
        }
        n->x = x;
        n->y = y;
        n->width = width;
        n->height = child.y - y + s->padding;
        f->y = y + n->height + s->margin;
        f->x = f->left;
        f->line = 0;
    } else {
        text_flow(d, g, f, n->text, s);
        for (int c = n->first; c >= 0; c = d->nodes[c].next) {
            layout(d, g, c, f, depth + 1);
        }
    }
    f->link = oldlink;
}
void b_render(struct b_document *d, struct gui *g, int width, int height) {
    (void)height;
    styles(d);
    struct flow f = {.left = 12, .right = width - 16, .x = 12, .y = 10, .link = -1};
    layout(d, g, 0, &f, 0);
    d->height = f.y + f.line + 10;
    int maximum = d->height - (g->height - 72);
    if (maximum < 0) {
        maximum = 0;
    }
    if (d->scroll > maximum) {
        d->scroll = maximum;
    }
    if (d->scroll < 0) {
        d->scroll = 0;
    }
    gui_rect(g, 0, 48, width, g->height - 72, 0xffffff);
    d->hit_count = 0;
    f = (struct flow){.left = 12, .right = width - 16, .x = 12, .y = 10, .paint = 1, .link = -1};
    layout(d, g, 0, &f, 0);
    if (maximum > 0) {
        int track = g->height - 72;
        int thumb = track * track / d->height;
        if (thumb < 12) {
            thumb = 12;
        }
        gui_rect(g, width - 8, 48, 6, track, 0xe2e8ef);
        gui_rect(g, width - 8, 48 + (track - thumb) * d->scroll / maximum, 6, thumb, 0x889db2);
    }
}
int b_hit(struct b_document *d, int x, int y) {
    y = y - 48 + d->scroll;
    for (int i = d->hit_count - 1; i >= 0; i--) {
        struct b_hit *h = &d->hits[i];
        if (x >= h->x && y >= h->y && x < h->x + h->width && y < h->y + h->height) {
            return h->node;
        }
    }
    return -1;
}
