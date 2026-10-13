#include "net.h"
#define SOCKET_COUNT 32
#define RECEIVE_SIZE 16384
#define DATAGRAM_COUNT 8
#define TCP_FIN 1
#define TCP_SYN 2
#define TCP_RST 4
#define TCP_PSH 8
#define TCP_ACK 16
struct ethernet {
    uint8_t destination[6], source[6];
    uint16_t type;
} PACKED;
struct ipv4 {
    uint8_t version, tos;
    uint16_t length, id, fragment;
    uint8_t ttl, protocol;
    uint16_t checksum;
    uint32_t source, destination;
} PACKED;
struct arp_packet {
    uint16_t hardware, protocol;
    uint8_t hardware_length, protocol_length;
    uint16_t operation;
    uint8_t sender_mac[6];
    uint32_t sender_ip;
    uint8_t target_mac[6];
    uint32_t target_ip;
} PACKED;
struct udp_header {
    uint16_t source, destination, length, checksum;
} PACKED;
struct tcp_header {
    uint16_t source, destination;
    uint32_t sequence, acknowledgement;
    uint8_t offset, flags;
    uint16_t window, checksum, urgent;
} PACKED;
struct sockaddr_in {
    uint16_t family, port;
    uint32_t address;
    uint8_t padding[8];
};
struct datagram {
    uint32_t address;
    uint16_t port, length;
    uint8_t data[NET_MTU];
};
enum tcp_state {
    TCP_IDLE,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_CLOSE_WAIT,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_LAST_ACK,
    TCP_CLOSING,
    TCP_TIME_WAIT,
    TCP_CLOSED
};
struct net_socket {
    int type, protocol, error;
    bool detached, read_shutdown, write_shutdown, connecting_arp;
    uint32_t local_address, remote_address;
    uint16_t local_port, remote_port;
    enum tcp_state state;
    uint32_t send_next, send_unacked, receive_next;
    uint16_t remote_window, mss;
    uint8_t pending[1460], pending_flags;
    uint16_t pending_length;
    unsigned retries;
    uint64_t deadline;
    uint8_t receive[RECEIVE_SIZE];
    unsigned receive_head, receive_tail, receive_length;
    struct datagram datagrams[DATAGRAM_COUNT];
    unsigned datagram_head, datagram_tail, datagram_length;
};
struct arp_entry {
    struct netif *interface;
    uint32_t address;
    uint8_t mac[6];
    uint64_t expires, request_time;
    bool resolved;
    unsigned requests;
};
struct netif net_interfaces[NET_INTERFACES];
unsigned net_interface_count;
static struct net_socket *sockets[SOCKET_COUNT];
static struct arp_entry arp_cache[32];
static struct lock network_lock;
static uint16_t next_port = 49152, packet_id;
static uint64_t last_link_poll;
uint16_t net_be16(uint16_t value) {
    return __builtin_bswap16(value);
}
uint32_t net_be32(uint32_t value) {
    return __builtin_bswap32(value);
}
static uint32_t sum_bytes(const void *data, size_t length, uint32_t sum) {
    const uint8_t *p = data;
    while (length >= 2) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        length -= 2;
    }
    if (length) {
        sum += (uint16_t)p[0] << 8;
    }
    return sum;
}
static uint16_t checksum(const void *data, size_t length, uint32_t sum) {
    sum = sum_bytes(data, length, sum);
    while (sum >> 16) {
        sum = (sum & 65535) + (sum >> 16);
    }
    return net_be16(~sum);
}
static uint32_t pseudo_sum(uint32_t source, uint32_t destination, unsigned protocol, size_t n) {
    uint32_t addresses[2] = {net_be32(source), net_be32(destination)};
    return sum_bytes(addresses, sizeof(addresses), protocol + n);
}
int net_register(const struct netif *interface) {
    if (net_interface_count == NET_INTERFACES) {
        return -ENOSPC;
    }
    unsigned index = net_interface_count++;
    net_interfaces[index] = *interface;
    strcpy(net_interfaces[index].name, "eth0");
    net_interfaces[index].name[3] += index;
    return index;
}
static struct netif *route(uint32_t destination, uint32_t source) {
    struct netif *gateway = NULL;
    for (unsigned i = 0; i < net_interface_count; i++) {
        struct netif *n = &net_interfaces[i];
        if (!n->up || !n->address || (source && source != n->address)) {
            continue;
        }
        if ((destination & n->mask) == (n->address & n->mask)) {
            return n;
        }
        if (n->gateway && !gateway) {
            gateway = n;
        }
    }
    return gateway;
}
static int ethernet_send(struct netif *n, const uint8_t *mac, unsigned type, const void *data,
                         size_t length) {
    uint8_t frame[1514];
    if (length > NET_MTU) {
        return -90;
    }
    if (!n->link) {
        return -100;
    }
    struct ethernet *h = (void *)frame;
    memcpy(h->destination, mac, 6);
    memcpy(h->source, n->mac, 6);
    h->type = net_be16(type);
    memcpy(frame + sizeof(*h), data, length);
    int e = n->send(n, frame, length + sizeof(*h));
    if (!e) {
        n->tx_packets++;
    } else if (e != -EAGAIN) {
        n->tx_errors++;
    }
    return e;
}
static int arp_send(struct netif *n, unsigned operation, uint32_t target, const uint8_t *mac) {
    struct arp_packet p = {.hardware = net_be16(1),
                           .protocol = net_be16(0x800),
                           .hardware_length = 6,
                           .protocol_length = 4,
                           .operation = net_be16(operation),
                           .sender_ip = net_be32(n->address),
                           .target_ip = net_be32(target)};
    memcpy(p.sender_mac, n->mac, 6);
    if (operation == 2) {
        memcpy(p.target_mac, mac, 6);
    }
    return ethernet_send(n, mac, 0x806, &p, sizeof(p));
}
static struct arp_entry *arp_entry(struct netif *n, uint32_t address) {
    struct arp_entry *oldest = &arp_cache[0];
    for (unsigned i = 0; i < 32; i++) {
        struct arp_entry *e = &arp_cache[i];
        if (e->interface == n && e->address == address) {
            return e;
        }
        if (!e->interface) {
            oldest = e;
        } else if (oldest->interface && e->expires < oldest->expires) {
            oldest = e;
        }
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->interface = n;
    oldest->address = address;
    return oldest;
}
static int arp_resolve(struct netif *n, uint32_t address, uint8_t mac[6]) {
    if (address == 0xffffffff || address == (n->address | ~n->mask)) {
        memset(mac, 255, 6);
        return 0;
    }
    if ((address & n->mask) != (n->address & n->mask)) {
        address = n->gateway;
    }
    if (!address) {
        return -101;
    }
    struct arp_entry *e = arp_entry(n, address);
    uint64_t now = hw_time_us();
    if (e->resolved && now < e->expires) {
        memcpy(mac, e->mac, 6);
        return 0;
    }
    if (e->requests >= 5 && now - e->request_time >= 1000000) {
        return -113;
    }
    if (!e->request_time || now - e->request_time >= 1000000) {
        const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
        int error = arp_send(n, 1, address, broadcast);
        if (error && error != -EAGAIN) {
            return error;
        }
        e->request_time = now;
        e->requests++;
    }
    return -EAGAIN;
}
static int ip_send(uint32_t destination, uint32_t source, unsigned protocol, const void *payload,
                   size_t length) {
    struct netif *n = route(destination, source);
    if (!n) {
        return -101;
    }
    if (length > NET_MTU - sizeof(struct ipv4)) {
        return -90;
    }
    uint8_t mac[6];
    int error = arp_resolve(n, destination, mac);
    if (error) {
        return error;
    }
    uint8_t packet[NET_MTU];
    struct ipv4 *h = (void *)packet;
    *h = (struct ipv4){.version = 0x45,
                       .length = net_be16(sizeof(*h) + length),
                       .id = net_be16(packet_id++),
                       .fragment = net_be16(0x4000),
                       .ttl = 64,
                       .protocol = protocol,
                       .source = net_be32(n->address),
                       .destination = net_be32(destination)};
    h->checksum = checksum(h, sizeof(*h), 0);
    memcpy(packet + sizeof(*h), payload, length);
    return ethernet_send(n, mac, 0x800, packet, sizeof(*h) + length);
}
static void enqueue_datagram(struct net_socket *s, const void *p, size_t length, uint32_t address,
                             uint16_t port) {
    if (s->read_shutdown || s->datagram_length == DATAGRAM_COUNT || length > NET_MTU) {
        return;
    }
    struct datagram *d = &s->datagrams[s->datagram_head];
    d->address = address;
    d->port = port;
    d->length = length;
    memcpy(d->data, p, length);
    s->datagram_head = (s->datagram_head + 1) % DATAGRAM_COUNT;
    s->datagram_length++;
}
static int tcp_emit(struct net_socket *s, unsigned flags, const void *payload, size_t length,
                    uint32_t sequence) {
    uint8_t packet[1480];
    unsigned header_length = flags & TCP_SYN ? 24 : 20;
    struct tcp_header *h = (void *)packet;
    *h = (struct tcp_header){.source = net_be16(s->local_port),
                             .destination = net_be16(s->remote_port),
                             .sequence = net_be32(sequence),
                             .acknowledgement = net_be32(s->receive_next),
                             .offset = (header_length / 4) << 4,
                             .flags = flags,
                             .window = net_be16(RECEIVE_SIZE - s->receive_length)};
    if (flags & TCP_SYN) {
        packet[20] = 2;
        packet[21] = 4;
        packet[22] = 0x05;
        packet[23] = 0xb4;
    }
    memcpy(packet + header_length, payload, length);
    h->checksum =
        checksum(packet, length + header_length,
                 pseudo_sum(s->local_address, s->remote_address, 6, length + header_length));
    return ip_send(s->remote_address, s->local_address, 6, packet, length + header_length);
}
static int tcp_queue(struct net_socket *s, unsigned flags, const void *payload, size_t length) {
    if (s->pending_flags) {
        return -EAGAIN;
    }
    int error = tcp_emit(s, flags, payload, length, s->send_next);
    if (error) {
        return error;
    }
    memcpy(s->pending, payload, length);
    s->pending_length = length;
    s->pending_flags = flags;
    s->send_unacked = s->send_next;
    s->send_next += length + !!(flags & TCP_SYN) + !!(flags & TCP_FIN);
    s->retries = 0;
    s->deadline = hw_time_us() + 500000;
    return 0;
}
static bool sequence_before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}
static void tcp_input(uint32_t source, uint32_t destination, const uint8_t *p, size_t length) {
    if (length < 20 || checksum(p, length, pseudo_sum(source, destination, 6, length))) {
        return;
    }
    const struct tcp_header *h = (const void *)p;
    unsigned header_length = (h->offset >> 4) * 4;
    if (header_length < 20 || header_length > length) {
        return;
    }
    uint32_t seq = net_be32(h->sequence), ack = net_be32(h->acknowledgement);
    struct net_socket *s = NULL;
    for (unsigned i = 0; i < SOCKET_COUNT; i++) {
        struct net_socket *candidate = sockets[i];
        if (candidate && candidate->type == 1 && candidate->local_address == destination &&
            candidate->remote_address == source &&
            candidate->local_port == net_be16(h->destination) &&
            candidate->remote_port == net_be16(h->source)) {
            s = candidate;
            break;
        }
    }
    if (!s) {
        return;
    }
    if (h->flags & TCP_RST) {
        if ((s->state == TCP_SYN_SENT && (h->flags & TCP_ACK) && ack == s->send_next) ||
            (s->state != TCP_SYN_SENT && seq == s->receive_next)) {
            s->error = s->state == TCP_SYN_SENT ? 111 : 104;
            s->state = TCP_CLOSED;
            s->pending_flags = 0;
        }
        return;
    }
    if (s->state == TCP_SYN_SENT) {
        if ((h->flags & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK) || ack != s->send_next) {
            return;
        }
        s->receive_next = seq + 1;
        s->remote_window = net_be16(h->window);
        for (unsigned off = 20; off < header_length;) {
            unsigned kind = p[off++];
            if (!kind) {
                break;
            }
            if (kind == 1) {
                continue;
            }
            if (off >= header_length) {
                break;
            }
            unsigned n = p[off++];
            if (n < 2 || n - 2 > header_length - off) {
                break;
            }
            if (kind == 2 && n == 4) {
                unsigned mss = ((unsigned)p[off] << 8) | p[off + 1];
                if (mss) {
                    s->mss = MIN(mss, 1460);
                }
            }
            off += n - 2;
        }
        s->pending_flags = 0;
        s->send_unacked = ack;
        s->state = TCP_ESTABLISHED;
        tcp_emit(s, TCP_ACK, NULL, 0, s->send_next);
        return;
    }
    if (s->state == TCP_IDLE || s->state == TCP_CLOSED) {
        return;
    }
    if (!(h->flags & TCP_ACK) || sequence_before(s->send_next, ack)) {
        return;
    }
    if (h->flags & TCP_SYN) {
        tcp_emit(s, TCP_ACK, NULL, 0, s->send_next);
        return;
    }
    if (sequence_before(s->receive_next, seq)) {
        tcp_emit(s, TCP_ACK, NULL, 0, s->send_next);
        return;
    }
    if (s->pending_flags && ack == s->send_next) {
        s->send_unacked = ack;
        s->pending_flags = 0;
        if (s->state == TCP_FIN_WAIT1) {
            s->state = TCP_FIN_WAIT2;
            s->deadline = hw_time_us() + 30000000;
        } else if (s->state == TCP_LAST_ACK) {
            s->state = TCP_CLOSED;
        } else if (s->state == TCP_CLOSING) {
            s->state = TCP_TIME_WAIT;
            s->deadline = hw_time_us() + 30000000;
        }
    }
    s->remote_window = net_be16(h->window);
    size_t bytes = length - header_length;
    unsigned skip = s->receive_next - seq;
    if (skip > bytes) {
        if (h->flags & TCP_FIN) {
            tcp_emit(s, TCP_ACK, NULL, 0, s->send_next);
        }
        return;
    }
    bytes -= skip;
    p += header_length + skip;
    if (bytes > RECEIVE_SIZE - s->receive_length) {
        tcp_emit(s, TCP_ACK, NULL, 0, s->send_next);
        return;
    }
    if (!s->read_shutdown) {
        for (size_t i = 0; i < bytes; i++) {
            s->receive[s->receive_head++ % RECEIVE_SIZE] = p[i];
        }
        s->receive_length += bytes;
    }
    s->receive_next += bytes;
    if (h->flags & TCP_FIN) {
        s->receive_next++;
        if (s->state == TCP_ESTABLISHED) {
            s->state = TCP_CLOSE_WAIT;
        } else if (s->state == TCP_FIN_WAIT1) {
            s->state = TCP_CLOSING;
        } else if (s->state == TCP_FIN_WAIT2) {
            s->state = TCP_TIME_WAIT;
            s->deadline = hw_time_us() + 30000000;
        }
    }
    if (bytes || (h->flags & TCP_FIN)) {
        tcp_emit(s, TCP_ACK, NULL, 0, s->send_next);
    }
}
static void ip_input(struct netif *n, const uint8_t *packet, size_t length) {
    if (length < sizeof(struct ipv4)) {
        return;
    }
    const struct ipv4 *h = (const void *)packet;
    unsigned header_length = (h->version & 15) * 4;
    unsigned total = net_be16(h->length);
    uint32_t source = net_be32(h->source), destination = net_be32(h->destination);
    if ((h->version >> 4) != 4 || header_length < 20 || header_length > length ||
        total < header_length || total > length || !h->ttl || checksum(packet, header_length, 0) ||
        (net_be16(h->fragment) & 0xbfff) ||
        (destination != n->address && destination != 0xffffffff &&
         destination != (n->address | ~n->mask))) {
        n->rx_errors++;
        return;
    }
    const uint8_t *payload = packet + header_length;
    size_t bytes = total - header_length;
    if (h->protocol == 1) {
        if (bytes < 8 || checksum(payload, bytes, 0)) {
            return;
        }
        for (unsigned i = 0; i < SOCKET_COUNT; i++) {
            struct net_socket *s = sockets[i];
            if (s && !s->detached && s->type == 3 && s->protocol == 1 &&
                (!s->local_address || s->local_address == destination) &&
                (!s->remote_address || s->remote_address == source)) {
                enqueue_datagram(s, packet, total, source, 0);
            }
        }
        if (payload[0] == 8 && payload[1] == 0 && destination == n->address) {
            uint8_t reply[1480];
            memcpy(reply, payload, bytes);
            reply[0] = 0;
            reply[2] = reply[3] = 0;
            *(uint16_t *)(reply + 2) = checksum(reply, bytes, 0);
            ip_send(source, n->address, 1, reply, bytes);
        }
    } else if (h->protocol == 17) {
        if (bytes < sizeof(struct udp_header)) {
            return;
        }
        const struct udp_header *udp = (const void *)payload;
        unsigned udp_length = net_be16(udp->length);
        if (udp_length < 8 || udp_length > bytes ||
            (udp->checksum &&
             checksum(payload, udp_length, pseudo_sum(source, destination, 17, udp_length)))) {
            return;
        }
        for (unsigned i = 0; i < SOCKET_COUNT; i++) {
            struct net_socket *s = sockets[i];
            if (s && !s->detached && s->type == 2 && s->local_port == net_be16(udp->destination) &&
                (!s->local_address || s->local_address == destination) &&
                (!s->remote_address ||
                 (s->remote_address == source && s->remote_port == net_be16(udp->source)))) {
                enqueue_datagram(s, payload + 8, udp_length - 8, source, net_be16(udp->source));
                break;
            }
        }
    } else if (h->protocol == 6 && destination == n->address) {
        tcp_input(source, destination, payload, bytes);
    }
}
static void frame_input(struct netif *n, const uint8_t *frame, size_t length) {
    if (length < sizeof(struct ethernet)) {
        return;
    }
    const struct ethernet *h = (const void *)frame;
    const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    if (memcmp(h->destination, n->mac, 6) && memcmp(h->destination, broadcast, 6)) {
        return;
    }
    const uint8_t *payload = frame + sizeof(*h);
    length -= sizeof(*h);
    if (net_be16(h->type) == 0x806 && length >= sizeof(struct arp_packet)) {
        const struct arp_packet *arp = (const void *)payload;
        unsigned operation = net_be16(arp->operation);
        uint32_t source = net_be32(arp->sender_ip), target = net_be32(arp->target_ip);
        if (arp->hardware != net_be16(1) || arp->protocol != net_be16(0x800) ||
            arp->hardware_length != 6 || arp->protocol_length != 4 ||
            (operation != 1 && operation != 2) || (arp->sender_mac[0] & 1) ||
            memcmp(arp->sender_mac, h->source, 6) || target != n->address) {
            return;
        }
        if (source && (source & n->mask) == (n->address & n->mask) && source != n->address) {
            struct arp_entry *e = arp_entry(n, source);
            memcpy(e->mac, arp->sender_mac, 6);
            e->resolved = true;
            e->requests = 0;
            e->expires = hw_time_us() + 60000000;
        }
        if (operation == 1) {
            arp_send(n, 2, source, arp->sender_mac);
        }
    } else if (net_be16(h->type) == 0x800) {
        ip_input(n, payload, length);
    }
}
void net_poll(void) {
    if (__atomic_exchange_n(&network_lock.value, 1, __ATOMIC_ACQUIRE)) {
        return;
    }
    uint64_t now = hw_time_us();
    for (unsigned i = 0; i < net_interface_count; i++) {
        struct netif *n = &net_interfaces[i];
        if (now - last_link_poll >= 100000) {
            n->link = n->link_status(n);
        }
        uint8_t frame[1514];
        for (unsigned count = 0; count < 128; count++) {
            int length = n->receive(n, frame, sizeof(frame));
            if (!length) {
                break;
            }
            if (length < 0) {
                n->rx_errors++;
                continue;
            }
            n->rx_packets++;
            if (n->up && n->address) {
                frame_input(n, frame, length);
            }
        }
    }
    if (now - last_link_poll >= 100000) {
        last_link_poll = now;
    }
    for (unsigned i = 0; i < SOCKET_COUNT; i++) {
        struct net_socket *s = sockets[i];
        if (!s || s->type != 1) {
            continue;
        }
        if (s->state == TCP_SYN_SENT && s->connecting_arp) {
            int error = tcp_queue(s, TCP_SYN, NULL, 0);
            if (!error) {
                s->connecting_arp = false;
            } else if (error != -EAGAIN) {
                s->error = -error;
                s->state = TCP_CLOSED;
                s->connecting_arp = false;
            }
        }
        if (s->pending_flags && now >= s->deadline) {
            if (s->retries++ == 6) {
                s->error = 110;
                s->state = TCP_CLOSED;
                s->pending_flags = 0;
            } else {
                tcp_emit(s, s->pending_flags, s->pending, s->pending_length, s->send_unacked);
                s->deadline = now + (500000UL << MIN(s->retries, 4));
            }
        }
        if ((s->state == TCP_TIME_WAIT || s->state == TCP_FIN_WAIT2) && now >= s->deadline) {
            s->state = TCP_CLOSED;
        }
        if (s->write_shutdown && !s->pending_flags &&
            (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT)) {
            if (!tcp_queue(s, TCP_FIN | TCP_ACK, NULL, 0)) {
                s->state = s->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK : TCP_FIN_WAIT1;
            }
        }
        if (s->detached && (s->state == TCP_CLOSED || s->state == TCP_IDLE)) {
            sockets[i] = NULL;
            kfree(s);
        }
    }
    unlock(&network_lock);
}
static struct net_socket *get_socket(int fd) {
    struct process *p = this_cpu()->current;
    if (fd < 0 || fd >= MAX_FD || !p->fds[fd] || !p->fds[fd]->node->socket) {
        return NULL;
    }
    return p->fds[fd]->node->socket;
}
long net_socket_create(int domain, int type, int protocol) {
    if (domain != 2) {
        return -97;
    }
    if ((type & ~0x80800) < 1 || (type & ~0x80800) > 3 || (type & ~0x80803)) {
        return -EINVAL;
    }
    unsigned base = type & 15;
    if ((base == 1 && protocol && protocol != 6) || (base == 2 && protocol && protocol != 17) ||
        (base == 3 && protocol != 1)) {
        return -93;
    }
    if (base == 3 && this_cpu()->current->uid) {
        return -EPERM;
    }
    struct process *p = this_cpu()->current;
    unsigned fd = 0;
    while (fd < MAX_FD && p->fds[fd]) {
        fd++;
    }
    if (fd == MAX_FD) {
        return -24;
    }
    lock(&network_lock);
    unsigned index = 0;
    while (index < SOCKET_COUNT && sockets[index]) {
        index++;
    }
    if (index == SOCKET_COUNT) {
        unlock(&network_lock);
        return -ENOSPC;
    }
    struct net_socket *s = kalloc(sizeof(*s));
    struct file *f = kalloc(sizeof(*f));
    struct node *n = node_new("socket", 0140000 | 0600, NULL);
    if (!s || !f || !n) {
        kfree(s);
        kfree(f);
        kfree(n);
        unlock(&network_lock);
        return -ENOMEM;
    }
    s->type = base;
    s->protocol = protocol ? protocol : base == 1 ? 6 : 17;
    s->mss = 1460;
    n->socket = s;
    n->links = 0;
    n->refs = 1;
    f->node = n;
    f->flags = 2 | (type & 0x800);
    f->refs = 1;
    p->fds[fd] = f;
    p->fd_flags[fd] = type & O_CLOEXEC;
    sockets[index] = s;
    unlock(&network_lock);
    return fd;
}
static int address_read(struct sockaddr_in *a, uint64_t user, size_t n) {
    if (n < sizeof(*a)) {
        return -EINVAL;
    }
    int error = copy_from_user(a, user, sizeof(*a));
    if (error) {
        return error;
    }
    return a->family == 2 ? 0 : -97;
}
static bool port_used(struct net_socket *s, unsigned port, uint32_t address) {
    for (unsigned i = 0; i < SOCKET_COUNT; i++) {
        struct net_socket *other = sockets[i];
        if (other && other != s && other->type == s->type && other->local_port == port &&
            (!address || !other->local_address || address == other->local_address)) {
            return true;
        }
    }
    return false;
}
static int ephemeral(struct net_socket *s) {
    if (s->local_port) {
        return 0;
    }
    for (unsigned attempt = 0; attempt < 16384; attempt++) {
        uint16_t port = next_port++;
        if (next_port < 49152) {
            next_port = 49152;
        }
        if (!port_used(s, port, s->local_address)) {
            s->local_port = port;
            return 0;
        }
    }
    return -98;
}
long net_socket_bind(int fd, uint64_t user, size_t length) {
    struct net_socket *s = get_socket(fd);
    if (!s) {
        return -88;
    }
    struct sockaddr_in a;
    int error = address_read(&a, user, length);
    if (error) {
        return error;
    }
    uint32_t address = net_be32(a.address);
    uint16_t port = net_be16(a.port);
    if (port && port < 1024 && this_cpu()->current->uid) {
        return -EACCES;
    }
    lock(&network_lock);
    if (s->local_port || s->state != TCP_IDLE) {
        error = -EINVAL;
    } else if (address && !route(address, address)) {
        error = -99;
    } else if (port && port_used(s, port, address)) {
        error = -98;
    } else {
        s->local_address = address;
        s->local_port = port;
        error = ephemeral(s);
    }
    unlock(&network_lock);
    return error;
}
long net_socket_connect(int fd, uint64_t user, size_t length, bool retry) {
    net_poll();
    struct net_socket *s = get_socket(fd);
    if (!s) {
        return -88;
    }
    struct sockaddr_in a;
    int error = address_read(&a, user, length);
    if (error) {
        return error;
    }
    uint32_t address = net_be32(a.address);
    uint16_t port = net_be16(a.port);
    if (!address || (s->type != 3 && !port)) {
        return -EINVAL;
    }
    bool nonblocking = this_cpu()->current->fds[fd]->flags & 0x800;
    lock(&network_lock);
    if (s->error) {
        error = -s->error;
    } else if (s->type == 1 && s->state != TCP_IDLE) {
        if (s->remote_address != address || s->remote_port != port) {
            error = -106;
        } else if (s->state == TCP_SYN_SENT) {
            error = nonblocking ? -114 : -EAGAIN;
        } else {
            error = s->state == TCP_ESTABLISHED ? retry ? 0 : -106 : -107;
        }
    } else {
        struct netif *n = route(address, s->local_address);
        if (!n) {
            error = -101;
        } else {
            s->local_address = n->address;
            s->remote_address = address;
            s->remote_port = port;
            error = ephemeral(s);
            if (!error && s->type == 1) {
                s->send_next = (uint32_t)hw_time_us() ^ ((uint32_t)s->local_port << 16);
                error = tcp_queue(s, TCP_SYN, NULL, 0);
                if (!error || error == -EAGAIN) {
                    s->connecting_arp = error == -EAGAIN;
                    s->state = TCP_SYN_SENT;
                    error = nonblocking ? -115 : -EAGAIN;
                }
            }
        }
    }
    unlock(&network_lock);
    return error;
}
static long socket_send(struct net_socket *s, const void *data, size_t length, uint32_t address,
                        uint16_t port) {
    if (s->error) {
        return -s->error;
    }
    if (s->write_shutdown) {
        return -32;
    }
    if (s->type == 1) {
        if (s->state == TCP_SYN_SENT) {
            return -EAGAIN;
        }
        if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT) {
            return -107;
        }
        if (!length) {
            return 0;
        }
        size_t count = MIN(length, MIN(s->mss, s->remote_window));
        if (!count) {
            return -EAGAIN;
        }
        int error = tcp_queue(s, TCP_ACK | TCP_PSH, data, count);
        return error ? error : (long)count;
    }
    if (!address) {
        address = s->remote_address;
    }
    if (!port) {
        port = s->remote_port;
    }
    if (!address || (s->type == 2 && !port)) {
        return -89;
    }
    struct netif *n = route(address, s->local_address);
    if (!n) {
        return -101;
    }
    if (s->type == 3) {
        if (length < 8 || length > 1480) {
            return -90;
        }
        int error = ip_send(address, n->address, s->protocol, data, length);
        return error ? error : (long)length;
    }
    if (length > 1472) {
        return -90;
    }
    int error = ephemeral(s);
    if (error) {
        return error;
    }
    uint8_t packet[1480];
    struct udp_header *h = (void *)packet;
    *h = (struct udp_header){net_be16(s->local_port), net_be16(port), net_be16(length + 8), 0};
    memcpy(packet + 8, data, length);
    h->checksum = checksum(packet, length + 8, pseudo_sum(n->address, address, 17, length + 8));
    if (!h->checksum) {
        h->checksum = 0xffff;
    }
    error = ip_send(address, n->address, 17, packet, length + 8);
    return error ? error : (long)length;
}
long net_socket_write(struct net_socket *s, const void *data, size_t length) {
    net_poll();
    lock(&network_lock);
    long result = socket_send(s, data, length, 0, 0);
    unlock(&network_lock);
    return result;
}
static long socket_receive(struct net_socket *s, void *buffer, size_t length, unsigned flags,
                           struct sockaddr_in *address) {
    if (s->read_shutdown) {
        return 0;
    }
    if (s->type == 1) {
        if (s->state == TCP_IDLE) {
            return -107;
        }
        if (!length) {
            return 0;
        }
        if (s->receive_length) {
            size_t n = MIN(length, s->receive_length);
            for (size_t i = 0; i < n; i++) {
                ((uint8_t *)buffer)[i] = s->receive[(s->receive_tail + i) % RECEIVE_SIZE];
            }
            if (!(flags & 2)) {
                s->receive_tail += n;
                s->receive_length -= n;
                tcp_emit(s, TCP_ACK, NULL, 0, s->send_next);
            }
            return n;
        }
        if (s->error) {
            return -s->error;
        }
        return s->state == TCP_CLOSE_WAIT || s->state == TCP_TIME_WAIT || s->state == TCP_CLOSED
                   ? 0
                   : -EAGAIN;
    }
    if (!s->datagram_length) {
        return s->error ? -s->error : -EAGAIN;
    }
    struct datagram *d = &s->datagrams[s->datagram_tail];
    size_t count = MIN(length, d->length);
    memcpy(buffer, d->data, count);
    if (address) {
        *address = (struct sockaddr_in){
            .family = 2, .port = net_be16(d->port), .address = net_be32(d->address)};
    }
    if (!(flags & 2)) {
        s->datagram_tail = (s->datagram_tail + 1) % DATAGRAM_COUNT;
        s->datagram_length--;
    }
    return flags & 32 ? d->length : count;
}
long net_socket_read(struct net_socket *s, void *buffer, size_t length) {
    net_poll();
    lock(&network_lock);
    long result = socket_receive(s, buffer, length, 0, NULL);
    unlock(&network_lock);
    return result;
}
long net_socket_send(int fd, uint64_t user, size_t length, unsigned flags, uint64_t user_address,
                     size_t address_length) {
    struct net_socket *s = get_socket(fd);
    if (!s) {
        return -88;
    }
    if (flags & ~(0x40U | 0x4000U)) {
        return -95;
    }
    struct sockaddr_in address = {0};
    if (user_address) {
        int error = address_read(&address, user_address, address_length);
        if (error) {
            return error;
        }
        if (s->type == 1) {
            return -106;
        }
    }
    size_t count = s->type == 1 ? MIN(length, 1460) : length;
    if (count > 1480) {
        return -90;
    }
    uint8_t buffer[1480];
    int error = copy_from_user(buffer, user, count);
    if (error) {
        return error;
    }
    net_poll();
    lock(&network_lock);
    long result = socket_send(s, buffer, count, net_be32(address.address), net_be16(address.port));
    unlock(&network_lock);
    return result;
}
static int address_write(uint64_t user, uint64_t user_length, struct sockaddr_in *address) {
    uint32_t length;
    int error = copy_from_user(&length, user_length, sizeof(length));
    if (error) {
        return error;
    }
    error = copy_to_user(user, address, MIN(length, sizeof(*address)));
    if (error) {
        return error;
    }
    length = sizeof(*address);
    return copy_to_user(user_length, &length, sizeof(length));
}
long net_socket_receive(int fd, uint64_t user, size_t length, unsigned flags, uint64_t user_address,
                        uint64_t user_length) {
    struct net_socket *s = get_socket(fd);
    if (!s) {
        return -88;
    }
    if (flags & ~(2U | 32U | 64U)) {
        return -95;
    }
    size_t count = MIN(length, 16384);
    if (user_range(user, count, true)) {
        return -EFAULT;
    }
    if (user_address && (user_range(user_address, sizeof(struct sockaddr_in), true) ||
                         user_range(user_length, sizeof(uint32_t), true))) {
        return -EFAULT;
    }
    uint8_t *buffer = kalloc(count ? count : 1);
    if (!buffer) {
        return -ENOMEM;
    }
    struct sockaddr_in address = {
        .family = 2, .port = net_be16(s->remote_port), .address = net_be32(s->remote_address)};
    net_poll();
    lock(&network_lock);
    long result = socket_receive(s, buffer, count, flags, &address);
    if (result >= 0) {
        int error = copy_to_user(user, buffer, MIN((size_t)result, count));
        if (!error && user_address) {
            error = address_write(user_address, user_length, &address);
        }
        if (error) {
            result = error;
        }
    }
    unlock(&network_lock);
    kfree(buffer);
    return result;
}
long net_socket_option(int fd, int level, int option, uint64_t user, uint64_t user_length) {
    struct net_socket *s = get_socket(fd);
    if (!s) {
        return -88;
    }
    if (level != 1 || (option != 3 && option != 4 && option != 7 && option != 8)) {
        return -92;
    }
    uint32_t length;
    int error = copy_from_user(&length, user_length, sizeof(length));
    if (error) {
        return error;
    }
    if (user_range(user, MIN(length, sizeof(int)), true) ||
        user_range(user_length, sizeof(length), true)) {
        return -EFAULT;
    }
    net_poll();
    lock(&network_lock);
    int value = option == 3    ? s->type
                : option == 4  ? s->error
                : option == 7  ? 1460
                : s->type == 1 ? RECEIVE_SIZE
                               : DATAGRAM_COUNT * NET_MTU;
    error = copy_to_user(user, &value, MIN(length, sizeof(value)));
    if (!error) {
        length = MIN(length, sizeof(value));
        error = copy_to_user(user_length, &length, sizeof(length));
    }
    if (!error && option == 4) {
        s->error = 0;
    }
    unlock(&network_lock);
    return error;
}
long net_socket_name(int fd, uint64_t user, uint64_t length, bool peer) {
    struct net_socket *s = get_socket(fd);
    if (!s) {
        return -88;
    }
    if (peer && (!s->remote_address ||
                 (s->type == 1 && s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT))) {
        return -107;
    }
    struct sockaddr_in address = {.family = 2,
                                  .port = net_be16(peer ? s->remote_port : s->local_port),
                                  .address = net_be32(peer ? s->remote_address : s->local_address)};
    return address_write(user, length, &address);
}
long net_socket_shutdown(int fd, int how) {
    struct net_socket *s = get_socket(fd);
    if (!s) {
        return -88;
    }
    if (how < 0 || how > 2) {
        return -EINVAL;
    }
    lock(&network_lock);
    if (how != 1) {
        s->read_shutdown = true;
    }
    if (how != 0) {
        s->write_shutdown = true;
    }
    unlock(&network_lock);
    net_poll();
    return 0;
}
void net_socket_close(struct net_socket *s) {
    lock(&network_lock);
    s->detached = true;
    s->read_shutdown = s->write_shutdown = true;
    if (s->type != 1 || s->state == TCP_SYN_SENT || s->state == TCP_IDLE ||
        s->state == TCP_CLOSED) {
        for (unsigned i = 0; i < SOCKET_COUNT; i++) {
            if (sockets[i] == s) {
                sockets[i] = NULL;
            }
        }
        kfree(s);
    }
    unlock(&network_lock);
}
long net_ioctl(int fd, unsigned long request, uint64_t user) {
    if (!get_socket(fd)) {
        return -88;
    }
    if (request == 0x890b || request == 0x890c) {
        uint8_t entry[120];
        if (this_cpu()->current->uid) {
            return -EPERM;
        }
        int e = copy_from_user(entry, user, sizeof(entry));
        if (e) {
            return e;
        }
        if (*(uint16_t *)(entry + 8) != 2 || *(uint16_t *)(entry + 24) != 2 ||
            *(uint16_t *)(entry + 40) != 2 || *(uint32_t *)(entry + 12) ||
            *(uint32_t *)(entry + 44) || (*(uint16_t *)(entry + 56) & ~3U)) {
            return -EINVAL;
        }
        char name[16] = {0};
        uint64_t device = *(uint64_t *)(entry + 88);
        if (device && user_string(name, device, sizeof(name))) {
            return -EFAULT;
        }
        uint32_t gateway = net_be32(*(uint32_t *)(entry + 28));
        lock(&network_lock);
        e = -101;
        for (unsigned i = 0; i < net_interface_count; i++) {
            struct netif *n = &net_interfaces[i];
            if ((!device || !strcmp(name, n->name)) && n->address &&
                (gateway & n->mask) == (n->address & n->mask)) {
                n->gateway = request == 0x890b ? gateway : 0;
                e = 0;
                break;
            }
        }
        unlock(&network_lock);
        return e;
    }
    uint8_t ifreq[40];
    int error = copy_from_user(ifreq, user, sizeof(ifreq));
    if (error) {
        return error;
    }
    ifreq[15] = 0;
    lock(&network_lock);
    struct netif *n = NULL;
    for (unsigned i = 0; i < net_interface_count; i++) {
        if (!strcmp((char *)ifreq, net_interfaces[i].name)) {
            n = &net_interfaces[i];
        }
    }
    if (!n) {
        error = -19;
    } else {
        bool set = request == 0x8914 || request == 0x8916 || request == 0x891c;
        if (set && this_cpu()->current->uid) {
            error = -EPERM;
        } else if (request == 0x8913) {
            *(uint16_t *)(ifreq + 16) = (n->up ? 1 : 0) | 2 | (n->link ? 64 : 0);
        } else if (request == 0x8914) {
            n->up = (*(uint16_t *)(ifreq + 16) & 1) != 0;
        } else if (request == 0x8915 || request == 0x891b) {
            memset(ifreq + 16, 0, 24);
            *(uint16_t *)(ifreq + 16) = 2;
            *(uint32_t *)(ifreq + 20) = net_be32(request == 0x8915 ? n->address : n->mask);
        } else if (request == 0x8916 || request == 0x891c) {
            if (*(uint16_t *)(ifreq + 16) != 2) {
                error = -EINVAL;
            } else {
                uint32_t value = net_be32(*(uint32_t *)(ifreq + 20));
                if (request == 0x8916) {
                    if (value && (value >> 24 == 0 || value >> 24 >= 224)) {
                        error = -EINVAL;
                    } else {
                        n->address = value;
                    }
                } else if ((~value & (~value + 1)) != 0) {
                    error = -EINVAL;
                } else {
                    n->mask = value;
                }
                memset(arp_cache, 0, sizeof(arp_cache));
            }
        } else if (request == 0x8927) {
            memset(ifreq + 16, 0, 24);
            *(uint16_t *)(ifreq + 16) = 1;
            memcpy(ifreq + 18, n->mac, 6);
        } else if (request == 0x8921) {
            *(uint32_t *)(ifreq + 16) = NET_MTU;
        } else {
            error = -ENOSYS;
        }
    }
    if (!error) {
        error = copy_to_user(user, ifreq, sizeof(ifreq));
    }
    unlock(&network_lock);
    return error;
}
static size_t append(char *buffer, size_t capacity, size_t offset, const char *text) {
    while (*text && offset + 1 < capacity) {
        buffer[offset++] = *text++;
    }
    return offset;
}
static size_t number(char *buffer, size_t capacity, size_t offset, uint64_t value, unsigned base) {
    char digits[32];
    unsigned count = 0;
    do {
        digits[count++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value);
    while (count && offset + 1 < capacity) {
        buffer[offset++] = digits[--count];
    }
    return offset;
}
static size_t address_text(char *buffer, size_t capacity, size_t offset, uint32_t address) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        offset = number(buffer, capacity, offset, (address >> shift) & 255, 10);
        if (shift) {
            offset = append(buffer, capacity, offset, ".");
        }
    }
    return offset;
}
size_t net_status(char *buffer, size_t capacity) {
    net_poll();
    lock(&network_lock);
    size_t offset = 0;
    for (unsigned i = 0; i < net_interface_count; i++) {
        struct netif *n = &net_interfaces[i];
        offset = append(buffer, capacity, offset, n->name);
        offset = append(buffer, capacity, offset, n->link ? " link=up ip=" : " link=down ip=");
        offset = address_text(buffer, capacity, offset, n->address);
        offset = append(buffer, capacity, offset, " mask=");
        offset = address_text(buffer, capacity, offset, n->mask);
        offset = append(buffer, capacity, offset, " gateway=");
        offset = address_text(buffer, capacity, offset, n->gateway);
        offset = append(buffer, capacity, offset, " mac=");
        for (unsigned j = 0; j < 6; j++) {
            if (n->mac[j] < 16) {
                offset = append(buffer, capacity, offset, "0");
            }
            offset = number(buffer, capacity, offset, n->mac[j], 16);
            if (j < 5) {
                offset = append(buffer, capacity, offset, ":");
            }
        }
        offset = append(buffer, capacity, offset, " rx=");
        offset = number(buffer, capacity, offset, n->rx_packets, 10);
        offset = append(buffer, capacity, offset, " tx=");
        offset = number(buffer, capacity, offset, n->tx_packets, 10);
        offset = append(buffer, capacity, offset, " errors=");
        offset = number(buffer, capacity, offset, n->rx_errors + n->tx_errors, 10);
        offset = append(buffer, capacity, offset, "\n");
    }
    unlock(&network_lock);
    return offset;
}
