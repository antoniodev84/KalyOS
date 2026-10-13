#ifndef NET_IF_H
#define NET_IF_H
#include <sys/socket.h>
struct ifreq {
    char ifr_name[16];
    union {
        struct sockaddr address;
        short flags;
        int mtu;
        uint8_t padding[24];
    } value;
};
#define ifr_addr value.address
#define ifr_netmask value.address
#define ifr_flags value.flags
#define IFF_UP 1
#endif
