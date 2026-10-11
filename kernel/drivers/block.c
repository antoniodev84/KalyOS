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
