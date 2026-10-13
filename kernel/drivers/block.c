#include "kaly.h"
struct blockdev *block_devices;
void block_register(struct blockdev *d) {
    d->next = block_devices;
    block_devices = d;
}
static int transfer(struct blockdev *d, uint64_t lba, uint32_t n, void *buf, bool write) {
    if (!d || !d->transfer || lba > d->sectors || n > d->sectors - lba) {
        return -EINVAL;
    }
    if (!n) {
        return 0;
    }
    return d->transfer(d, lba, n, buf, write);
}
int block_read(struct blockdev *d, uint64_t l, uint32_t n, void *b) {
    return transfer(d, l, n, b, false);
}
int block_write(struct blockdev *d, uint64_t l, uint32_t n, const void *b) {
    return transfer(d, l, n, (void *)b, true);
}

void block_publish(void) {
    if (!root) {
        return;
    }
    int error;
    struct node *dev = vfs_resolve("/dev", root, true, &error);
    if (!dev) {
        return;
    }
    for (struct blockdev *d = block_devices; d; d = d->next) {
        bool present = false;
        for (struct node *n = dev->children; n; n = n->next) {
            if (!strcmp(n->name, d->name)) {
                present = true;
            }
        }
        if (present) {
            continue;
        }
        struct node *n = node_new(d->name, S_IFBLK | 0600, dev);
        if (!n) {
            kprintf("Block device node allocation failed\n");
            return;
        }
        n->size = d->sectors * d->sector_size;
    }
}
