#ifndef NETINET_IN_H
#define NETINET_IN_H
#include <sys/socket.h>
struct in_addr {
    uint32_t s_addr;
};
struct sockaddr_in {
    uint16_t sin_family, sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define INADDR_ANY 0
uint16_t htons(uint16_t);
uint16_t ntohs(uint16_t);
uint32_t htonl(uint32_t);
uint32_t ntohl(uint32_t);
#endif
