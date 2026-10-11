#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
int errno;
long syscall(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    if ((unsigned long)result >= (unsigned long)-4095) {
        errno = -result;
        return -1;
    }
    return result;
}
ssize_t read(int f, void *b, size_t n) {
    return syscall(0, f, (long)b, n, 0, 0, 0);
}
ssize_t write(int f, const void *b, size_t n) {
    return syscall(1, f, (long)b, n, 0, 0, 0);
}
int open(const char *p, int f, ...) {
    unsigned mode = 0;
    if (f & O_CREAT) {
        va_list ap;
        va_start(ap, f);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    return syscall(2, (long)p, f, mode, 0, 0, 0);
}
int close(int f) {
    return syscall(3, f, 0, 0, 0, 0, 0);
}
int stat(const char *p, struct stat *s) {
    return syscall(4, (long)p, (long)s, 0, 0, 0, 0);
}
int fstat(int f, struct stat *s) {
    return syscall(5, f, (long)s, 0, 0, 0, 0);
}
int lstat(const char *p, struct stat *s) {
    return syscall(6, (long)p, (long)s, 0, 0, 0, 0);
}
off_t lseek(int f, off_t o, int w) {
    return syscall(8, f, o, w, 0, 0, 0);
}
void *mmap(void *p, size_t n, int prot, int flags, int fd, long off) {
    return (void *)syscall(9, (long)p, n, prot, flags, fd, off);
}
int mprotect(void *p, size_t n, int prot) {
    return syscall(10, (long)p, n, prot, 0, 0, 0);
}
int munmap(void *p, size_t n) {
    return syscall(11, (long)p, n, 0, 0, 0, 0);
}
int brk(void *p) {
    long r = syscall(12, (long)p, 0, 0, 0, 0, 0);
    if (r == (long)p) {
        return 0;
    }
    errno = ENOMEM;
    return -1;
}
void *sbrk(long n) {
    long old = syscall(12, 0, 0, 0, 0, 0, 0);
    if (n && brk((void *)(old + n))) {
        return MAP_FAILED;
    }
    return (void *)old;
}
int pipe(int f[2]) {
    return syscall(22, (long)f, 0, 0, 0, 0, 0);
}
int sched_yield(void) {
    return syscall(24, 0, 0, 0, 0, 0, 0);
}
int dup2(int f, int t) {
    return syscall(33, f, t, 0, 0, 0, 0);
}
int nanosleep(const struct timespec *t, struct timespec *r) {
    return syscall(35, (long)t, (long)r, 0, 0, 0, 0);
}
pid_t getpid(void) {
    return syscall(39, 0, 0, 0, 0, 0, 0);
}
pid_t fork(void) {
    return syscall(57, 0, 0, 0, 0, 0, 0);
}
int execve(const char *p, char *const a[], char *const e[]) {
    return syscall(59, (long)p, (long)a, (long)e, 0, 0, 0);
}
_Noreturn void _exit(int s) {
    syscall(60, s, 0, 0, 0, 0, 0);
    for (;;) {
        __asm__ volatile("ud2");
    }
}
pid_t waitpid(pid_t p, int *s, int o) {
    return syscall(61, p, (long)s, o, 0, 0, 0);
}
pid_t wait(int *s) {
    return waitpid(-1, s, 0);
}
int uname(struct utsname *u) {
    return syscall(63, (long)u, 0, 0, 0, 0, 0);
}
char *getcwd(char *b, size_t n) {
    return syscall(79, (long)b, n, 0, 0, 0, 0) < 0 ? NULL : b;
}
int chdir(const char *p) {
    return syscall(80, (long)p, 0, 0, 0, 0, 0);
}
int mkdir(const char *p, unsigned m) {
    return syscall(83, (long)p, m, 0, 0, 0, 0);
}
int rmdir(const char *p) {
    return syscall(84, (long)p, 0, 0, 0, 0, 0);
}
int link(const char *p, const char *q) {
    return syscall(86, (long)p, (long)q, 0, 0, 0, 0);
}
int unlink(const char *p) {
    return syscall(87, (long)p, 0, 0, 0, 0, 0);
}
int symlink(const char *p, const char *q) {
    return syscall(88, (long)p, (long)q, 0, 0, 0, 0);
}
ssize_t readlink(const char *p, char *b, size_t n) {
    return syscall(89, (long)p, (long)b, n, 0, 0, 0);
}
int chmod(const char *p, unsigned m) {
    return syscall(90, (long)p, m, 0, 0, 0, 0);
}
unsigned getuid(void) {
    return syscall(102, 0, 0, 0, 0, 0, 0);
}
int setuid(unsigned u) {
    return syscall(105, u, 0, 0, 0, 0, 0);
}
int mount(const char *s, const char *t, const char *f, unsigned long flags, const void *d) {
    return syscall(165, (long)s, (long)t, (long)f, flags, (long)d, 0);
}
int umount(const char *p) {
    return syscall(166, (long)p, 0, 0, 0, 0, 0);
}
unsigned sleep(unsigned n) {
    struct timespec t = {n, 0};
    return nanosleep(&t, NULL) ? n : 0;
}
int execvp(const char *file, char *const argv[]) {
    if (strchr(file, '/')) {
        return execve(file, argv, environ);
    }
    char path[512];
    if (strlen(file) + 6 > sizeof(path)) {
        errno = EINVAL;
        return -1;
    }
    strcpy(path, "/bin/");
    strcat(path, file);
    return execve(path, argv, environ);
}
