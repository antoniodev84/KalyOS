#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/mount.h>
int main(void) {
    int fd = open("/dev/console", O_RDWR);
    if (fd < 0) {
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        if (dup2(fd, i) < 0) {
            return 1;
        }
    }
    if (fd > 2) {
        close(fd);
    }
    if (mount("/dev/sda", "/mnt", "ext2", 0, NULL)) {
        perror("mount");
    }
    puts("KalyOS ready");
    for (;;) {
        pid_t child = fork();
        if (child < 0) {
            perror("fork");
            sleep(1);
            continue;
        }
        if (!child) {
            char *argv[] = {"/bin/sh", NULL};
            execve(argv[0], argv, environ);
            perror("sh");
            return 1;
        }
        int status;
        while (wait(&status) != child)
            ;
    }
}
