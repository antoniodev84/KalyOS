#include "kaly.h"
void vfs_init(uint64_t info) {
    root = node_new("", S_IFDIR | 0755, NULL);
    if (!root) {
        panic("vfs memory");
    }
    uint32_t total = *(uint32_t *)info;
    for (uint64_t t = info + 8; t < info + total; t += ALIGN_UP(*(uint32_t *)(t + 4), 8)) {
        if (!*(uint32_t *)t) {
            break;
        }
        if (*(uint32_t *)t != 3) {
            continue;
        }
        uint8_t *p = phys(*(uint32_t *)(t + 8)), *end = phys(*(uint32_t *)(t + 12));
        while (p + 110 <= end) {
            if (memcmp(p, "070701", 6)) {
                panic("initramfs format");
            }
            char field[9];
            field[8] = 0;
            memcpy(field, p + 14, 8);
            uint32_t mode = strtou(field, 16);
            memcpy(field, p + 54, 8);
            uint64_t size = strtou(field, 16);
            memcpy(field, p + 94, 8);
            uint32_t namesize = strtou(field, 16);
            if (!namesize || namesize > MAX_PATH || p + 110 + namesize > end ||
                p[110 + namesize - 1]) {
                panic("initramfs name");
            }
            char *name = (char *)p + 110;
            uint8_t *data = p + ALIGN_UP(110 + namesize, 4);
            if (data > end || size > (uint64_t)(end - data)) {
                panic("initramfs size");
            }
            if (!strcmp(name, "TRAILER!!!")) {
                break;
            }
            while (!strncmp(name, "./", 2)) {
                name += 2;
            }
            if (strcmp(name, ".") && *name) {
                char path[MAX_PATH];
                path[0] = '/';
                if (strlen(name) + 2 > sizeof(path)) {
                    panic("initramfs path");
                }
                strcpy(path + 1, name);
                int e;
                struct node *n = vfs_create(path, mode, NULL, &e);
                if (!n) {
                    panic("initramfs node");
                }
                if (size && node_write(n, 0, data, size) != (long)size) {
                    panic("initramfs data");
                }
            }
            p = data + ALIGN_UP(size, 4);
        }
        break;
    }
    int e;
    struct node *dev = vfs_resolve("/dev", root, true, &e);
    if (!dev) {
        dev = vfs_create("/dev", S_IFDIR | 0755, NULL, &e);
    }
    struct node *n = node_new("console", S_IFCHR | 0666, dev);
    if (!n) {
        panic("dev memory");
    }
    n->device = 1;
    n = node_new("null", S_IFCHR | 0666, dev);
    if (!n) {
        panic("dev memory");
    }
    n->device = 2;
    n = node_new("zero", S_IFCHR | 0666, dev);
    if (!n) {
        panic("dev memory");
    }
    n->device = 3;
    n = node_new("dsp", S_IFCHR | 0666, dev);
    if (!n) {
        panic("audio node");
    }
    n->device = 5;
    n = node_new("usb", S_IFCHR | 0600, dev);
    if (!n) {
        panic("usb node");
    }
    n->device = 6;
    n = node_new("event0", S_IFCHR | 0444, dev);
    if (!n) {
        panic("input node");
    }
    n->device = 4;
    for (struct blockdev *d = block_devices; d; d = d->next) {
        n = node_new(d->name, S_IFBLK | 0600, dev);
        if (!n) {
            panic("dev memory");
        }
        n->size = d->sectors * 512;
    }
    struct node *proc = vfs_resolve("/proc", root, true, &e);
    if (!proc) {
        proc = vfs_create("/proc", S_IFDIR | 0555, NULL, &e);
    }
    const char *names[] = {"meminfo", "cpuinfo", "uptime", "processes"};
    for (unsigned i = 0; i < 4; i++) {
        n = node_new(names[i], S_IFREG | 0444, proc);
        if (!n) {
            panic("proc memory");
        }
        n->device = 10 + i;
    }
    n = node_new("net", S_IFREG | 0444, proc);
    if (!n) {
        panic("net node");
    }
    n->device = 16;
    n = node_new("sound", S_IFREG | 0444, proc);
    if (!n) {
        panic("sound node");
    }
    n->device = 17;
    struct node *sys = vfs_resolve("/sys", root, true, &e);
    if (!sys) {
        sys = vfs_create("/sys", S_IFDIR | 0555, NULL, &e);
    }
    n = node_new("cpus", S_IFREG | 0444, sys);
    if (!n) {
        panic("sys memory");
    }
    n->device = 14;
    n = node_new("block", S_IFREG | 0444, sys);
    if (!n) {
        panic("sys memory");
    }
    n->device = 15;
}
