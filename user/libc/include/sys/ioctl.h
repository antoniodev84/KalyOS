#ifndef SYS_IOCTL_H
#define SYS_IOCTL_H
int ioctl(int, unsigned long, void *);
#define SIOCADDRT 0x890b
#define SIOCDELRT 0x890c
#define SIOCGIFFLAGS 0x8913
#define SIOCSIFFLAGS 0x8914
#define SIOCGIFADDR 0x8915
#define SIOCSIFADDR 0x8916
#define SIOCGIFNETMASK 0x891b
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFMTU 0x8921
#define SIOCGIFHWADDR 0x8927
#endif
