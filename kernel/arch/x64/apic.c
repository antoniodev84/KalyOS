#include "kaly.h"
static volatile uint32_t *lapic;
static uint32_t timer_count;
static void lapic_write(unsigned r, uint32_t v) {
    lapic[r / 4] = v;
    (void)lapic[0x20 / 4];
}
void apic_init(void) {
    uint64_t base = rdmsr(0x1b);
    wrmsr(0x1b, (base | 0x800) & ~(1UL << 10));
    lapic = phys(base & 0xfffff000);
    outb(0x21, 255);
    outb(0xa1, 255);
    lapic_write(0xf0, 0x1ff);
    lapic_write(0x80, 0);
    lapic_write(0x350, 1 << 16);
    lapic_write(0x360, 1 << 16);
    lapic_write(0x370, 1 << 16);
    lapic_write(0x280, 0);
    lapic_write(0xb0, 0);
}
void apic_eoi(void) {
    lapic_write(0xb0, 0);
}
static void pit_wait(uint16_t count) {
    uint8_t port = inb(0x61);
    outb(0x61, (port & ~2) | 1);
    outb(0x43, 0xb0);
    outb(0x42, count);
    outb(0x42, count >> 8);
    while (!(inb(0x61) & 32)) {
        __asm__ volatile("pause");
    }
    outb(0x61, port);
}
void apic_timer(void) {
    lapic_write(0x3e0, 3);
    if (!this_cpu()->index) {
        lapic_write(0x320, (1 << 16) | 32);
        lapic_write(0x380, 0xffffffff);
        pit_wait(11932);
        timer_count = 0xffffffff - lapic[0x390 / 4];
        if (!timer_count) {
            panic("apic timer");
        }
    }
    lapic_write(0x320, (1 << 17) | 32);
    lapic_write(0x380, timer_count);
}
void apic_start(uint32_t id) {
    lapic_write(0x310, id << 24);
    lapic_write(0x300, 0xc500);
    while (lapic[0x300 / 4] & (1 << 12)) {
        __asm__ volatile("pause");
    }
    pit_wait(11932);
    lapic_write(0x310, id << 24);
    lapic_write(0x300, 0x8500);
    while (lapic[0x300 / 4] & (1 << 12)) {
        __asm__ volatile("pause");
    }
    for (unsigned i = 0; i < 2; i++) {
        lapic_write(0x310, id << 24);
        lapic_write(0x300, 0x608);
        while (lapic[0x300 / 4] & (1 << 12)) {
            __asm__ volatile("pause");
        }
        pit_wait(240);
    }
}
uint32_t apic_id(void) {
    return lapic[0x20 / 4] >> 24;
}
