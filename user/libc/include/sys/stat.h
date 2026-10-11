#ifndef STAT_H
#define STAT_H
#include <stdint.h>
struct stat {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, pad;
    uint64_t st_rdev;
    int64_t st_size, st_blksize, st_blocks;
    int64_t st_atime, st_atime_ns, st_mtime, st_mtime_ns, st_ctime, st_ctime_ns;
    int64_t reserved[3];
};
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
int stat(const char *, struct stat *);
int lstat(const char *, struct stat *);
int fstat(int, struct stat *);
int mkdir(const char *, unsigned);
int chmod(const char *, unsigned);
#endif
