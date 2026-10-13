#ifndef SYS_SOCKET_H
#define SYS_SOCKET_H
#include <unistd.h>
typedef unsigned socklen_t;
struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000
#define SOL_SOCKET 1
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define MSG_PEEK 2
#define MSG_TRUNC 32
#define MSG_DONTWAIT 64
#define MSG_NOSIGNAL 0x4000
int socket(int, int, int);
int connect(int, const struct sockaddr *, socklen_t);
int bind(int, const struct sockaddr *, socklen_t);
ssize_t send(int, const void *, size_t, int);
ssize_t recv(int, void *, size_t, int);
ssize_t sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
int shutdown(int, int);
int getsockname(int, struct sockaddr *, socklen_t *);
int getsockopt(int, int, int, void *, socklen_t *);
int getpeername(int, struct sockaddr *, socklen_t *);
#endif
