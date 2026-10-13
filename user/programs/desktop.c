#include <gui.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
struct application {
    struct kwm_application item;
    char path[512], name[256];
};
struct catalog {
    struct application *apps;
    unsigned count, generation;
};
static struct catalog current, previous;
static int launch(const char *program, unsigned app) {
    if (!app && getpid() == 3) {
        int pid = gui_launch_system(program, NULL, 4);
        if (pid >= 0 || errno != 16) {
            return pid;
        }
    }
    return gui_launch(program, NULL);
}
static int duplicate(const struct catalog *catalog, const char *name) {
    for (unsigned i = 0; i < catalog->count; i++) {
        if (!strcmp(catalog->apps[i].name, name)) {
            return 1;
        }
    }
    return 0;
}
static int executable(const char *path) {
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & 0111)) {
        return 0;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    unsigned char magic[20];
    ssize_t n = read(fd, magic, sizeof(magic));
    close(fd);
    return (n == 20 && !memcmp(magic, "\177ELF\2\1\1", 7) && (magic[16] == 2 || magic[16] == 3) &&
            magic[17] == 0 && magic[18] == 62 && magic[19] == 0) ||
           (n >= 2 && magic[0] == '#' && magic[1] == '!');
}
static int publish(struct gui *g, const struct catalog *catalog, const char *status) {
    struct kwm_application *items = malloc(catalog->count * sizeof(*items));
    if (!items) {
        return -1;
    }
    for (unsigned i = 0; i < catalog->count; i++) {
        items[i] = catalog->apps[i].item;
    }
    struct kwm_request r = {.operation = KWM_APPLICATIONS,
                            .data = (uintptr_t)items,
                            .length = catalog->count * sizeof(*items),
                            .argument = catalog->generation};
    strncpy(r.title, status, sizeof(r.title) - 1);
    int result = gui_request(g, &r);
    free(items);
    return result;
}
static int refresh(struct gui *g) {
    struct catalog next = {.generation = current.generation + 1};
    if (!next.generation) {
        errno = EINVAL;
        return -1;
    }
    next.apps = calloc(KWM_APP_LIMIT, sizeof(*next.apps));
    if (!next.apps) {
        return -1;
    }
    const char *names[] = {"terminal", "files", "notes"};
    const char *labels[] = {"Terminal", "Files", "Notepad"};
    for (unsigned i = 0; i < 3; i++) {
        struct application *app = &next.apps[next.count++];
        app->item.id = i;
        app->item.flags = KWM_APP_SYSTEM;
        strcpy(app->item.label, labels[i]);
        strcpy(app->name, names[i]);
        snprintf(app->path, sizeof(app->path), "/bin/%s", names[i]);
    }
    const char *path = getenv("PATH");
    if (!path) {
        path = "/bin";
    } else if (!*path) {
        path = ".";
    }
    int limited = 0, errors = 0;
    while (*path) {
        const char *separator = strchr(path, ':');
        size_t size = separator ? (size_t)(separator - path) : strlen(path);
        char directory[512];
        if (size >= sizeof(directory)) {
            fprintf(stderr, "desktop PATH: directory name too long\n");
            errors = 1;
        } else {
            memcpy(directory, path, size);
            directory[size] = 0;
            if (!size) {
                strcpy(directory, ".");
            }
            DIR *dir = opendir(directory);
            if (!dir) {
                if (errno != ENOENT) {
                    perror(directory);
                    errors = 1;
                }
            } else {
                struct dirent *entry;
                for (;;) {
                    errno = 0;
                    entry = readdir(dir);
                    if (!entry) {
                        if (errno) {
                            perror(directory);
                            errors = 1;
                        }
                        break;
                    }
                    const char *name = entry->d_name;
                    if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, "init") ||
                        !strcmp(name, "sh") || !strcmp(name, "desktop") ||
                        !strcmp(name, "kalybox") || duplicate(&next, name)) {
                        continue;
                    }
                    char candidate[512];
                    int n = snprintf(candidate, sizeof(candidate), "%s%s%s", directory,
                                     size && directory[size - 1] == '/' ? "" : "/", name);
                    if (n < 0 || (size_t)n >= sizeof(candidate)) {
                        errors = 1;
                        continue;
                    }
                    if (!executable(candidate)) {
                        continue;
                    }
                    if (next.count == KWM_APP_LIMIT) {
                        limited = 1;
                        continue;
                    }
                    struct application *app = &next.apps[next.count++];
                    strcpy(app->path, candidate);
                    strncpy(app->name, name, sizeof(app->name) - 1);
                    strncpy(app->item.label, name, sizeof(app->item.label) - 1);
                }
                if (closedir(dir)) {
                    perror(directory);
                    errors = 1;
                }
            }
        }
        if (!separator) {
            break;
        }
        path = separator + 1;
        if (!*path) {
            path = ".";
        }
    }
    for (unsigned i = 4; i < next.count; i++) {
        struct application value = next.apps[i];
        unsigned j = i;
        while (j > 3 && strcmp(value.name, next.apps[j - 1].name) < 0) {
            next.apps[j] = next.apps[j - 1];
            j--;
        }
        next.apps[j] = value;
    }
    for (unsigned i = 3; i < next.count; i++) {
        next.apps[i].item.id = i;
    }
    char status[64];
    snprintf(status, sizeof(status), "%u apps%s%s", next.count, limited ? " (limite)" : "",
             errors ? " (erro PATH)" : "");
    if (publish(g, &next, status)) {
        free(next.apps);
        return -1;
    }
    free(previous.apps);
    previous = current;
    current = next;
    printf("Start menu: %u system, %u PATH applications, generation %u%s\n", 3, current.count - 3,
           current.generation, limited ? " (catalog full)" : "");
    return 0;
}
static void launch_item(struct gui *g, const struct kwm_event *event) {
    const struct catalog *catalog = event->reserved == current.generation    ? &current
                                    : event->reserved == previous.generation ? &previous
                                                                             : NULL;
    const struct application *app = NULL;
    if (catalog) {
        for (unsigned i = 0; i < catalog->count; i++) {
            if (catalog->apps[i].item.id == event->code) {
                app = &catalog->apps[i];
                break;
            }
        }
    }
    if (!app) {
        fprintf(stderr, "desktop: menu entry expired; open the menu again\n");
        if (publish(g, &current, "Atualize o menu")) {
            perror("menu status");
        }
        return;
    }
    int pid = app->item.flags & KWM_APP_SYSTEM ? launch(app->path, app->item.id)
                                               : gui_launch("/bin/terminal", app->path);
    if (pid < 0) {
        perror(app->path);
        if (publish(g, &current, "Falha ao abrir aplicativo")) {
            perror("menu status");
        }
    } else {
        printf("Start menu: launched %s PID %d\n", app->path, pid);
    }
}
int main(void) {
    struct gui g = {.fd = open("/dev/wm", O_RDWR | O_CLOEXEC)};
    if (g.fd < 0) {
        perror("desktop");
        return 1;
    }
    struct kwm_request r = {.operation = KWM_SESSION};
    if (gui_request(&g, &r) || refresh(&g)) {
        perror("desktop session/catalog");
        close(g.fd);
        return 1;
    }
    for (unsigned i = 0; i < 3; i++) {
        if (launch(current.apps[i].path, i) < 0) {
            perror("desktop launch");
        }
    }
    for (;;) {
        struct kwm_event e;
        r = (struct kwm_request){.operation = KWM_EVENT, .data = (uintptr_t)&e};
        if (!gui_request(&g, &r)) {
            if (e.type == KWM_LAUNCH) {
                launch_item(&g, &e);
            } else if (e.type == KWM_MENU_REFRESH || e.type == KWM_OVERFLOW) {
                if (refresh(&g)) {
                    perror("desktop catalog refresh");
                    if (publish(&g, &current, "Falha ao atualizar PATH")) {
                        perror("menu status");
                    }
                }
            }
        } else if (errno != EAGAIN) {
            perror("desktop events");
            free(current.apps);
            free(previous.apps);
            close(g.fd);
            return 1;
        }
        while (waitpid(-1, NULL, WNOHANG) > 0) {
        }
        gui_pause();
    }
}
