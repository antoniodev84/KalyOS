#include <gui.h>
#include <kaly/theme.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
struct entry {
    char name[256];
    unsigned mode;
    long size;
};
static struct entry *entries;
static size_t count, capacity;
static int selected = -1, scroll, modal, running = 1;
static unsigned buttons;
static char path[1024] = "/", message[256], name[256], clipboard[1024];
static int full_path(char *out, const char *leaf) {
    int n = snprintf(out, 1024, "%s%s%s", path, strcmp(path, "/") ? "/" : "", leaf);
    if (n < 0 || n >= 1024) {
        snprintf(message, sizeof(message), "Path too long");
        return -1;
    }
    return 0;
}
static void error(const char *action) {
    snprintf(message, sizeof(message), "%s: %s", action, strerror(errno));
}
static int compare(const struct entry *a, const struct entry *b) {
    int ad = S_ISDIR(a->mode), bd = S_ISDIR(b->mode);
    return ad != bd ? bd - ad : strcmp(a->name, b->name);
}
static void load(void) {
    DIR *dir = opendir(path);
    if (!dir) {
        error("Open directory");
        return;
    }
    count = 0;
    selected = -1;
    scroll = 0;
    struct dirent *d;
    int failure = 0;
    errno = 0;
    while ((d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) {
            continue;
        }
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            if (next > 16384) {
                snprintf(message, sizeof(message), "Directory exceeds 16384 entries");
                failure = 1;
                break;
            }
            struct entry *p = realloc(entries, next * sizeof(*p));
            if (!p) {
                error("Directory memory");
                failure = 1;
                break;
            }
            entries = p;
            capacity = next;
        }
        struct entry *item = &entries[count];
        strncpy(item->name, d->d_name, sizeof(item->name) - 1);
        item->name[sizeof(item->name) - 1] = 0;
        char full[1024];
        struct stat st;
        if (full_path(full, item->name) || lstat(full, &st)) {
            error("File metadata");
            failure = 1;
            continue;
        }
        item->mode = st.st_mode;
        item->size = st.st_size;
        count++;
        errno = 0;
    }
    if (!failure && errno) {
        error("Read directory");
        failure = 1;
    }
    closedir(dir);
    for (size_t i = 1; i < count; i++) {
        struct entry value = entries[i];
        size_t j = i;
        while (j && compare(&value, &entries[j - 1]) < 0) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = value;
    }
    if (!failure) {
        snprintf(message, sizeof(message), "%lu entries | Enter: open | Del: delete", count);
    }
}
static void parent(void) {
    char *slash = strrchr(path, '/');
    if (slash == path) {
        path[1] = 0;
    } else if (slash) {
        *slash = 0;
    }
    load();
}
static void open_selected(void) {
    if (selected < 0 || (size_t)selected >= count) {
        return;
    }
    char full[1024];
    if (full_path(full, entries[selected].name)) {
        return;
    }
    struct stat st;
    if (stat(full, &st)) {
        error("Open");
    } else if (S_ISDIR(st.st_mode)) {
        strcpy(path, full);
        load();
    } else if (S_ISREG(st.st_mode)) {
        if (gui_launch("/bin/notes", full) < 0) {
            error("Launch editor");
        }
    } else {
        snprintf(message, sizeof(message), "Only directories and regular text files can be opened");
    }
}
static void copy_file(void) {
    if (!*clipboard) {
        snprintf(message, sizeof(message), "Select a regular file and click Copy first");
        return;
    }
    const char *leaf = strrchr(clipboard, '/');
    char dest[1024];
    if (!leaf || full_path(dest, leaf + 1)) {
        return;
    }
    int source = open(clipboard, O_RDONLY);
    if (source < 0) {
        error("Copy source");
        return;
    }
    struct stat st;
    if (fstat(source, &st) || !S_ISREG(st.st_mode)) {
        close(source);
        snprintf(message, sizeof(message), "Copy requires a regular file");
        return;
    }
    int target = open(dest, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (target < 0) {
        error("Copy destination");
        close(source);
        return;
    }
    char buffer[4096];
    int failed = 0, saved = 0;
    ssize_t n;
    while ((n = read(source, buffer, sizeof(buffer))) > 0) {
        ssize_t done = 0;
        while (done < n) {
            ssize_t written = write(target, buffer + done, n - done);
            if (written <= 0) {
                failed = 1;
                saved = written < 0 ? errno : EIO;
                break;
            }
            done += written;
        }
        if (failed) {
            break;
        }
    }
    if (n < 0) {
        failed = 1;
        saved = errno;
    }
    if (close(target) && !failed) {
        failed = 1;
        saved = errno;
    }
    close(source);
    if (failed) {
        if (unlink(dest)) {
            snprintf(message, sizeof(message), "Copy failed; partial file remains: %s",
                     strerror(saved));
        } else {
            errno = saved;
            error("Copy");
        }
    } else {
        load();
        snprintf(message, sizeof(message), "Copied %s", leaf + 1);
    }
}
static void action(int which) {
    if (which == 0) {
        parent();
    } else if (which == 1) {
        strcpy(path, "/home");
        load();
    } else if (which == 2) {
        load();
    } else if (which == 3 || which == 4) {
        modal = which == 3 ? 1 : 2;
        name[0] = 0;
    } else if (which == 5) {
        if (selected >= 0) {
            modal = 3;
        }
    } else if (which == 6) {
        if (selected >= 0 && S_ISREG(entries[selected].mode)) {
            if (!full_path(clipboard, entries[selected].name)) {
                snprintf(message, sizeof(message), "Copied path; navigate and click Paste");
            }
        } else {
            snprintf(message, sizeof(message), "Select a regular file to copy");
        }
    } else if (which == 7) {
        copy_file();
    } else if (which == 8) {
        open_selected();
    }
}
static void confirm(void) {
    char full[1024];
    if (modal == 3) {
        if (selected >= 0 && !full_path(full, entries[selected].name)) {
            int e = S_ISDIR(entries[selected].mode) ? rmdir(full) : unlink(full);
            modal = 0;
            if (e) {
                error("Delete");
            } else {
                load();
            }
        }
        return;
    }
    if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        snprintf(message, sizeof(message), "Enter a filename without slashes");
        return;
    }
    if (full_path(full, name)) {
        return;
    }
    int e;
    if (modal == 2) {
        e = mkdir(full, 0755);
    } else {
        int fd = open(full, O_WRONLY | O_CREAT | O_EXCL, 0644);
        e = fd < 0 ? -1 : close(fd);
    }
    modal = 0;
    if (e) {
        error("Create");
    } else {
        load();
    }
}
static int draw(struct gui *g) {
    gui_rect(g, 0, 0, g->width, g->height, THEME_FILES_BG);
    const char *labels[] = {"Up",  "Home", "Reload", "File+", "Dir+",
                            "Del", "Copy", "Paste",  "Open"};
    int button_width = (g->width - 12) / 9;
    for (int i = 0; i < 9; i++) {
        gui_button(g, 6 + i * button_width, 5, button_width - 3, labels[i], 0);
    }
    gui_rect(g, 6, 37, g->width - 12, 25, THEME_FILES_PATH_BG);
    gui_text(g, 12, 42, path, THEME_FILES_PATH_FG);
    int rows = (g->height - 100) / 24;
    if (rows < 1) {
        rows = 1;
    }
    for (int i = 0; i < rows && (size_t)(scroll + i) < count; i++) {
        int index = scroll + i, y = 68 + i * 24;
        struct entry *item = &entries[index];
        if (index == selected) {
            gui_rect(g, 6, y, g->width - 12, 23, THEME_FILES_SELECTION);
        }
        char label[300];
        snprintf(label, sizeof(label), "%s %s",
                 S_ISDIR(item->mode)   ? "[D]"
                 : S_ISREG(item->mode) ? "[F]"
                 : S_ISLNK(item->mode) ? "[L]"
                                       : "[?]",
                 item->name);
        gui_text(g, 12, y + 3, label, S_ISDIR(item->mode) ? THEME_FILES_DIR_FG : THEME_FILES_FILE_FG);
        char size[32];
        snprintf(size, sizeof(size), "%ld", item->size);
        if ((int)strlen(label) * g->font_width < g->width - 110) {
            gui_text(g, g->width - 95, y + 3, size, THEME_FILES_SIZE_FG);
        }
    }
    gui_rect(g, 0, g->height - 28, g->width, 28, THEME_FILES_STATUS_BG);
    gui_text(g, 8, g->height - 23, message, THEME_FILES_STATUS_FG);
    if (modal) {
        int w = g->width - 24, x = 12, y = g->height / 2 - 64;
        gui_rect(g, x, y, w, 125, THEME_FILES_MODAL_BORDER);
        gui_rect(g, x + 2, y + 2, w - 4, 121, THEME_FILES_MODAL_BG);
        gui_text(g, x + 12, y + 12,
                 modal == 1   ? "New file name"
                 : modal == 2 ? "New directory name"
                              : "Delete selected entry?",
                 THEME_FILES_MODAL_TEXT);
        gui_rect(g, x + 10, y + 39, w - 20, 28, THEME_FILES_MODAL_INPUT);
        gui_text(g, x + 15, y + 44, modal == 3 ? entries[selected].name : name, THEME_FILES_MODAL_TEXT);
        gui_button(g, x + 12, y + 82, 110, "Confirm", 1);
        gui_button(g, x + 132, y + 82, 100, "Cancel", 0);
    }
    return gui_present(g);
}
int main(int argc, char **argv) {
    struct gui g;
    if (argc > 1 && strlen(argv[1]) < sizeof(path)) {
        strcpy(path, argv[1]);
    }
    if (gui_open(&g, "Files", 470, 82, 530, 400)) {
        perror("files window");
        return 1;
    }
    load();
    int changed = 1;
    while (running) {
        struct kwm_event e;
        for (unsigned i = 0; i < 128 && !gui_event(&g, &e); i++) {
            changed = 1;
            if (e.type == KWM_CLOSE) {
                running = 0;
            } else if (e.type == KWM_SIZE) {
                if (gui_resize_buffer(&g, e.width, e.height)) {
                    error("Resize");
                    running = 0;
                }
            } else if (e.type == KWM_OVERFLOW) {
                buttons = 0;
                snprintf(message, sizeof(message), "Input overflow; click again");
            } else if (e.type == KWM_MOUSE) {
                int down = (e.buttons & 1) && !(buttons & 1);
                buttons = e.buttons;
                if (down && modal) {
                    int y = g.height / 2 - 64;
                    if (e.y >= y + 82 && e.y < y + 108) {
                        if (e.x >= 24 && e.x < 134) {
                            confirm();
                        } else if (e.x >= 144 && e.x < 244) {
                            modal = 0;
                        }
                    }
                } else if (down && e.y >= 5 && e.y < 31 && e.x >= 6) {
                    int which = (e.x - 6) / ((g.width - 12) / 9);
                    if (which < 9) {
                        action(which);
                    }
                } else if (down && e.y >= 68 && e.y < g.height - 32 && e.x >= 0) {
                    int index = scroll + (e.y - 68) / 24;
                    if ((size_t)index < count) {
                        if (selected == index) {
                            open_selected();
                        } else {
                            selected = index;
                        }
                    }
                }
            } else if (e.type == KWM_WHEEL && !modal) {
                scroll -= (int)e.code * 3;
            } else if (e.type == KWM_KEY) {
                if (modal) {
                    size_t n = strlen(name);
                    if (e.code == 27) {
                        modal = 0;
                    } else if (e.code == '\n') {
                        confirm();
                    } else if (e.code == '\b' && n && modal != 3) {
                        name[n - 1] = 0;
                    } else if (e.code >= 32 && e.code < 127 && n + 1 < sizeof(name) && modal != 3) {
                        name[n] = e.code;
                        name[n + 1] = 0;
                    }
                } else if (e.code == '\n') {
                    open_selected();
                } else if (e.code == '\b') {
                    parent();
                } else if (e.code == 127) {
                    action(5);
                } else if (e.code == KWM_DOWN && selected + 1 < (int)count) {
                    selected++;
                    int visible = (g.height - 100) / 24;
                    if (visible < 1) {
                        visible = 1;
                    }
                    if (selected >= scroll + visible) {
                        scroll = selected - visible + 1;
                    }
                } else if (e.code == KWM_UP && selected > 0) {
                    selected--;
                    if (selected < scroll) {
                        scroll = selected;
                    }
                }
            }
        }
        int rows = (g.height - 100) / 24;
        if (rows < 1) {
            rows = 1;
        }
        int maximum = (int)count > rows ? (int)count - rows : 0;
        if (scroll < 0) {
            scroll = 0;
        }
        if (scroll > maximum) {
            scroll = maximum;
        }
        if (changed) {
            if (draw(&g)) {
                perror("files present");
                running = 0;
            }
            changed = 0;
        }
        while (waitpid(-1, NULL, WNOHANG) > 0) {
        }
        gui_pause();
    }
    free(entries);
    gui_close(&g);
    return 0;
}
