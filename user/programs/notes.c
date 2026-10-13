#include <gui.h>
#include <kaly/theme.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define TEXT_LIMIT (1024UL * 1024)
static char *text;
static size_t length, cursor;
static int scroll, path_focus, modified, modal, running = 1;
static unsigned buttons;
static char path[1024], document_path[1024],
    message[256] = "Enter a path above | Ctrl+S: save | arrows: move";
static size_t previous(size_t pos) {
    if (pos) {
        pos--;
        while (pos && ((uint8_t)text[pos] & 0xc0) == 0x80) {
            pos--;
        }
    }
    return pos;
}
static size_t next(size_t pos) {
    if (pos < length) {
        pos++;
        while (pos < length && ((uint8_t)text[pos] & 0xc0) == 0x80) {
            pos++;
        }
    }
    return pos;
}
static void error(const char *action) {
    snprintf(message, sizeof(message), "%s: %s", action, strerror(errno));
}
static void update_title(struct gui *g) {
    char title[64];
    const char *leaf = strrchr(path, '/');
    snprintf(title, sizeof(title), "%s%s", modified ? "* " : "",
             *path ? (leaf ? leaf + 1 : path) : "Notes");
    gui_title(g, title);
}
static int load(struct gui *g) {
    if (!*path) {
        snprintf(message, sizeof(message), "Enter a path to open");
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        error("Open");
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st)) {
        error("Stat");
        close(fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > (long)TEXT_LIMIT) {
        close(fd);
        snprintf(message, sizeof(message), "Requires a regular text file up to 1 MiB");
        return -1;
    }
    char *replacement = malloc(TEXT_LIMIT + 1);
    if (!replacement) {
        error("Memory");
        close(fd);
        return -1;
    }
    size_t total = 0;
    ssize_t n = 0;
    while (total < TEXT_LIMIT && (n = read(fd, replacement + total, TEXT_LIMIT - total)) > 0) {
        total += n;
    }
    if (n < 0) {
        error("Read");
        free(replacement);
        close(fd);
        return -1;
    }
    if (total == TEXT_LIMIT) {
        char extra;
        n = read(fd, &extra, 1);
        if (n != 0) {
            if (n < 0) {
                error("Read");
            } else {
                snprintf(message, sizeof(message), "File grew beyond 1 MiB");
            }
            free(replacement);
            close(fd);
            return -1;
        }
    }
    close(fd);
    for (size_t i = 0; i < total; i++) {
        unsigned c = (uint8_t)replacement[i];
        if (!c || (c < 32 && c != '\n' && c != '\r' && c != '\t')) {
            free(replacement);
            snprintf(message, sizeof(message), "Binary file: use hexdump in Terminal");
            return -1;
        }
    }
    replacement[total] = 0;
    free(text);
    text = replacement;
    length = total;
    cursor = 0;
    scroll = 0;
    modified = 0;
    strcpy(document_path, path);
    path_focus = 0;
    snprintf(message, sizeof(message), "Loaded %lu bytes", length);
    update_title(g);
    return 0;
}
static int save(struct gui *g) {
    if (!*path) {
        path_focus = 1;
        snprintf(message, sizeof(message), "Enter a path before saving");
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error("Save");
        return -1;
    }
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, text + done, length - done);
        if (n <= 0) {
            if (!n) {
                errno = EIO;
            }
            error("Save incomplete; buffer retained");
            close(fd);
            return -1;
        }
        done += n;
    }
    if (close(fd)) {
        error("Close saved file");
        return -1;
    }
    modified = 0;
    strcpy(document_path, path);
    snprintf(message, sizeof(message), "Saved %lu bytes to %s", length, path);
    update_title(g);
    return 0;
}
static unsigned decode(size_t *offset) {
    unsigned code = (uint8_t)text[(*offset)++];
    if (code >= 0xc2 && code <= 0xf4) {
        unsigned n = code < 0xe0 ? 1 : code < 0xf0 ? 2 : 3;
        code &= n == 1 ? 31 : n == 2 ? 15 : 7;
        while (n-- && *offset < length && ((uint8_t)text[*offset] & 0xc0) == 0x80) {
            code = (code << 6) | ((uint8_t)text[(*offset)++] & 63);
        }
    }
    return code;
}
static void position(struct gui *g, size_t stop, int *line, int *column) {
    int cols = (g->width - 20) / g->font_width;
    *line = *column = 0;
    for (size_t i = 0; i < stop;) {
        unsigned c = decode(&i);
        if (c == '\n') {
            (*line)++;
            *column = 0;
        } else if (c != '\r') {
            *column += c == '\t' ? 4 - *column % 4 : 1;
            if (*column >= cols) {
                (*line)++;
                *column = 0;
            }
        }
    }
}
static size_t locate(struct gui *g, int row, int col) {
    int cols = (g->width - 20) / g->font_width, y = 0, x = 0;
    for (size_t i = 0; i < length;) {
        if (y > row || (y == row && x >= col)) {
            return i;
        }
        unsigned c = decode(&i);
        if (c == '\n') {
            if (y == row) {
                return previous(i);
            }
            y++;
            x = 0;
        } else if (c != '\r') {
            x += c == '\t' ? 4 - x % 4 : 1;
            if (x >= cols) {
                y++;
                x = 0;
            }
        }
    }
    return length;
}
static int draw(struct gui *g) {
    gui_rect(g, 0, 0, g->width, g->height, THEME_NOTES_BG);
    gui_button(g, 6, 5, 60, "Open", 0);
    gui_button(g, 72, 5, 60, "Save", 0);
    gui_rect(g, 140, 5, g->width - 146, 26, path_focus ? THEME_NOTES_PATH_BG_FOCUS : THEME_NOTES_PATH_BG);
    gui_text(g, 145, 10, *path ? path : "/home/note.txt", THEME_NOTES_PATH_FG);
    int y = 0, x = 0, columns = (g->width - 20) / g->font_width;
    int rows = (g->height - 76) / g->font_height;
    for (size_t i = 0; i < length;) {
        unsigned c = decode(&i);
        if (c == '\n') {
            y++;
            x = 0;
        } else if (c != '\r') {
            if (y >= scroll && y < scroll + rows && c != '\t') {
                gui_glyph(g, 10 + x * g->font_width, 43 + (y - scroll) * g->font_height, c,
                          THEME_NOTES_TEXT);
            }
            x += c == '\t' ? 4 - x % 4 : 1;
            if (x >= columns) {
                y++;
                x = 0;
            }
        }
        if (y >= scroll + rows) {
            break;
        }
    }
    int cy, cx;
    position(g, cursor, &cy, &cx);
    if (!path_focus && cy >= scroll && cy < scroll + rows) {
        gui_rect(g, 10 + cx * g->font_width, 43 + (cy - scroll) * g->font_height, 2, g->font_height,
                 THEME_NOTES_CURSOR);
    }
    gui_rect(g, 0, g->height - 28, g->width, 28, THEME_NOTES_STATUS_BG);
    gui_text(g, 8, g->height - 22, message, THEME_NOTES_STATUS_FG);
    if (modal) {
        int y0 = g->height / 2 - 52;
        gui_rect(g, 10, y0, g->width - 20, 108, THEME_NOTES_MODAL_BG);
        gui_text(g, 20, y0 + 12, "Unsaved changes", THEME_NOTES_MODAL_TITLE);
        gui_text(g, 20, y0 + 34, "Save before continuing?", THEME_NOTES_MODAL_TEXT);
        gui_button(g, 20, y0 + 68, 74, "Save", 1);
        gui_button(g, 102, y0 + 68, 90, "Discard", 0);
        gui_button(g, 200, y0 + 68, 82, "Cancel", 0);
    }
    return gui_present(g);
}
static void continue_action(struct gui *g, int decision) {
    int action = modal;
    modal = 0;
    if (decision == 2) {
        return;
    }
    if (decision == 0) {
        if (action == 2 && !*document_path) {
            snprintf(message, sizeof(message), "Save the new document before opening another file");
            return;
        }
        char requested_path[1024];
        strcpy(requested_path, path);
        if (*document_path) {
            strcpy(path, document_path);
        }
        int result = save(g);
        strcpy(path, requested_path);
        if (result) {
            return;
        }
    }
    if (action == 1) {
        running = 0;
    } else {
        load(g);
    }
}
int main(int argc, char **argv) {
    struct gui g;
    if (gui_open(&g, "Notes", 260, 430, 550, 290)) {
        perror("notes window");
        return 1;
    }
    text = malloc(TEXT_LIMIT + 1);
    if (!text) {
        gui_close(&g);
        return 1;
    }
    text[0] = 0;
    if (argc > 1) {
        if (strlen(argv[1]) >= sizeof(path)) {
            snprintf(message, sizeof(message), "Path too long");
        } else {
            strcpy(path, argv[1]);
            load(&g);
        }
    }
    int changed = 1;
    while (running) {
        struct kwm_event e;
        for (unsigned i = 0; i < 128 && !gui_event(&g, &e); i++) {
            changed = 1;
            int ensure_visible = 0;
            if (e.type == KWM_CLOSE) {
                if (modified) {
                    modal = 1;
                } else {
                    running = 0;
                }
            } else if (e.type == KWM_SIZE) {
                if (gui_resize_buffer(&g, e.width, e.height)) {
                    error("Resize");
                    running = 0;
                }
            } else if (e.type == KWM_OVERFLOW) {
                buttons = 0;
                snprintf(message, sizeof(message), "Input overflow; buffer retained");
            } else if (e.type == KWM_MOUSE) {
                int down = (e.buttons & 1) && !(buttons & 1);
                buttons = e.buttons;
                if (down && modal) {
                    int y = g.height / 2 - 52;
                    if (e.y >= y + 68 && e.y < y + 94) {
                        if (e.x >= 20 && e.x < 94) {
                            continue_action(&g, 0);
                        } else if (e.x >= 102 && e.x < 192) {
                            continue_action(&g, 1);
                        } else if (e.x >= 200 && e.x < 282) {
                            continue_action(&g, 2);
                        }
                    }
                } else if (down && e.y >= 5 && e.y < 31) {
                    if (e.x >= 6 && e.x < 66) {
                        if (modified) {
                            modal = 2;
                        } else {
                            load(&g);
                        }
                    } else if (e.x >= 72 && e.x < 132) {
                        save(&g);
                    } else if (e.x >= 140) {
                        path_focus = 1;
                    }
                } else if (down && e.x >= 10 && e.y >= 43 && e.y < g.height - 28) {
                    path_focus = 0;
                    cursor =
                        locate(&g, scroll + (e.y - 43) / g.font_height, (e.x - 10) / g.font_width);
                    ensure_visible = 1;
                }
            } else if (e.type == KWM_WHEEL && !modal) {
                scroll -= (int)e.code * 3;
                if (scroll < 0) {
                    scroll = 0;
                }
            } else if (e.type == KWM_KEY) {
                unsigned key = e.code;
                if (modal) {
                    if (key == 's' || key == '\n') {
                        continue_action(&g, 0);
                    } else if (key == 'd') {
                        continue_action(&g, 1);
                    } else if (key == 'c' || key == 27) {
                        continue_action(&g, 2);
                    }
                } else if (key == 19) {
                    save(&g);
                } else if (path_focus) {
                    size_t n = strlen(path);
                    if (key == '\b' && n) {
                        path[n - 1] = 0;
                    } else if (key == '\n' || key == '\t') {
                        path_focus = 0;
                    } else if (key >= 32 && key < 127 && n + 1 < sizeof(path)) {
                        path[n] = key;
                        path[n + 1] = 0;
                    }
                } else if (key == KWM_LEFT) {
                    cursor = previous(cursor);
                    ensure_visible = 1;
                } else if (key == KWM_RIGHT) {
                    cursor = next(cursor);
                    ensure_visible = 1;
                } else if (key == KWM_UP || key == KWM_DOWN) {
                    int y, x;
                    position(&g, cursor, &y, &x);
                    cursor = locate(&g, key == KWM_UP ? (y > 0 ? y - 1 : 0) : y + 1, x);
                    ensure_visible = 1;
                } else if (key == '\b' && cursor) {
                    size_t start = previous(cursor);
                    memmove(text + start, text + cursor, length - cursor + 1);
                    length -= cursor - start;
                    cursor = start;
                    modified = 1;
                    ensure_visible = 1;
                    update_title(&g);
                } else if (key == 127 && cursor < length) {
                    size_t end = next(cursor);
                    memmove(text + cursor, text + end, length - end + 1);
                    length -= end - cursor;
                    modified = 1;
                    update_title(&g);
                } else if (key == '\n' || key == '\t' || (key >= 32 && key < 127)) {
                    if (length == TEXT_LIMIT) {
                        snprintf(message, sizeof(message), "1 MiB editing limit reached");
                    } else {
                        memmove(text + cursor + 1, text + cursor, length - cursor + 1);
                        text[cursor++] = key;
                        length++;
                        modified = 1;
                        ensure_visible = 1;
                        update_title(&g);
                    }
                }
            }
            if (ensure_visible) {
                int y, x;
                position(&g, cursor, &y, &x);
                int rows = (g.height - 76) / g.font_height;
                if (rows < 1) {
                    rows = 1;
                }
                if (y < scroll) {
                    scroll = y;
                } else if (y >= scroll + rows) {
                    scroll = y - rows + 1;
                }
            }
        }
        if (changed) {
            if (draw(&g)) {
                perror("notes present");
                running = 0;
            }
            changed = 0;
        }
        gui_pause();
    }
    free(text);
    gui_close(&g);
    return 0;
}
