#ifndef NET_H
#define NET_H
#include "kaly.h"
#define NET_MTU 1500
#define NET_INTERFACES 4
struct netif {
    char name[16];
    uint8_t mac[6];
    uint32_t address, mask, gateway;
    uint64_t rx_packets, tx_packets, rx_errors, tx_errors;
    bool up, link;
    void *private;
    int (*send)(struct netif *, const void *, size_t);
    int (*receive)(struct netif *, void *, size_t);
    bool (*link_status)(struct netif *);
};
extern struct netif net_interfaces[NET_INTERFACES];
extern unsigned net_interface_count;
uint16_t net_be16(uint16_t);
uint32_t net_be32(uint32_t);
int net_register(const struct netif *);
void nic_probe(uint8_t, uint8_t, uint8_t);
void net_poll(void);
size_t net_status(char *, size_t);
struct net_socket;
long net_socket_create(int, int, int);
long net_socket_connect(int, uint64_t, size_t, bool);
long net_socket_bind(int, uint64_t, size_t);
long net_socket_send(int, uint64_t, size_t, unsigned, uint64_t, size_t);
long net_socket_receive(int, uint64_t, size_t, unsigned, uint64_t, uint64_t);
long net_socket_option(int, int, int, uint64_t, uint64_t);
long net_socket_name(int, uint64_t, uint64_t, bool);
long net_socket_shutdown(int, int);
long net_ioctl(int, unsigned long, uint64_t);
long net_socket_read(struct net_socket *, void *, size_t);
long net_socket_write(struct net_socket *, const void *, size_t);
void net_socket_close(struct net_socket *);
#endif
