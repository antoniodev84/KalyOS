#include "browser.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
struct browser {
    struct gui gui;
    struct b_document *document;
    char address[B_URL], current[B_URL], pending[B_URL], status[256];
    char history[32][B_URL];
    int history_count, history_index, history_target;
    int address_focus, selected, running, dirty, pipe_fd, worker, phase;
    size_t received, header_received;
    char *source;
    struct b_response response;
    unsigned buttons;
};
static int write_all(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n <= 0) {
            return -1;
        }
        p += n;
        size -= n;
    }
    return 0;
}
static void message(struct browser *b, const char *text) {
    b_copy(b->status, sizeof(b->status), text);
    b->dirty = 1;
}
static void navigate(struct browser *b, const char *reference, int from_page) {
    char target[B_URL];
    if (!strncmp(reference, "https://", 8)) {
        message(b, "HTTPS is not supported: TLS is unavailable");
        return;
    }
    if (!strcmp(reference, "about:home")) {
        reference = "file:///home/browser.html";
    }
    if (b_url(from_page ? b->current : NULL, reference, target, sizeof(target))) {
        message(b, "Invalid URL. Use http://host/path or file:///path");
        return;
    }
    if (from_page && !strncmp(b->current, "http://", 7) && strncmp(target, "http://", 7)) {
        message(b, "HTTP pages cannot navigate to local files");
        return;
    }
    if (from_page && reference[0] == '#') {
        int node = b_element(b->document, reference + 1);
        if (node >= 0) {
            b->document->scroll = b->document->nodes[node].y;
        }
        b->dirty = 1;
        return;
    }
    if (b->worker > 0) {
        b_copy(b->pending, sizeof(b->pending), target);
        message(b, "Loading; the new address is queued");
        return;
    }
    int pipes[2];
    if (pipe(pipes)) {
        message(b, "Could not create network worker pipe");
        return;
    }
    int child = fork();
    if (child < 0) {
        close(pipes[0]);
        close(pipes[1]);
        message(b, "Could not create network worker");
        return;
    }
    if (!child) {
        close(pipes[0]);
        close(b->gui.fd);
        struct b_response response;
        char *body = NULL;
        b_fetch(target, &response, &body);
        int failed = write_all(pipes[1], &response, sizeof(response));
        if (!failed && response.ok) {
            failed = write_all(pipes[1], body, response.length);
        }
        free(body);
        close(pipes[1]);
        _exit(failed ? 1 : 0);
    }
    close(pipes[1]);
    if (fcntl(pipes[0], F_SETFL, O_NONBLOCK) < 0) {
        close(pipes[0]);
        message(b, "Could not configure network pipe");
        return;
    }
    b->pipe_fd = pipes[0];
    b->worker = child;
    b->header_received = b->received = 0;
    free(b->source);
    b->source = NULL;
    memset(&b->response, 0, sizeof(b->response));
    b_copy(b->address, sizeof(b->address), target);
    b->address_focus = 0;
    message(b, "Loading...");
}
static void finish(struct browser *b, int success) {
    close(b->pipe_fd);
    b->pipe_fd = -1;
    b->worker = 0;
    if (*b->pending) {
        char target[B_URL];
        b_copy(target, sizeof(target), b->pending);
        b->pending[0] = 0;
        free(b->source);
        b->source = NULL;
        navigate(b, target, 0);
        return;
    }
    if (success) {
        b->source[b->response.length] = 0;
        if (b_parse(b->document, b->source)) {
            message(b, b->document->message);
        } else {
            b_copy(b->current, sizeof(b->current), b->response.url);
            b_copy(b->document->location, sizeof(b->document->location), b->current);
            b_copy(b->address, sizeof(b->address), b->current);
            if (b->history_target >= 0) {
                b->history_index = b->history_target;
            } else if (b->history_index < 0 || strcmp(b->history[b->history_index], b->current)) {
                b->history_count = b->history_index + 1;
                if (b->history_count == 32) {
                    memmove(b->history, b->history + 1, 31 * sizeof(b->history[0]));
                    b->history_count = 31;
                }
                b_copy(b->history[b->history_count], B_URL, b->current);
                b->history_index = b->history_count++;
            }
            snprintf(b->status, sizeof(b->status),
                     "Loaded %u bytes | HTTP, HTML/CSS subset, KalyJS", b->response.length);
            b_js_init(b->document);
            if (*b->document->message) {
                message(b, b->document->message);
            }
            char title[64];
            snprintf(title, sizeof(title), "%s - KalyBrowser",
                     *b->document->title ? b->document->title : "Page");
            gui_title(&b->gui, title);
        }
    } else {
        message(b, *b->response.message ? b->response.message
                                        : "Network worker returned an incomplete response");
    }
    b->history_target = -1;
    free(b->source);
    b->source = NULL;
    b->dirty = 1;
}
static void receive_page(struct browser *b) {
    while (b->worker > 0) {
        if (b->header_received < sizeof(b->response)) {
            ssize_t n = read(b->pipe_fd, (char *)&b->response + b->header_received,
                             sizeof(b->response) - b->header_received);
            if (n < 0 && errno == EAGAIN) {
                break;
            }
            if (n <= 0) {
                finish(b, 0);
                break;
            }
            b->header_received += n;
            if (b->header_received < sizeof(b->response)) {
                continue;
            }
            if (!b->response.ok || b->response.length > B_SOURCE) {
                finish(b, 0);
                break;
            }
            b->source = malloc((size_t)b->response.length + 1);
            if (!b->source) {
                b_copy(b->response.message, sizeof(b->response.message),
                       "Not enough memory for page");
                finish(b, 0);
                break;
            }
        }
        if (b->received == b->response.length) {
            finish(b, 1);
            break;
        }
        ssize_t n = read(b->pipe_fd, b->source + b->received, b->response.length - b->received);
        if (n < 0 && errno == EAGAIN) {
            break;
        }
        if (n <= 0) {
            finish(b, 0);
            break;
        }
        b->received += n;
    }
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}
static void redraw(struct browser *b) {
    struct gui *g = &b->gui;
    b_render(b->document, g, g->width, g->height);
    gui_rect(g, 0, 0, g->width, 48, 0x26364a);
    gui_button(g, 4, 8, 36, "<", b->history_index > 0 ? 1 : 0);
    gui_button(g, 44, 8, 36, ">", b->history_index + 1 < b->history_count ? 1 : 0);
    gui_button(g, 84, 8, 52, "Reload", 1);
    gui_button(g, 140, 8, 44, "Home", 1);
    gui_rect(g, 190, 8, g->width - 244, 30, b->address_focus ? 0x65a9ea : 0x768a9e);
    gui_rect(g, 192, 10, g->width - 248, 26, 0xffffff);
    int chars = (g->width - 264) / 8;
    if (chars < 1) {
        chars = 1;
    }
    size_t length = strlen(b->address), start = length > (size_t)chars ? length - chars : 0;
    gui_text(g, 198, 15, b->address + start, b->selected ? 0x4469ac : 0x172a40);
    gui_button(g, g->width - 48, 8, 44, "Go", 1);
    gui_rect(g, 0, g->height - 24, g->width, 24, 0xe3eaf2);
    gui_text(g, 8, g->height - 19, b->status, 0x263a50);
    gui_present(g);
    b->dirty = 0;
}
static void history(struct browser *b, int delta) {
    int target = b->history_index + delta;
    if (b->worker > 0 || target < 0 || target >= b->history_count) {
        return;
    }
    b->history_target = target;
    navigate(b, b->history[target], 0);
}
static int encode(char *out, size_t capacity, const char *text) {
    size_t n = strlen(out);
    const char *hex = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (n + 4 >= capacity) {
            return -1;
        }
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            strchr("-_.~", *p)) {
            out[n++] = *p;
        } else {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    out[n] = 0;
    return 0;
}
static void submit(struct browser *b, int node) {
    struct b_document *d = b->document;
    int form = node;
    while (form > 0 && strcmp(d->nodes[form].tag, "form")) {
        form = d->nodes[form].parent;
    }
    if (form <= 0) {
        return;
    }
    const char *method = b_attr(d, form, "method");
    if (*method && strcasecmp(method, "get")) {
        message(b, "Only GET forms are supported");
        return;
    }
    if (*b_attr(d, form, "onsubmit")) {
        b_js_run(d, b_attr(d, form, "onsubmit"));
        if (*d->message) {
            message(b, d->message);
        }
        b->dirty = 1;
        return;
    }
    char url[B_URL];
    const char *action = b_attr(d, form, "action");
    if (b_url(b->current, *action ? action : b->current, url, sizeof(url))) {
        message(b, "Invalid form action");
        return;
    }
    char *fragment = strchr(url, '#');
    if (fragment) {
        *fragment = 0;
    }
    size_t len = strlen(url);
    if (len + 2 >= sizeof(url)) {
        message(b, "Form URL too long");
        return;
    }
    url[len++] = strchr(url, '?') ? '&' : '?';
    url[len] = 0;
    for (int i = 0; i < d->count; i++) {
        if (strcmp(d->nodes[i].tag, "input") || !*b_attr(d, i, "name")) {
            continue;
        }
        int ancestor = d->nodes[i].parent;
        while (ancestor >= 0 && ancestor != form) {
            ancestor = d->nodes[ancestor].parent;
        }
        if (ancestor != form) {
            continue;
        }
        const char *type = b_attr(d, i, "type");
        if (!strcmp(type, "submit") && i != node) {
            continue;
        }
        if (url[strlen(url) - 1] != '?' && url[strlen(url) - 1] != '&') {
            strcat(url, "&");
        }
        if (encode(url, sizeof(url), b_attr(d, i, "name"))) {
            message(b, "Form URL too long");
            return;
        }
        if (strlen(url) + 2 >= sizeof(url)) {
            message(b, "Form URL too long");
            return;
        }
        strcat(url, "=");
        if (encode(url, sizeof(url), b_attr(d, i, "value"))) {
            message(b, "Form URL too long");
            return;
        }
    }
    navigate(b, url, 1);
}
static void click(struct browser *b, int x, int y) {
    b->dirty = 1;
    if (y < 48) {
        if (x < 40) {
            history(b, -1);
        } else if (x < 80) {
            history(b, 1);
        } else if (x < 136 && *b->current) {
            navigate(b, b->current, 0);
        } else if (x < 184) {
            navigate(b, "about:home", 0);
        } else if (x >= b->gui.width - 48) {
            navigate(b, b->address, 0);
        } else {
            b->address_focus = 1;
            b->selected = 1;
        }
        return;
    }
    b->address_focus = 0;
    int node = b_hit(b->document, x, y);
    b->document->focused = -1;
    if (node < 0) {
        return;
    }
    const char *event = b_attr(b->document, node, "onclick");
    if (*event) {
        b_js_run(b->document, event);
        if (*b->document->message) {
            message(b, b->document->message);
        }
        return;
    }
    if (!strcmp(b->document->nodes[node].tag, "a")) {
        const char *href = b_attr(b->document, node, "href");
        if (*href) {
            navigate(b, href, 1);
        }
    } else if (!strcmp(b->document->nodes[node].tag, "input") &&
               strcmp(b_attr(b->document, node, "type"), "submit")) {
        b->document->focused = node;
    } else if (!strcmp(b->document->nodes[node].tag, "button") ||
               !strcmp(b_attr(b->document, node, "type"), "submit")) {
        submit(b, node);
    }
}
static void key(struct browser *b, unsigned code) {
    if (code == 17) {
        b->running = 0;
        return;
    }
    if (code == 12) {
        b->address_focus = 1;
        b->selected = 1;
        b->dirty = 1;
        return;
    }
    if (code == 18 && *b->current) {
        navigate(b, b->current, 0);
        return;
    }
    if (code == 27) {
        b->address_focus = 0;
        b->document->focused = -1;
        b->dirty = 1;
        return;
    }
    int focus = b->document->focused;
    if (b->address_focus || focus >= 0) {
        char text[B_URL];
        size_t size = b->address_focus ? sizeof(b->address) : 512;
        b_copy(text, size, b->address_focus ? b->address : b_attr(b->document, focus, "value"));
        size_t n = strlen(text);
        if (code == 10 || code == 13) {
            if (b->address_focus) {
                navigate(b, b->address, 0);
            } else {
                submit(b, focus);
            }
            return;
        }
        if (code == 8 || code == 127) {
            if (b->address_focus && b->selected) {
                n = 0;
            } else if (n) {
                n--;
            }
            text[n] = 0;
        } else if (code >= 32 && code < 127 && n + 1 < size) {
            if (b->address_focus && b->selected) {
                n = 0;
            }
            text[n++] = code;
            text[n] = 0;
        } else {
            return;
        }
        b->selected = 0;
        if (b->address_focus) {
            b_copy(b->address, sizeof(b->address), text);
        } else {
            b_set_attr(b->document, focus, "value", text);
        }
    } else if (code == KWM_DOWN) {
        b->document->scroll += 32;
    } else if (code == KWM_UP) {
        b->document->scroll -= 32;
    }
    b->dirty = 1;
}
int main(int argc, char **argv) {
    struct browser *b = calloc(1, sizeof(*b));
    if (!b) {
        return 1;
    }
    b->document = calloc(1, sizeof(*b->document));
    if (!b->document) {
        free(b);
        return 1;
    }
    b->pipe_fd = -1;
    b->history_index = b->history_target = -1;
    b->document->focused = -1;
    b->document->count = 1;
    b->document->nodes[0].first = b->document->nodes[0].last = b->document->nodes[0].next =
        b->document->nodes[0].parent = b->document->nodes[0].attr = -1;
    b_copy(b->document->nodes[0].tag, sizeof(b->document->nodes[0].tag), "document");
    if (gui_open(&b->gui, "KalyBrowser", 80, 72, 864, 640)) {
        perror("KalyBrowser");
        free(b->document);
        free(b);
        return 1;
    }
    b->running = b->dirty = 1;
    navigate(b, argc > 1 ? argv[1] : "about:home", 0);
    while (b->running) {
        receive_page(b);
        struct kwm_event event;
        for (unsigned i = 0; i < 64 && !gui_event(&b->gui, &event); i++) {
            if (event.type == KWM_CLOSE) {
                b->running = 0;
            } else if (event.type == KWM_SIZE) {
                if (gui_resize_buffer(&b->gui, event.width, event.height)) {
                    b->running = 0;
                } else {
                    b->dirty = 1;
                }
            } else if (event.type == KWM_KEY) {
                key(b, event.code);
            } else if (event.type == KWM_WHEEL) {
                b->document->scroll -= (int)event.code * 48;
                b->dirty = 1;
            } else if (event.type == KWM_MOUSE) {
                if ((event.buttons & 1) && !(b->buttons & 1)) {
                    click(b, event.x, event.y);
                }
                b->buttons = event.buttons;
            }
        }
        if (*b->document->navigate) {
            char target[B_URL];
            b_copy(target, sizeof(target), b->document->navigate);
            b->document->navigate[0] = 0;
            navigate(b, target, 1);
        }
        if (b->dirty) {
            redraw(b);
        }
        gui_pause();
    }
    if (b->pipe_fd >= 0) {
        close(b->pipe_fd);
    }
    free(b->source);
    b_js_free(b->document);
    gui_close(&b->gui);
    free(b->document);
    free(b);
    return 0;
}
