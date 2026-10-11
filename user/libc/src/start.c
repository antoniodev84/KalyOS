#include <unistd.h>
#include <stdlib.h>
char **environ;
extern int main(int, char **);
_Noreturn void libc_start(long argc, char **argv, char **env) {
    environ = env;
    exit(main(argc, argv));
}
