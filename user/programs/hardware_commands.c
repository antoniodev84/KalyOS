#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <sys/mount.h>
int dns_resolve(const char *, const char *, struct in_addr *);
int dns_command(int, char **);
static int failure(const char *name) {
    perror(name);
    return 1;
}
static int status_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return failure(path);
    }
    char buffer[4096];
    ssize_t length;
    int result = 0;
    while ((length = read(fd, buffer, sizeof(buffer))) > 0) {
        ssize_t offset = 0;
        while (offset < length) {
            ssize_t count = write(1, buffer + offset, length - offset);
            if (count <= 0) {
                result = failure("stdout");
                break;
            }
            offset += count;
        }
        if (result) {
            break;
        }
    }
    if (length < 0) {
        result = failure(path);
    }
    close(fd);
    return result;
}
static int configure_interface(int argc, char **argv) {
    if (argc == 1) {
        return status_file("/proc/net");
    }
    if (argc != 4 && argc != 5) {
        fputs("ifconfig interface IPv4 netmask [gateway]\n", stderr);
        return 2;
    }
    if (strlen(argv[1]) >= 16) {
        return 2;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return failure("socket");
    }
    struct ifreq request = {0};
    strcpy(request.ifr_name, argv[1]);
    struct sockaddr_in *address = (void *)&request.ifr_addr;
    address->sin_family = AF_INET;
    int result = 0;
    if (inet_pton(AF_INET, argv[3], &address->sin_addr) != 1) {
        result = 2;
    } else if (ioctl(fd, SIOCSIFNETMASK, &request)) {
        result = failure("netmask");
    }
    if (!result) {
        if (inet_pton(AF_INET, argv[2], &address->sin_addr) != 1) {
            result = 2;
        } else if (ioctl(fd, SIOCSIFADDR, &request)) {
            result = failure("address");
        }
    }
    if (!result) {
        request.ifr_flags = IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &request)) {
            result = failure("up");
        }
    }
    if (!result && argc == 5) {
        struct rtentry route = {.rt_flags = RTF_UP | RTF_GATEWAY, .rt_dev = argv[1]};
        ((struct sockaddr_in *)&route.rt_dst)->sin_family = AF_INET;
        ((struct sockaddr_in *)&route.rt_genmask)->sin_family = AF_INET;
        address = (void *)&route.rt_gateway;
        address->sin_family = AF_INET;
        if (inet_pton(AF_INET, argv[4], &address->sin_addr) != 1) {
            result = 2;
        } else if (ioctl(fd, SIOCADDRT, &route)) {
            result = failure("route");
        }
    }
    close(fd);
    return result;
}
static uint16_t internet_checksum(const uint8_t *packet, size_t length) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < length; i += 2) {
        sum += ((unsigned)packet[i] << 8) | packet[i + 1];
    }
    if (length & 1) {
        sum += (unsigned)packet[length - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 65535) + (sum >> 16);
    }
    return htons(~sum);
}
static void pause_poll(void) {
    struct timespec pause = {0, 10000000};
    nanosleep(&pause, NULL);
}
static int ping_host(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        return 2;
    }
    struct sockaddr_in address = {.sin_family = AF_INET};
    if (dns_resolve(argv[1], "10.0.2.3", &address.sin_addr)) {
        return 2;
    }
    unsigned count = argc == 3 ? strtoul(argv[2], NULL, 10) : 4;
    if (!count || count > 1000) {
        return 2;
    }
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        return failure("socket");
    }
    unsigned received = 0;
    for (unsigned sequence = 0; sequence < count; sequence++) {
        uint8_t request[64] = {8, 0};
        *(uint16_t *)(request + 4) = htons(getpid());
        *(uint16_t *)(request + 6) = htons(sequence);
        for (unsigned i = 8; i < sizeof(request); i++) {
            request[i] = i;
        }
        *(uint16_t *)(request + 2) = internet_checksum(request, sizeof(request));
        bool sent = false;
        for (unsigned attempt = 0; attempt < 300; attempt++) {
            if (sendto(fd, request, sizeof(request), MSG_DONTWAIT, (void *)&address,
                       sizeof(address)) >= 0) {
                sent = true;
                break;
            }
            if (errno != EAGAIN) {
                break;
            }
            pause_poll();
        }
        if (!sent) {
            perror("ping send");
            continue;
        }
        bool matched = false;
        for (unsigned attempt = 0; attempt < 300; attempt++) {
            uint8_t response[1500];
            ssize_t length = recv(fd, response, sizeof(response), MSG_DONTWAIT);
            if (length >= 28) {
                unsigned header_length = (response[0] & 15) * 4;
                if (header_length >= 20 && header_length + 8 <= (unsigned)length &&
                    response[header_length] == 0 &&
                    *(uint32_t *)(response + 12) == address.sin_addr.s_addr &&
                    *(uint16_t *)(response + header_length + 4) == htons(getpid()) &&
                    *(uint16_t *)(response + header_length + 6) == htons(sequence)) {
                    printf("%ld bytes from %s: icmp_seq=%u ttl=%u\n", length - header_length,
                           argv[1], sequence, response[8]);
                    received++;
                    matched = true;
                    break;
                }
            } else if (length < 0 && errno != EAGAIN) {
                break;
            }
            pause_poll();
        }
        if (!matched) {
            printf("timeout icmp_seq=%u\n", sequence);
        }
    }
    close(fd);
    printf("%u transmitted, %u received\n", count, received);
    return received == count ? 0 : 1;
}
static int http_get(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        fputs("wget IPv4 [path] [port]\n", stderr);
        return 2;
    }
    unsigned port = argc == 4 ? strtoul(argv[3], NULL, 10) : 80;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (!port || port > 65535 || dns_resolve(argv[1], "10.0.2.3", &address.sin_addr)) {
        return 2;
    }
    const char *path = argc >= 3 ? argv[2] : "/";
    if (*path != '/' || strchr(path, '\r') || strchr(path, '\n')) {
        return 2;
    }
    char request[1024];
    int length =
        snprintf(request, sizeof(request),
                 "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, argv[1]);
    if (length < 0 || (unsigned)length >= sizeof(request)) {
        return 2;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return failure("socket");
    }
    int result = 0;
    if (connect(fd, (void *)&address, sizeof(address))) {
        result = failure("connect");
    }
    for (int offset = 0; !result && offset < length;) {
        ssize_t sent = send(fd, request + offset, length - offset, MSG_NOSIGNAL);
        if (sent <= 0) {
            result = failure("send");
        } else {
            offset += sent;
        }
    }
    if (!result) {
        char buffer[4096];
        ssize_t count;
        while ((count = recv(fd, buffer, sizeof(buffer), 0)) > 0) {
            for (ssize_t offset = 0; offset < count;) {
                ssize_t written = write(1, buffer + offset, count - offset);
                if (written <= 0) {
                    result = failure("stdout");
                    break;
                }
                offset += written;
            }
            if (result) {
                break;
            }
        }
        if (count < 0) {
            result = failure("recv");
        }
    }
    close(fd);
    return result;
}
static int udp_exchange(int argc, char **argv) {
    if (argc != 4) {
        fputs("udp IPv4 port message\n", stderr);
        return 2;
    }
    unsigned port = strtoul(argv[2], NULL, 10);
    struct sockaddr_in destination = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (!port || port > 65535 || inet_pton(AF_INET, argv[1], &destination.sin_addr) != 1 ||
        strlen(argv[3]) > 1472) {
        return 2;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return failure("socket");
    }
    int result = 1;
    for (unsigned attempt = 0; attempt < 300; attempt++) {
        if (sendto(fd, argv[3], strlen(argv[3]), MSG_DONTWAIT, (void *)&destination,
                   sizeof(destination)) >= 0) {
            result = 0;
            break;
        }
        if (errno != EAGAIN) {
            perror("udp send");
            break;
        }
        pause_poll();
    }
    if (!result) {
        result = 1;
        for (unsigned attempt = 0; attempt < 300; attempt++) {
            char buffer[1472];
            struct sockaddr_in source;
            socklen_t size = sizeof(source);
            ssize_t count =
                recvfrom(fd, buffer, sizeof(buffer), MSG_DONTWAIT, (void *)&source, &size);
            if (count >= 0 && source.sin_addr.s_addr == destination.sin_addr.s_addr &&
                source.sin_port == destination.sin_port) {
                ssize_t offset = 0;
                while (offset < count) {
                    ssize_t written = write(1, buffer + offset, count - offset);
                    if (written <= 0) {
                        break;
                    }
                    offset += written;
                }
                if (offset == count) {
                    putchar('\n');
                    result = 0;
                }
                break;
            }
            if (count < 0 && errno != EAGAIN) {
                perror("udp receive");
                break;
            }
            pause_poll();
        }
    }
    if (result) {
        fputs("udp: exchange failed or timed out\n", stderr);
    }
    close(fd);
    return result;
}
int sound_command(int, char **);
int hardware_command(const char *command, int argc, char **argv) {
    if (!strcmp(command, "udp")) {
        return udp_exchange(argc, argv);
    }
    if (!strcmp(command, "nslookup")) {
        return dns_command(argc, argv);
    }
    if (!strcmp(command, "ifconfig")) {
        return configure_interface(argc, argv);
    }
    if (!strcmp(command, "ping")) {
        return ping_host(argc, argv);
    }
    if (!strcmp(command, "wget")) {
        return http_get(argc, argv);
    }
    if (!strcmp(command, "netstat")) {
        return status_file("/proc/net");
    }
    if (!strcmp(command, "sound")) {
        return sound_command(argc, argv);
    }
    if (!strcmp(command, "pendrive")) {
        if (argc == 1) {
            return status_file("/sys/block");
        }
        if (argc == 2 && !strcmp(argv[1], "rescan")) {
            int fd = open("/dev/usb", O_RDONLY);
            if (fd < 0) {
                return failure("usb");
            }
            int result = ioctl(fd, 0x5501, NULL) ? failure("usb rescan") : 0;
            close(fd);
            return result ? result : status_file("/sys/block");
        }
        if (argc == 4 && !strcmp(argv[1], "mount")) {
            return mount(argv[2], argv[3], "ext2", 0, NULL) ? failure("pendrive mount") : 0;
        }
        if (argc == 3 && !strcmp(argv[1], "umount")) {
            return umount(argv[2]) ? failure("pendrive umount") : 0;
        }
        fputs("pendrive [rescan | mount device directory | umount directory]\n", stderr);
        return 2;
    }
    if (!strcmp(command, "shutdown")) {
        if (argc != 1) {
            return 2;
        }
        return syscall(169, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0) < 0 ? failure("shutdown")
                                                                            : 0;
    }
    return -1;
}
