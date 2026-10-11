#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return NULL;
    }
    DIR *d = calloc(1, sizeof(*d));
    if (!d) {
        close(fd);
        return NULL;
    }
    d->fd = fd;
    return d;
}
struct dirent *readdir(DIR *d) {
    if (d->position == d->length) {
        d->length = syscall(217, d->fd, (long)d->buffer, sizeof(d->buffer), 0, 0, 0);
        d->position = 0;
        if (d->length <= 0) {
            return NULL;
        }
    }
    struct dirent *entry = (void *)(d->buffer + d->position);
    d->position += entry->d_reclen;
    return entry;
}
int closedir(DIR *d) {
    int r = close(d->fd);
    free(d);
    return r;
}
