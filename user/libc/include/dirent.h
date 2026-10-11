#ifndef DIRENT_H
#define DIRENT_H
#include <stdint.h>
struct dirent {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} __attribute__((packed));
typedef struct {
    int fd;
    long position, length;
    char buffer[4096];
} DIR;
DIR *opendir(const char *);
struct dirent *readdir(DIR *);
int closedir(DIR *);
#endif
