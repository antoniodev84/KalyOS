#include "kaly.h"
struct header {
    char signature[4];
    uint32_t length;
    uint8_t revision, checksum;
    uint8_t rest[26];
} PACKED;
struct gas {
    uint8_t space, width, offset, access;
    uint64_t address;
} PACKED;
static struct gas pm1a, pm1b, sleep_control;
static uint32_t smi;
static uint8_t enable;
static unsigned sleep_a, sleep_b;
static bool available, reduced;
static bool checksum(const void *p, size_t n) {
    const uint8_t *b = p;
    uint8_t sum = 0;
    while (n--) {
        sum += *b++;
    }
    return !sum;
}
static struct header *table(uint64_t address, const char *sig) {
    if (!address || address > 0x100000000UL - sizeof(struct header)) {
        return NULL;
    }
    struct header *h = phys(address);
    if (h->length < sizeof(*h) || h->length > 1024 * 1024 || h->length > 0x100000000UL - address ||
        memcmp(h->signature, sig, 4) || !checksum(h, h->length)) {
        return NULL;
    }
    return h;
}
static int integer(const uint8_t **p, const uint8_t *end, unsigned *out) {
    if (*p >= end) {
        return -EINVAL;
    }
    uint8_t op = *(*p)++;
    if (op <= 1) {
        *out = op;
        return 0;
    }
    unsigned size = op == 0x0a ? 1 : op == 0x0b ? 2 : op == 0x0c ? 4 : op == 0x0e ? 8 : 0;
    if (!size || (size_t)(end - *p) < size) {
        return -EINVAL;
    }
    uint64_t value = 0;
    for (unsigned i = 0; i < size; i++) {
        value |= (uint64_t)*(*p)++ << (i * 8);
    }
    if (value > 7) {
        return -EINVAL;
    }
    *out = value;
    return 0;
}
static bool s5(struct header *dsdt) {
    const uint8_t *start = (void *)(dsdt + 1), *end = (uint8_t *)dsdt + dsdt->length;
    for (const uint8_t *p = start; p + 7 < end; p++) {
        if (memcmp(p, "_S5_", 4) || p == start ||
            !(p[-1] == 8 || (p >= start + 2 && p[-2] == 8 && p[-1] == '\\')) || p[4] != 0x12) {
            continue;
        }
        const uint8_t *q = p + 5, *pkg = q;
        unsigned follow = *q >> 6;
        uint32_t length = *q++ & (follow ? 15 : 63);
        if (q + follow >= end) {
            continue;
        }
        for (unsigned i = 0; i < follow; i++) {
            length |= (uint32_t)*q++ << (4 + i * 8);
        }
        if (length > (size_t)(end - pkg) || length <= (unsigned)(q - pkg) || *q++ < 2) {
            continue;
        }
        if (!integer(&q, pkg + length, &sleep_a) && !integer(&q, pkg + length, &sleep_b)) {
            return true;
        }
    }
    return false;
}
static bool valid_gas(struct gas *g, unsigned width) {
    if (!g->address || g->offset || g->width < width || g->width > 32 || g->space > 1) {
        return false;
    }
    if (g->space == 1) {
        return g->address <= 65532;
    }
    return mmio_map(g->address, 4) != NULL;
}
static uint32_t gas_read(struct gas g) {
    if (g.space == 1) {
        return g.width <= 8 ? inb(g.address) : g.width <= 16 ? inw(g.address) : inl(g.address);
    }
    return g.width <= 8    ? *(volatile uint8_t *)phys(g.address)
           : g.width <= 16 ? *(volatile uint16_t *)phys(g.address)
                           : *(volatile uint32_t *)phys(g.address);
}
static void gas_write(struct gas g, uint32_t value) {
    if (g.space == 1) {
        if (g.width <= 8) {
            outb(g.address, value);
        } else if (g.width <= 16) {
            outw(g.address, value);
        } else {
            outl(g.address, value);
        }
    } else {
        if (g.width <= 8) {
            *(volatile uint8_t *)phys(g.address) = value;
        } else if (g.width <= 16) {
            *(volatile uint16_t *)phys(g.address) = value;
        } else {
            *(volatile uint32_t *)phys(g.address) = value;
        }
    }
}
void acpi_init(uint64_t info) {
    uint8_t *rsdp = NULL;
    unsigned rsdp_size = 0;
    uint32_t total = *(uint32_t *)info;
    for (uint64_t t = info + 8; t + 8 <= info + total;) {
        unsigned size = *(uint32_t *)(t + 4), type = *(uint32_t *)t;
        if (!type || size < 8 || size > info + total - t) {
            break;
        }
        if ((type == 14 && size >= 28) || (type == 15 && size >= 44)) {
            rsdp = (void *)(t + 8);
            rsdp_size = size - 8;
        }
        t += ALIGN_UP(size, 8);
    }
    if (!rsdp || memcmp(rsdp, "RSD PTR ", 8) || !checksum(rsdp, 20)) {
        return;
    }
    bool ext = rsdp_size >= 36 && rsdp[15] >= 2 && checksum(rsdp, 36) && *(uint64_t *)(rsdp + 24);
    struct header *root_table =
        table(ext ? *(uint64_t *)(rsdp + 24) : *(uint32_t *)(rsdp + 16), ext ? "XSDT" : "RSDT");
    if (!root_table) {
        return;
    }
    struct header *fadt = NULL;
    for (unsigned off = 36; off + (ext ? 8 : 4) <= root_table->length; off += ext ? 8 : 4) {
        uint8_t *p = (uint8_t *)root_table + off;
        fadt = table(ext ? *(uint64_t *)p : *(uint32_t *)p, "FACP");
        if (fadt) {
            break;
        }
    }
    if (!fadt || fadt->length < 116) {
        return;
    }
    uint8_t *f = (void *)fadt;
    uint64_t dsdt_address = fadt->length >= 148 && *(uint64_t *)(f + 140) ? *(uint64_t *)(f + 140)
                                                                          : *(uint32_t *)(f + 40);
    struct header *dsdt = table(dsdt_address, "DSDT");
    if (!dsdt || !s5(dsdt)) {
        return;
    }
    reduced = (*(uint32_t *)(f + 112) & (1U << 20)) != 0;
    if (reduced) {
        if (fadt->length < 256) {
            return;
        }
        memcpy(&sleep_control, f + 244, sizeof(sleep_control));
        if (!valid_gas(&sleep_control, 8)) {
            return;
        }
    } else {
        smi = *(uint32_t *)(f + 48);
        enable = f[52];
        pm1a = (struct gas){1, 16, 0, 2, *(uint32_t *)(f + 64)};
        pm1b = (struct gas){1, 16, 0, 2, *(uint32_t *)(f + 68)};
        if (fadt->length >= 196) {
            struct gas a, b;
            memcpy(&a, f + 172, sizeof(a));
            memcpy(&b, f + 184, sizeof(b));
            if (a.address) {
                pm1a = a;
            }
            if (b.address) {
                pm1b = b;
            }
        }
        if (!valid_gas(&pm1a, 16) || (pm1b.address && !valid_gas(&pm1b, 16))) {
            return;
        }
    }
    available = true;
    kprintf("ACPI FADT S5 ready\n");
}
int acpi_shutdown(void) {
    if (!available) {
        return -ENOSYS;
    }
    for (struct blockdev *d = block_devices; d; d = d->next) {
        if (d->mounted) {
            int e = ext2_sync(d->mounted);
            if (e) {
                return e;
            }
        }
    }
    if (reduced) {
        gas_write(sleep_control, (sleep_a << 2) | 32);
    } else {
        if (!(gas_read(pm1a) & 1)) {
            if (!smi || smi > 65535 || !enable) {
                return -EIO;
            }
            outb(smi, enable);
            uint64_t end = hw_time_us() + 3000000;
            while (!(gas_read(pm1a) & 1)) {
                if (hw_time_us() >= end) {
                    return -110;
                }
            }
        }
        gas_write(pm1a, (gas_read(pm1a) & ~0x3c00U) | (sleep_a << 10) | 0x2000);
        if (pm1b.address) {
            gas_write(pm1b, (gas_read(pm1b) & ~0x3c00U) | (sleep_b << 10) | 0x2000);
        }
    }
    hw_delay(100000);
    return -EIO;
}
