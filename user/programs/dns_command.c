#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
static unsigned get16(const uint8_t *p) {
    return ((unsigned)p[0] << 8) | p[1];
}
static int skip_name(const uint8_t *packet, unsigned length, unsigned *offset) {
    unsigned position = *offset, jumps = 0;
    bool redirected = false;
    while (position < length && jumps++ < 128) {
        unsigned count = packet[position++];
        if (!count) {
            if (!redirected) {
                *offset = position;
            }
            return 0;
        }
        if ((count & 0xc0) == 0xc0) {
            if (position >= length) {
                return -1;
            }
            unsigned pointer = ((count & 63) << 8) | packet[position++];
            if (pointer >= length) {
                return -1;
            }
            if (!redirected) {
                *offset = position;
            }
            redirected = true;
            position = pointer;
        } else {
            if (count > 63 || count > length - position) {
                return -1;
            }
            position += count;
        }
    }
    return -1;
}
int dns_resolve(const char *name, const char *server, struct in_addr *address) {
    if (inet_pton(AF_INET, name, address) == 1) {
        return 0;
    }
    size_t name_length = strlen(name);
    if (!name_length || name_length > 253) {
        errno = EINVAL;
        return -1;
    }
    struct sockaddr_in destination = {.sin_family = AF_INET, .sin_port = htons(53)};
    if (inet_pton(AF_INET, server, &destination.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    uint8_t request[512] = {0};
    uint16_t id = (uint16_t)getpid();
    *(uint16_t *)request = htons(id);
    request[2] = 1;
    request[5] = 1;
    unsigned length = 12;
    const char *p = name;
    while (*p) {
        const char *label = p;
        while (*p && *p != '.') {
            p++;
        }
        unsigned count = p - label;
        if (!count || count > 63 || length + count + 6 > sizeof(request)) {
            errno = EINVAL;
            return -1;
        }
        request[length++] = count;
        memcpy(request + length, label, count);
        length += count;
        if (*p) {
            p++;
        }
    }
    request[length++] = 0;
    request[length++] = 0;
    request[length++] = 1;
    request[length++] = 0;
    request[length++] = 1;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }
    int result = -1;
    for (unsigned attempt = 0; attempt < 300; attempt++) {
        if (sendto(fd, request, length, MSG_DONTWAIT, (void *)&destination, sizeof(destination)) >=
            0) {
            result = 0;
            break;
        }
        if (errno != EAGAIN) {
            break;
        }
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    if (result) {
        close(fd);
        return -1;
    }
    result = -1;
    errno = 110;
    for (unsigned attempt = 0; attempt < 500; attempt++) {
        uint8_t response[1500];
        struct sockaddr_in source;
        socklen_t address_length = sizeof(source);
        ssize_t count = recvfrom(fd, response, sizeof(response), MSG_DONTWAIT, (void *)&source,
                                 &address_length);
        if (count >= 12 && source.sin_addr.s_addr == destination.sin_addr.s_addr &&
            source.sin_port == destination.sin_port && get16(response) == id &&
            (response[2] & 0x80) && !(response[2] & 0x78)) {
            if ((response[3] & 15) || (response[2] & 2)) {
                errno = 2;
                break;
            }
            unsigned offset = 12, questions = get16(response + 4), answers = get16(response + 6);
            if (questions != 1 || skip_name(response, count, &offset) ||
                offset + 4 > (unsigned)count || get16(response + offset) != 1 ||
                get16(response + offset + 2) != 1) {
                errno = EIO;
                break;
            }
            offset += 4;
            for (unsigned i = 0; i < answers; i++) {
                if (skip_name(response, count, &offset) || offset + 10 > (unsigned)count) {
                    break;
                }
                unsigned type = get16(response + offset), class = get16(response + offset + 2);
                unsigned bytes = get16(response + offset + 8);
                offset += 10;
                if (bytes > (unsigned)count - offset) {
                    break;
                }
                if (type == 1 && class == 1 && bytes == 4) {
                    memcpy(address, response + offset, 4);
                    result = 0;
                    break;
                }
                offset += bytes;
            }
            if (result) {
                errno = 2;
            }
            break;
        } else if (count < 0 && errno != EAGAIN) {
            break;
        }
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    close(fd);
    return result;
}
int dns_command(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        return 2;
    }
    struct in_addr address;
    if (dns_resolve(argv[1], argc == 3 ? argv[2] : "10.0.2.3", &address)) {
        perror("DNS");
        return 1;
    }
    char text[16];
    if (!inet_ntop(AF_INET, &address, text, sizeof(text))) {
        return 1;
    }
    printf("%s %s\n", argv[1], text);
    return 0;
}
