#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <errno.h>
int socket(int domain, int type, int protocol) {
    return syscall(41, domain, type, protocol, 0, 0, 0);
}
int connect(int fd, const struct sockaddr *address, socklen_t length) {
    return syscall(42, fd, (long)address, length, 0, 0, 0);
}
int bind(int fd, const struct sockaddr *address, socklen_t length) {
    return syscall(49, fd, (long)address, length, 0, 0, 0);
}
ssize_t sendto(int fd, const void *data, size_t length, int flags, const struct sockaddr *address,
               socklen_t address_length) {
    return syscall(44, fd, (long)data, length, flags, (long)address, address_length);
}
ssize_t recvfrom(int fd, void *data, size_t length, int flags, struct sockaddr *address,
                 socklen_t *address_length) {
    return syscall(45, fd, (long)data, length, flags, (long)address, (long)address_length);
}
ssize_t send(int fd, const void *data, size_t length, int flags) {
    return sendto(fd, data, length, flags, NULL, 0);
}
ssize_t recv(int fd, void *data, size_t length, int flags) {
    return recvfrom(fd, data, length, flags, NULL, NULL);
}
int shutdown(int fd, int how) {
    return syscall(48, fd, how, 0, 0, 0, 0);
}
int getsockname(int fd, struct sockaddr *address, socklen_t *length) {
    return syscall(51, fd, (long)address, (long)length, 0, 0, 0);
}
int getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
    return syscall(55, fd, level, option, (long)value, (long)length, 0);
}
int getpeername(int fd, struct sockaddr *address, socklen_t *length) {
    return syscall(52, fd, (long)address, (long)length, 0, 0, 0);
}
int ioctl(int fd, unsigned long request, void *argument) {
    return syscall(16, fd, request, (long)argument, 0, 0, 0);
}
uint16_t htons(uint16_t value) {
    return __builtin_bswap16(value);
}
uint16_t ntohs(uint16_t value) {
    return htons(value);
}
uint32_t htonl(uint32_t value) {
    return __builtin_bswap32(value);
}
uint32_t ntohl(uint32_t value) {
    return htonl(value);
}
int inet_pton(int family, const char *text, void *destination) {
    if (family != AF_INET) {
        errno = 97;
        return -1;
    }
    uint32_t address = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned value = 0, digits = 0;
        while (*text >= '0' && *text <= '9') {
            if (++digits > 3) {
                return 0;
            }
            value = value * 10 + *text++ - '0';
        }
        if (!digits || value > 255 || (i < 3 ? *text++ != '.' : *text != 0)) {
            return 0;
        }
        address = (address << 8) | value;
    }
    *(uint32_t *)destination = htonl(address);
    return 1;
}
const char *inet_ntop(int family, const void *source, char *destination, socklen_t length) {
    if (family != AF_INET) {
        errno = 97;
        return NULL;
    }
    uint32_t address = ntohl(*(const uint32_t *)source);
    char text[16];
    int n = snprintf(text, sizeof(text), "%u.%u.%u.%u", address >> 24, (address >> 16) & 255,
                     (address >> 8) & 255, address & 255);
    if ((unsigned)n >= length) {
        errno = 28;
        return NULL;
    }
    for (int i = 0; i <= n; i++) {
        destination[i] = text[i];
    }
    return destination;
}
