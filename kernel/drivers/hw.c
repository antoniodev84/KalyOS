#include "kaly.h"
static uint64_t cycles_per_us;
static uint64_t counter(void) {
    uint32_t a, d;
    __asm__ volatile("lfence; rdtsc" : "=a"(a), "=d"(d));
    return ((uint64_t)d << 32) | a;
}
void hw_init(void) {
    uint8_t gate = inb(0x61);
    outb(0x61, gate & ~3U);
    outb(0x43, 0xb0);
    outb(0x42, 0x9c);
    outb(0x42, 0x2e);
    uint64_t start = counter();
    outb(0x61, (gate & ~2U) | 1);
    while (!(inb(0x61) & 32)) {
        __asm__ volatile("pause");
    }
    cycles_per_us = MAX((counter() - start) / 10000, 1);
    outb(0x61, gate);
}
uint64_t hw_time_us(void) {
    return counter() / cycles_per_us;
}
void hw_delay(unsigned us) {
    uint64_t end = hw_time_us() + us;
    while (hw_time_us() < end) {
        __asm__ volatile("pause");
    }
}
int hw_wait32(volatile uint32_t *r, uint32_t mask, uint32_t value, unsigned us) {
    uint64_t end = hw_time_us() + us;
    do {
        if ((*r & mask) == value) {
            return 0;
        }
        __asm__ volatile("pause");
    } while (hw_time_us() < end);
    return -110;
}
void *mmio_map(uint64_t address, size_t size) {
    if (!address || address >= 0x100000000UL || size > 0x100000000UL - address) {
        return NULL;
    }
    uint64_t *pdpt = phys(boot_pml4[256] & ~0xfffUL);
    for (uint64_t p = address & ~0x1fffffUL; p < address + size; p += 0x200000) {
        uint64_t *pd = phys(pdpt[p >> 30] & ~0xfffUL);
        pd[(p >> 21) & 511] |= 24;
        __asm__ volatile("invlpg (%0)" ::"r"(phys(p)) : "memory");
    }
    return phys(address);
}
void *pci_mmio(uint8_t b, uint8_t d, uint8_t f, unsigned bar, size_t size) {
    uint32_t low = pci_read(b, d, f, 0x10 + bar * 4);
    if ((low & 1) || !(low & ~15U) ||
        ((low & 6) == 4 && (bar == 5 || pci_read(b, d, f, 0x14 + bar * 4)))) {
        return NULL;
    }
    pci_write(b, d, f, 4, (pci_read(b, d, f, 4) & 0xffff) | 6 | 0x400);
    return mmio_map(low & ~15U, size);
}
