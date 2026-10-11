#include "kaly.h"
struct ahci_port {
    volatile uint32_t clb, clbu, fb, fbu, is, ie, cmd, reserved, tfd, sig, ssts, sctl, serr, sact,
        ci, sntf, fbs;
    uint32_t rest[15];
};
struct ahci_header {
    uint16_t flags, prdtl;
    volatile uint32_t transferred;
    uint32_t table, table_high, reserved[4];
} PACKED;
struct ahci_disk {
    struct blockdev block;
    volatile struct ahci_port *port;
    struct ahci_header *headers;
    uint8_t *table, *bounce;
    struct lock lock;
};
static unsigned disk_count;
static int wait_clear(volatile uint32_t *r, uint32_t mask) {
    for (uint64_t i = 0; i < 10000000; i++) {
        if (!(*r & mask)) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -EIO;
}
static int command(struct ahci_disk *d, uint8_t cmd, uint64_t lba, uint16_t sectors, bool write,
                   uint32_t bytes) {
    if (wait_clear(&d->port->tfd, 0x88)) {
        return -EIO;
    }
    memset(d->table, 0, 256);
    struct ahci_header *h = d->headers;
    h->flags = 5 | (write ? 64 : 0);
    h->prdtl = 1;
    h->transferred = 0;
    h->table = physical(d->table);
    h->table_high = physical(d->table) >> 32;
    uint8_t *fis = d->table;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = cmd;
    fis[4] = lba;
    fis[5] = lba >> 8;
    fis[6] = lba >> 16;
    fis[7] = 0x40;
    fis[8] = lba >> 24;
    fis[9] = lba >> 32;
    fis[10] = lba >> 40;
    fis[12] = sectors;
    fis[13] = sectors >> 8;
    uint32_t *prdt = (uint32_t *)(fis + 128);
    uint64_t p = physical(d->bounce);
    prdt[0] = p;
    prdt[1] = p >> 32;
    prdt[3] = bytes - 1;
    d->port->is = 0xffffffff;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d->port->ci = 1;
    if (wait_clear(&d->port->ci, 1) || (d->port->is & (1U << 30))) {
        return -EIO;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (h->transferred != bytes) {
        return -EIO;
    }
    return 0;
}
static int disk_transfer(struct blockdev *b, uint64_t lba, uint32_t n, void *buf, bool write) {
    struct ahci_disk *d = b->private;
    lock(&d->lock);
    int e = 0;
    while (n) {
        uint32_t chunk = MIN(n, 128), bytes = chunk * 512;
        if (write) {
            memcpy(d->bounce, buf, bytes);
        }
        e = command(d, write ? 0x35 : 0x25, lba, chunk, write, bytes);
        if (e) {
            break;
        }
        if (!write) {
            memcpy(buf, d->bounce, bytes);
        }
        buf = (uint8_t *)buf + bytes;
        lba += chunk;
        n -= chunk;
    }
    if (!e && write) {
        d->port->is = 0xffffffff;
        memset(d->table, 0, 256);
        d->headers->flags = 5;
        d->headers->prdtl = 0;
        d->table[0] = 0x27;
        d->table[1] = 0x80;
        d->table[2] = 0xea;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        d->port->ci = 1;
        if (wait_clear(&d->port->ci, 1) || (d->port->is & (1U << 30))) {
            e = -EIO;
        }
    }
    unlock(&d->lock);
    return e;
}
void ahci_probe(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t bar = pci_read(bus, dev, fn, 0x24);
    if ((bar & 1) || !(bar & ~15U)) {
        return;
    }
    uint32_t command_reg = pci_read(bus, dev, fn, 4);
    pci_write(bus, dev, fn, 4, (command_reg & 0xffff) | 6);
    volatile uint32_t *hba = phys(bar & ~15U);
    if (hba[9] & 1) {
        hba[10] |= 2;
        if (wait_clear(&hba[10], 17)) {
            kprintf("AHCI ownership error\n");
            return;
        }
    }
    hba[1] |= 1U << 31;
    hba[1] &= ~2U;
    uint32_t ports = hba[3];
    for (unsigned i = 0; i < 32; i++) {
        if (!(ports & (1U << i))) {
            continue;
        }
        volatile struct ahci_port *p = (void *)((uint8_t *)hba + 0x100 + i * 0x80);
        if ((p->ssts & 15) != 3 || ((p->ssts >> 8) & 15) != 1 || p->sig != 0x101) {
            continue;
        }
        p->cmd &= ~1U;
        if (wait_clear(&p->cmd, 1U << 15)) {
            continue;
        }
        p->cmd &= ~(1U << 4);
        if (wait_clear(&p->cmd, 1U << 14)) {
            continue;
        }
        struct ahci_disk *d = kalloc(sizeof(*d));
        uint64_t hdr = pmm_alloc(), tbl = pmm_alloc(), bounce = pmm_alloc_contiguous(16),
                 received = pmm_alloc();
        if (!d || !hdr || !tbl || !bounce || !received) {
            panic("ahci memory");
        }
        d->port = p;
        d->headers = phys(hdr);
        d->table = phys(tbl);
        d->bounce = phys(bounce);
        p->clb = hdr;
        p->clbu = hdr >> 32;
        p->fb = received;
        p->fbu = received >> 32;
        p->ie = 0;
        p->serr = 0xffffffff;
        p->is = 0xffffffff;
        p->cmd |= (1U << 4) | 1;
        if (command(d, 0xec, 0, 0, false, 512)) {
            kprintf("AHCI identify error\n");
            continue;
        }
        uint16_t *id = (void *)d->bounce;
        if (!(id[83] & (1 << 10)) || (id[106] & 0xd000) == 0x5000) {
            kprintf("AHCI sector format error\n");
            continue;
        }
        uint64_t size = *(uint64_t *)(id + 100) & 0x0000ffffffffffffUL;
        strcpy(d->block.name, "sda");
        d->block.name[2] += disk_count++;
        d->block.sectors = size;
        d->block.sector_size = 512;
        d->block.private = d;
        d->block.transfer = disk_transfer;
        block_register(&d->block);
        kprintf("AHCI %s %u sectors\n", d->block.name, size);
    }
}
