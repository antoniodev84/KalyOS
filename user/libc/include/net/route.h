#ifndef NET_ROUTE_H
#define NET_ROUTE_H
#include <sys/socket.h>
struct rtentry {
    unsigned long rt_pad1;
    struct sockaddr rt_dst, rt_gateway, rt_genmask;
    unsigned short rt_flags;
    short rt_pad2;
    unsigned long rt_pad3;
    void *rt_pad4;
    short rt_metric;
    char *rt_dev;
    unsigned long rt_mtu, rt_window;
    unsigned short rt_irtt;
};
#define RTF_UP 1
#define RTF_GATEWAY 2
#endif
