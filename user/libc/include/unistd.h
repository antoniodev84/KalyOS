#ifndef UNISTD_H
#define UNISTD_H
#include <stddef.h>
#include <stdint.h>
typedef long ssize_t;
typedef int pid_t;
typedef long off_t;
extern char **environ;
long syscall(long, long, long, long, long, long, long);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
int close(int);
off_t lseek(int, off_t, int);
int pipe(int[2]);
int dup2(int, int);
pid_t fork(void);
pid_t getpid(void);
unsigned getuid(void);
int setuid(unsigned);
int execve(const char *, char *const[], char *const[]);
int execvp(const char *, char *const[]);
_Noreturn void _exit(int);
int chdir(const char *);
char *getcwd(char *, size_t);
int unlink(const char *);
int rmdir(const char *);
int link(const char *, const char *);
int symlink(const char *, const char *);
ssize_t readlink(const char *, char *, size_t);
unsigned sleep(unsigned);
int sched_yield(void);
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int access(const char *, int);
int isatty(int);
int ftruncate(int, off_t);
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
