#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>
int window_command(const char *, int, char **);
int hardware_command(const char *, int, char **);
static int failure(const char *s) {
    perror(s);
    return 1;
}
static int cat_file(const char *path) {
    int fd = path ? open(path, O_RDONLY) : 0;
    if (fd < 0) {
        return failure(path);
    }
    char b[4096];
    ssize_t n;
    int result = 0;
    while ((n = read(fd, b, sizeof(b))) > 0) {
        size_t done = 0;
        while (done < (size_t)n) {
            ssize_t r = write(1, b + done, n - done);
            if (r <= 0) {
                result = failure("write");
                break;
            }
            done += r;
        }
        if (result) {
            break;
        }
    }
    if (n < 0) {
        result = failure(path ? path : "read");
    }
    if (fd) {
        close(fd);
    }
    return result;
}
static int list(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        return failure(path);
    }
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }
        char name[512];
        snprintf(name, sizeof(name), "%s/%s", path, entry->d_name);
        struct stat st;
        if (lstat(name, &st)) {
            perror(name);
            continue;
        }
        printf("%o %u %lu %ld %s", st.st_mode & 07777, st.st_uid, st.st_nlink, st.st_size,
               entry->d_name);
        if (S_ISLNK(st.st_mode)) {
            char target[512];
            ssize_t n = readlink(name, target, sizeof(target) - 1);
            if (n >= 0) {
                target[n] = 0;
                printf(" -> %s", target);
            }
        }
        putchar('\n');
    }
    return closedir(d) < 0;
}
static int copy_file(const char *from, const char *to) {
    int a = open(from, O_RDONLY);
    if (a < 0) {
        return failure(from);
    }
    struct stat st;
    if (fstat(a, &st)) {
        close(a);
        return failure(from);
    }
    int b = open(to, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (b < 0) {
        close(a);
        return failure(to);
    }
    char buf[4096];
    ssize_t n;
    int result = 0;
    while ((n = read(a, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t r = write(b, buf + off, n - off);
            if (r <= 0) {
                result = failure(to);
                break;
            }
            off += r;
        }
        if (result) {
            break;
        }
    }
    if (n < 0) {
        result = failure(from);
    }
    close(a);
    close(b);
    return result;
}
static int busy(unsigned seconds) {
    int fd = open("/proc/uptime", O_RDONLY);
    if (fd < 0) {
        return failure("uptime");
    }
    char b[32];
    ssize_t r = read(fd, b, sizeof(b) - 1);
    if (r <= 0) {
        close(fd);
        return 1;
    }
    b[r] = 0;
    unsigned end = strtoul(b, NULL, 10) + seconds;
    volatile uint64_t counter = 0;
    for (;;) {
        for (unsigned i = 0; i < 10000000; i++) {
            counter++;
        }
        lseek(fd, 0, SEEK_SET);
        r = read(fd, b, sizeof(b) - 1);
        if (r <= 0) {
            break;
        }
        b[r] = 0;
        if (strtoul(b, NULL, 10) >= end) {
            break;
        }
    }
    close(fd);
    printf("pid %d iterations %lu\n", getpid(), counter);
    return 0;
}
int main(int argc, char **argv) {
    char *cmd = strrchr(argv[0], '/');
    cmd = cmd ? cmd + 1 : argv[0];
    if (!strcmp(cmd, "kalybox")) {
        if (argc == 1) {
            puts("echo cat ls pwd mkdir rm rmdir ln readlink chmod mount umount stat cp mv touch "
                 "sleep ps uname id busy sparse hexdump as fault mem ifconfig ping wget netstat "
                 "nslookup pendrive sound shutdown window init");
            return 0;
        }
        argc--;
        argv++;
        cmd = argv[0];
    }
    int window_result = window_command(cmd, argc, argv);
    if (window_result >= 0) {
        return window_result;
    }
    int hardware_result = hardware_command(cmd, argc, argv);
    if (hardware_result >= 0) {
        return hardware_result;
    }
    if (!strcmp(cmd, "echo")) {
        for (int i = 1; i < argc; i++) {
            printf("%s%s", i == 1 ? "" : " ", argv[i]);
        }
        putchar('\n');
        return 0;
    }
    if (!strcmp(cmd, "cat")) {
        if (argc == 1) {
            return cat_file(NULL);
        }
        int r = 0;
        for (int i = 1; i < argc; i++) {
            r |= cat_file(argv[i]);
        }
        return r;
    }
    if (!strcmp(cmd, "ls")) {
        return list(argc > 1 ? argv[1] : ".");
    }
    if (!strcmp(cmd, "pwd")) {
        char b[512];
        if (!getcwd(b, sizeof(b))) {
            return failure("pwd");
        }
        puts(b);
        return 0;
    }
    if (!strcmp(cmd, "mkdir")) {
        int r = 0;
        for (int i = 1; i < argc; i++) {
            if (mkdir(argv[i], 0755)) {
                r = failure(argv[i]);
            }
        }
        return argc == 1 ? 2 : r;
    }
    if (!strcmp(cmd, "rm") || !strcmp(cmd, "rmdir")) {
        int r = 0;
        for (int i = 1; i < argc; i++) {
            if ((!strcmp(cmd, "rm") ? unlink(argv[i]) : rmdir(argv[i]))) {
                r = failure(argv[i]);
            }
        }
        return argc == 1 ? 2 : r;
    }
    if (!strcmp(cmd, "ln")) {
        int symbolic = argc > 1 && !strcmp(argv[1], "-s");
        if (argc != 3 + symbolic) {
            return 2;
        }
        return (symbolic ? symlink(argv[2], argv[3]) : link(argv[1], argv[2])) ? failure("ln") : 0;
    }
    if (!strcmp(cmd, "readlink")) {
        if (argc != 2) {
            return 2;
        }
        char b[512];
        ssize_t n = readlink(argv[1], b, sizeof(b) - 1);
        if (n < 0) {
            return failure(argv[1]);
        }
        b[n] = 0;
        puts(b);
        return 0;
    }
    if (!strcmp(cmd, "chmod")) {
        if (argc != 3) {
            return 2;
        }
        return chmod(argv[2], strtoul(argv[1], NULL, 8)) ? failure("chmod") : 0;
    }
    if (!strcmp(cmd, "mount")) {
        if (argc != 4) {
            return 2;
        }
        return mount(argv[1], argv[2], argv[3], 0, NULL) ? failure("mount") : 0;
    }
    if (!strcmp(cmd, "umount")) {
        if (argc != 2) {
            return 2;
        }
        return umount(argv[1]) ? failure("umount") : 0;
    }
    if (!strcmp(cmd, "stat")) {
        if (argc != 2) {
            return 2;
        }
        struct stat st;
        if (stat(argv[1], &st)) {
            return failure(argv[1]);
        }
        printf("inode %lu size %ld mode %o uid %u links %lu\n", st.st_ino, st.st_size, st.st_mode,
               st.st_uid, st.st_nlink);
        return 0;
    }
    if (!strcmp(cmd, "cp") || !strcmp(cmd, "mv")) {
        if (argc != 3) {
            return 2;
        }
        int r = copy_file(argv[1], argv[2]);
        if (!r && !strcmp(cmd, "mv") && unlink(argv[1])) {
            r = failure(argv[1]);
        }
        return r;
    }
    if (!strcmp(cmd, "touch")) {
        int r = 0;
        for (int i = 1; i < argc; i++) {
            int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
            if (fd < 0) {
                r = failure(argv[i]);
            } else {
                close(fd);
            }
        }
        return argc == 1 ? 2 : r;
    }
    if (!strcmp(cmd, "sleep")) {
        return argc == 2 ? sleep(atoi(argv[1])) != 0 : 2;
    }
    if (!strcmp(cmd, "ps")) {
        return cat_file("/proc/processes");
    }
    if (!strcmp(cmd, "uname")) {
        struct utsname u;
        if (uname(&u)) {
            return failure("uname");
        }
        printf("%s %s\n", u.sysname, u.machine);
        return 0;
    }
    if (!strcmp(cmd, "id")) {
        printf("uid=%u pid=%d\n", getuid(), getpid());
        return 0;
    }
    if (!strcmp(cmd, "busy")) {
        return busy(argc > 1 ? atoi(argv[1]) : 5);
    }
    if (!strcmp(cmd, "sparse")) {
        if (argc != 3) {
            return 2;
        }
        uint64_t size = strtoul(argv[2], NULL, 0);
        if (!size || size > 8UL * 1024 * 1024 * 1024) {
            return 2;
        }
        int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            return failure(argv[1]);
        }
        int r = lseek(fd, size - 1, SEEK_SET) < 0 || write(fd, "X", 1) != 1;
        if (r) {
            perror("sparse");
        }
        close(fd);
        return r;
    }
    if (!strcmp(cmd, "hexdump")) {
        if (argc < 2) {
            return 2;
        }
        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            return failure(argv[1]);
        }
        if (argc > 2 && lseek(fd, strtoul(argv[2], NULL, 0), SEEK_SET) < 0) {
            close(fd);
            return failure("seek");
        }
        unsigned char b[16];
        ssize_t n = read(fd, b, sizeof(b));
        if (n < 0) {
            close(fd);
            return failure("read");
        }
        for (ssize_t i = 0; i < n; i++) {
            printf("%02x ", b[i]);
        }
        putchar('\n');
        close(fd);
        return 0;
    }
    if (!strcmp(cmd, "as")) {
        if (argc < 3) {
            return 2;
        }
        if (setuid(atoi(argv[1]))) {
            return failure("setuid");
        }
        execvp(argv[2], argv + 2);
        return failure(argv[2]);
    }
    if (!strcmp(cmd, "fault")) {
        if (argc > 1 && !strcmp(argv[1], "kernel")) {
            *(volatile uint64_t *)0x100000 = 1;
        } else if (argc > 1 && !strcmp(argv[1], "nx")) {
            void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) {
                return failure("mmap");
            }
            *(uint8_t *)p = 0xc3;
            ((void (*)(void))p)();
        } else if (argc > 1 && !strcmp(argv[1], "readonly")) {
            void *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) {
                return failure("mmap");
            }
            *(volatile uint64_t *)p = 1;
        } else {
            *(volatile uint64_t *)0x2000000000 = 1;
        }
        return 1;
    }
    if (!strcmp(cmd, "mem")) {
        volatile unsigned *p = malloc(sizeof(*p));
        if (!p) {
            return failure("malloc");
        }
        *p = 42;
        pid_t child = fork();
        if (child < 0) {
            free((void *)p);
            return failure("fork");
        }
        if (!child) {
            *p = 7;
            printf("child value=%u\n", *p);
            _exit(0);
        }
        int s;
        waitpid(child, &s, 0);
        printf("parent value=%u\n", *p);
        free((void *)p);
        return 0;
    }
    fprintf(stderr, "%s: unknown command\n", cmd);
    return 127;
}
