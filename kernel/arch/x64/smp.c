#include "kaly.h"
struct cpu cpus[MAX_CPU];
unsigned cpu_count = 1;
extern void apic_start(uint32_t);
extern uint32_t apic_id(void);
extern uint8_t ap_blob_start[], ap_blob_end[];
struct acpi_header {
    char signature[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem[6], table[8];
    uint32_t oem_revision, creator, creator_revision;
} PACKED;
static bool checksum(const void *data, size_t n) {
    const uint8_t *p = data;
    uint8_t sum = 0;
    while (n--) {
        sum += *p++;
    }
    return !sum;
}
static struct acpi_header *madt_find(uint64_t info) {
    uint32_t total = *(uint32_t *)info;
    uint8_t *rsdp = NULL;
    for (uint64_t t = info + 8; t < info + total; t += ALIGN_UP(*(uint32_t *)(t + 4), 8)) {
        if (!*(uint32_t *)t) {
            break;
        }
        if (*(uint32_t *)t == 14 || *(uint32_t *)t == 15) {
            rsdp = (uint8_t *)t + 8;
        }
    }
    if (!rsdp || !checksum(rsdp, 20)) {
        return NULL;
    }
    bool extended = rsdp[15] >= 2 && checksum(rsdp, 36) && *(uint64_t *)(rsdp + 24);
    uint64_t address = extended ? *(uint64_t *)(rsdp + 24) : *(uint32_t *)(rsdp + 16);
    if (address >= 0x100000000UL) {
        return NULL;
    }
    struct acpi_header *table = phys(address);
    if (table->length < sizeof(*table) || !checksum(table, table->length)) {
        return NULL;
    }
    size_t stride = extended ? 8 : 4;
    for (size_t off = sizeof(*table); off + stride <= table->length; off += stride) {
        uint64_t p = extended ? *(uint64_t *)((uint8_t *)table + off)
                              : *(uint32_t *)((uint8_t *)table + off);
        if (p >= 0x100000000UL) {
            continue;
        }
        struct acpi_header *h = phys(p);
        if (!memcmp(h->signature, "APIC", 4) && h->length >= 44 && checksum(h, h->length)) {
            return h;
        }
    }
    return NULL;
}
void smp_init(uint64_t info) {
    cpus[0].apic_id = apic_id();
    cpus[0].online = 1;
    struct acpi_header *madt = madt_find(info);
    if (madt) {
        for (size_t off = 44; off + 2 <= madt->length;) {
            uint8_t *e = (uint8_t *)madt + off;
            if (e[1] < 2 || off + e[1] > madt->length) {
                break;
            }
            if (e[0] == 0 && e[1] >= 8 && (*(uint32_t *)(e + 4) & 1) && e[3] != cpus[0].apic_id &&
                cpu_count < MAX_CPU) {
                cpus[cpu_count].index = cpu_count;
                cpus[cpu_count++].apic_id = e[3];
            }
            off += e[1];
        }
    }
    memcpy((void *)0x8000, ap_blob_start, ap_blob_end - ap_blob_start);
    for (unsigned i = 1; i < cpu_count; i++) {
        uint64_t stack = pmm_alloc_contiguous(8);
        if (!stack) {
            panic("ap stack");
        }
        *(uint64_t *)0x8f00 = physical(boot_pml4);
        *(uint64_t *)0x8f08 = (uint64_t)phys(stack) + 8 * PAGE;
        *(uint64_t *)0x8f10 = i;
        *(uint64_t *)0x8f18 = (uint64_t)ap_main;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        apic_start(cpus[i].apic_id);
        uint64_t timeout = 100000000;
        while (!__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE) && --timeout) {
            __asm__ volatile("pause");
        }
        if (!timeout) {
            panic("ap startup");
        }
    }
    kprintf("SMP %u CPUs\n", (uint64_t)cpu_count);
}
_Noreturn void ap_main(uint64_t index) {
    struct cpu *c = &cpus[index];
    arch_init(c);
    apic_init();
    apic_timer();
    __atomic_store_n(&c->online, 1, __ATOMIC_RELEASE);
    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}
