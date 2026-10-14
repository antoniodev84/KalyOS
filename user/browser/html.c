#include "browser.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
void b_copy(char *dest, size_t capacity, const char *source) {
    if (capacity) {
        size_t n = strlen(source);
        if (n >= capacity) {
            n = capacity - 1;
        }
        memcpy(dest, source, n);
        dest[n] = 0;
    }
}
const char *b_attr(struct b_document *d, int node, const char *name) {
    if (node < 0 || node >= d->count) {
        return "";
    }
    for (int a = d->nodes[node].attr; a >= 0; a = d->attrs[a].next) {
        if (!strcasecmp(d->attrs[a].name, name)) {
            return d->attrs[a].value;
        }
    }
    return "";
}
int b_set_attr(struct b_document *d, int node, const char *name, const char *value) {
    if (node < 0 || node >= d->count) {
        return -1;
    }
    for (int a = d->nodes[node].attr; a >= 0; a = d->attrs[a].next) {
        if (!strcasecmp(d->attrs[a].name, name)) {
            b_copy(d->attrs[a].value, sizeof(d->attrs[a].value), value);
            return 0;
        }
    }
    if (d->attr_count >= B_ATTRS) {
        return -1;
    }
    int a = d->attr_count++;
    d->attrs[a].next = d->nodes[node].attr;
    d->nodes[node].attr = a;
    b_copy(d->attrs[a].name, sizeof(d->attrs[a].name), name);
    b_copy(d->attrs[a].value, sizeof(d->attrs[a].value), value);
    return 0;
}
int b_element(struct b_document *d, const char *id) {
    if (!*id) {
        return -1;
    }
    for (int i = 0; i < d->count; i++) {
        int root = i;
        while (d->nodes[root].parent >= 0) {
            root = d->nodes[root].parent;
        }
        if (root == 0 && !strcmp(b_attr(d, i, "id"), id)) {
            return i;
        }
    }
    return -1;
}
void b_text(struct b_document *d, int node, const char *text) {
    if (node < 0 || node >= d->count) {
        return;
    }
    for (int child = d->nodes[node].first; child >= 0; child = d->nodes[child].next) {
        d->nodes[child].parent = -1;
    }
    d->nodes[node].first = d->nodes[node].last = -1;
    b_copy(d->nodes[node].text, sizeof(d->nodes[node].text), text);
}
void b_collect_text(struct b_document *d, int node, char *out, size_t size) {
    if (!size || node < 0 || node >= d->count) {
        return;
    }
    size_t used = strlen(out);
    if (used + 1 < size) {
        b_copy(out + used, size - used, d->nodes[node].text);
    }
    for (int c = d->nodes[node].first; c >= 0; c = d->nodes[c].next) {
        b_collect_text(d, c, out, size);
    }
}
static int node_add(struct b_document *d, int parent, const char *tag) {
    if (d->count >= B_NODES) {
        return -1;
    }
    int index = d->count++;
    struct b_node *n = &d->nodes[index];
    n->parent = parent;
    n->first = n->last = n->next = n->attr = -1;
    b_copy(n->tag, sizeof(n->tag), tag);
    if (parent >= 0) {
        struct b_node *p = &d->nodes[parent];
        if (p->last >= 0) {
            d->nodes[p->last].next = index;
        } else {
            p->first = index;
        }
        p->last = index;
    }
    return index;
}
static const char *find_case(const char *text, const char *word) {
    size_t n = strlen(word);
    for (; *text; text++) {
        if (!strncasecmp(text, word, n)) {
            return text;
        }
    }
    return NULL;
}
static void decode(char *out, size_t capacity, const char *text, size_t length) {
    size_t used = 0;
    for (size_t i = 0; i < length && used + 1 < capacity; i++) {
        unsigned c = (unsigned char)text[i];
        int decoded = 0;
        if (c == '&') {
            size_t j = i + 1;
            while (j < length && j - i < 16 && text[j] != ';') {
                j++;
            }
            if (j < length && text[j] == ';') {
                char entity[16];
                size_t n = j - i - 1;
                memcpy(entity, text + i + 1, n);
                entity[n] = 0;
                unsigned replacement = 0;
                if (!strcmp(entity, "amp")) {
                    replacement = '&';
                } else if (!strcmp(entity, "lt")) {
                    replacement = '<';
                } else if (!strcmp(entity, "gt")) {
                    replacement = '>';
                } else if (!strcmp(entity, "quot")) {
                    replacement = '"';
                } else if (!strcmp(entity, "apos")) {
                    replacement = '\'';
                } else if (!strcmp(entity, "nbsp")) {
                    replacement = ' ';
                } else if (entity[0] == '#') {
                    char *end;
                    replacement = strtoul(entity + (entity[1] == 'x' || entity[1] == 'X' ? 2 : 1),
                                          &end, entity[1] == 'x' || entity[1] == 'X' ? 16 : 10);
                    if (*end || replacement > 0xffff ||
                        (replacement >= 0xd800 && replacement <= 0xdfff)) {
                        replacement = 0;
                    }
                }
                if (replacement) {
                    c = replacement;
                    decoded = 1;
                    i = j;
                }
            }
        }
        if (!decoded || c < 128) {
            out[used++] = c;
        } else if (c < 2048 && used + 2 < capacity) {
            out[used++] = 0xc0 | (c >> 6);
            out[used++] = 0x80 | (c & 63);
        } else if (c > 255 && used + 3 < capacity) {
            out[used++] = 0xe0 | (c >> 12);
            out[used++] = 0x80 | ((c >> 6) & 63);
            out[used++] = 0x80 | (c & 63);
        } else {
            out[used++] = c;
        }
    }
    out[used] = 0;
}
static void css_rules(struct b_document *d, const char *text, size_t size) {
    const char *p = text, *end = text + size;
    while (p < end && d->rule_count < B_RULES) {
        while (p < end && isspace((unsigned char)*p)) {
            p++;
        }
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            const char *close = strstr(p + 2, "*/");
            if (!close || close >= end) {
                break;
            }
            p = close + 2;
            continue;
        }
        const char *open = memchr(p, '{', end - p);
        if (!open) {
            break;
        }
        const char *close = memchr(open + 1, '}', end - open - 1);
        if (!close) {
            break;
        }
        if (*p != '@') {
            const char *selector = p;
            while (selector < open && d->rule_count < B_RULES) {
                const char *comma = memchr(selector, ',', open - selector);
                const char *stop = comma ? comma : open;
                while (selector < stop && isspace((unsigned char)*selector)) {
                    selector++;
                }
                while (stop > selector && isspace((unsigned char)stop[-1])) {
                    stop--;
                }
                struct b_rule *r = &d->rules[d->rule_count++];
                size_t n = stop - selector;
                if (n >= sizeof(r->selector)) {
                    n = sizeof(r->selector) - 1;
                }
                memcpy(r->selector, selector, n);
                r->selector[n] = 0;
                n = close - open - 1;
                if (n >= sizeof(r->declarations)) {
                    n = sizeof(r->declarations) - 1;
                }
                memcpy(r->declarations, open + 1, n);
                r->declarations[n] = 0;
                for (const char *s = r->selector; *s;) {
                    int weight = 0;
                    if (*s == '#') {
                        weight = 100;
                        s++;
                    } else if (*s == '.') {
                        weight = 10;
                        s++;
                    } else if (isalpha((unsigned char)*s)) {
                        weight = 1;
                    } else {
                        s++;
                        continue;
                    }
                    r->specificity += weight;
                    while (isalnum((unsigned char)*s) || *s == '-' || *s == '_') {
                        s++;
                    }
                }
                if (!comma) {
                    break;
                }
                selector = comma + 1;
            }
        }
        p = close + 1;
    }
}
int b_parse(struct b_document *d, const char *source) {
    b_js_free(d);
    memset(d, 0, sizeof(*d));
    d->focused = -1;
    int parent = node_add(d, -1, "document");
    const char *p = source;
    unsigned depth = 0;
    while (*p) {
        if (*p != '<') {
            const char *end = strchr(p, '<');
            if (!end) {
                end = p + strlen(p);
            }
            while (p < end) {
                int index = node_add(d, parent, "#text");
                if (index < 0) {
                    goto limit;
                }
                size_t n = end - p;
                if (n > 1536) {
                    n = 1536;
                }
                while (n && ((unsigned char)p[n] & 0xc0) == 0x80) {
                    n--;
                }
                if (!n) {
                    n = 1;
                }
                decode(d->nodes[index].text, sizeof(d->nodes[index].text), p, n);
                p += n;
            }
            continue;
        }
        if (!strncmp(p, "<!--", 4)) {
            const char *end = strstr(p + 4, "-->");
            p = end ? end + 3 : p + strlen(p);
            continue;
        }
        p++;
        if (*p == '!') {
            while (*p && *p != '>') {
                p++;
            }
            if (*p) {
                p++;
            }
            continue;
        }
        int closing = *p == '/';
        if (closing) {
            p++;
        }
        char tag[24];
        size_t n = 0;
        while (isalnum((unsigned char)*p) || *p == '-') {
            if (n + 1 < sizeof(tag)) {
                tag[n++] = tolower((unsigned char)*p);
            }
            p++;
        }
        tag[n] = 0;
        if (!n) {
            if (*p) {
                p++;
            }
            continue;
        }
        if (closing) {
            while (*p && *p != '>') {
                p++;
            }
            if (*p) {
                p++;
            }
            for (int a = parent; a > 0; a = d->nodes[a].parent) {
                if (!strcmp(d->nodes[a].tag, tag)) {
                    parent = d->nodes[a].parent;
                    depth = 0;
                    for (int k = parent; k > 0; k = d->nodes[k].parent) {
                        depth++;
                    }
                    break;
                }
            }
            continue;
        }
        if ((!strcmp(tag, "p") || !strcmp(tag, "li")) && !strcmp(d->nodes[parent].tag, tag)) {
            parent = d->nodes[parent].parent;
            if (depth) {
                depth--;
            }
        }
        int index = node_add(d, parent, tag);
        if (index < 0) {
            goto limit;
        }
        int self = 0;
        while (*p && *p != '>') {
            while (isspace((unsigned char)*p)) {
                p++;
            }
            if (*p == '/') {
                self = 1;
                p++;
                continue;
            }
            if (*p == '>') {
                break;
            }
            char name[32], value[512];
            n = 0;
            while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '>' && *p != '/') {
                if (n + 1 < sizeof(name)) {
                    name[n++] = tolower((unsigned char)*p);
                }
                p++;
            }
            name[n] = 0;
            while (isspace((unsigned char)*p)) {
                p++;
            }
            const char *start = p;
            size_t length = 0;
            if (*p == '=') {
                p++;
                while (isspace((unsigned char)*p)) {
                    p++;
                }
                int quote = *p == '\'' || *p == '"' ? *p++ : 0;
                start = p;
                while (*p && (quote ? *p != quote : !isspace((unsigned char)*p) && *p != '>')) {
                    p++;
                }
                length = p - start;
                if (quote && *p) {
                    p++;
                }
            }
            decode(value, sizeof(value), start, length);
            if (n && b_set_attr(d, index, name, value)) {
                goto limit;
            }
            if (!n && *p && *p != '>') {
                p++;
            }
        }
        if (*p) {
            p++;
        }
        if (!strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "title")) {
            char ending[40];
            snprintf(ending, sizeof(ending), "</%s", tag);
            const char *end = find_case(p, ending);
            if (!end) {
                end = p + strlen(p);
            }
            if (!strcmp(tag, "style")) {
                css_rules(d, p, end - p);
            } else if (!strcmp(tag, "title")) {
                decode(d->title, sizeof(d->title), p, end - p);
            } else if (!*b_attr(d, index, "src")) {
                const char *type = b_attr(d, index, "type");
                if (!*type || !strcasecmp(type, "text/javascript") ||
                    !strcasecmp(type, "application/javascript")) {
                    size_t size = end - p;
                    if (size + d->script_length + 2 > sizeof(d->scripts)) {
                        goto limit;
                    }
                    memcpy(d->scripts + d->script_length, p, size);
                    d->script_length += size;
                    d->scripts[d->script_length++] = '\n';
                    d->scripts[d->script_length] = 0;
                }
            }
            p = end;
            d->nodes[index].hidden = 1;
            continue;
        }
        if (!strcmp(tag, "head") || !strcmp(tag, "meta") || !strcmp(tag, "link")) {
            d->nodes[index].hidden = 1;
        }
        if (!self && strcmp(tag, "br") && strcmp(tag, "hr") && strcmp(tag, "img") &&
            strcmp(tag, "input") && strcmp(tag, "meta") && strcmp(tag, "link")) {
            if (++depth > 64) {
                goto limit;
            }
            parent = index;
        }
    }
    return 0;
limit:
    b_copy(d->message, sizeof(d->message), "Document exceeds HTML, CSS, nesting or script limits");
    return -1;
}
