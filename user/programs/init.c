#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <gui.h>
#include <sys/ioctl.h>
#include <kaly/system.h>
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
    puts("KalyOS ready - type init to start the graphical interface");
    int control = open("/dev/system", O_RDWR | O_CLOEXEC);
    if (control < 0) {
        perror("system control");
        return 1;
    }
    int shell = 0, desktop = 0, launch = 0;
    for (;;) {
        if (!shell) {
            shell = gui_launch_system("/bin/sh", NULL, 2);
            if (shell < 0) {
                perror("system shell");
                shell = 0;
            }
        }
        struct ksystem_request r = {.version = KSYSTEM_VERSION, .operation = KSYSTEM_POLL_GUI};
        if (ioctl(control, KSYSTEM_IOCTL, &r)) {
            perror("system events");
        } else if (r.flags && !desktop) {
            launch = 1;
        }
        if (launch && !desktop) {
            desktop = gui_launch_system("/bin/desktop", NULL, 3);
            if (desktop < 0) {
                perror("system desktop");
                desktop = 0;
            } else {
                launch = 0;
            }
        }
        int status, pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (pid == shell) {
                shell = 0;
            } else if (pid == desktop) {
                desktop = 0;
                if (!status) {
                    launch = 1;
                } else {
                    puts("Desktop stopped; type init to retry");
                }
            }
        }
        gui_pause();
    }
}
