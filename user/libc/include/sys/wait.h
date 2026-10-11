#ifndef WAIT_H
#define WAIT_H
#include <unistd.h>
#define WNOHANG 1
#define WEXITSTATUS(s) (((s) >> 8) & 255)
pid_t waitpid(pid_t, int *, int);
pid_t wait(int *);
#endif
