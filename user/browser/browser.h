#ifndef KALY_BROWSER_H
#define KALY_BROWSER_H
#include <gui.h>
#include <stdint.h>
#include <stddef.h>
#define B_NODES 512
#define B_ATTRS 2048
#define B_RULES 128
#define B_HITS 1024
#define B_SOURCE (1024 * 1024)
#define B_URL 1024
struct b_attr {
    int next;
    char name[32], value[512];
};
struct b_style {
    uint32_t color, background, border;
    int has_background, has_border, display, width, percent, margin, padding, scale, bold;
};
struct b_node {
    int parent, first, last, next, attr, hidden;
    char tag[24], text[2048];
    struct b_style style;
    int x, y, width, height;
};
struct b_rule {
    char selector[128], declarations[1024];
    int specificity;
};
struct b_hit {
    int node, x, y, width, height;
};
struct b_document {
    struct b_node nodes[B_NODES];
    struct b_attr attrs[B_ATTRS];
    struct b_rule rules[B_RULES];
    struct b_hit hits[B_HITS];
    int count, attr_count, rule_count, hit_count, height, scroll, focused;
    char title[64], scripts[65536], message[256], navigate[B_URL], location[B_URL];
    int script_length;
    void *js;
};
struct b_response {
    int ok;
    uint32_t length;
    char url[B_URL], message[256];
};
void b_copy(char *, size_t, const char *);
const char *b_attr(struct b_document *, int, const char *);
int b_set_attr(struct b_document *, int, const char *, const char *);
int b_element(struct b_document *, const char *);
void b_text(struct b_document *, int, const char *);
void b_collect_text(struct b_document *, int, char *, size_t);
int b_parse(struct b_document *, const char *);
void b_render(struct b_document *, struct gui *, int, int);
int b_hit(struct b_document *, int, int);
int b_url(const char *, const char *, char *, size_t);
int b_fetch(const char *, struct b_response *, char **);
int b_js_init(struct b_document *);
int b_js_run(struct b_document *, const char *);
void b_js_free(struct b_document *);
#endif
