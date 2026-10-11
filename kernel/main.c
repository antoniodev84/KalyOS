#include "kaly.h"
void kernel_main(uint64_t info, uint64_t magic) {
    console_init();
    if (magic != 0x36d76289) {
        panic("multiboot2");
    }
    kprintf("KalyOS x86_64\n");
    pmm_init(info);
    arch_init(&cpus[0]);
    apic_init();
    sched_init();
    rtc_init();
    pci_init();
    vfs_init(info);
    int pid = process_spawn("/init");
    if (pid < 0) {
        kprintf("init error %u\n", (uint64_t)-pid);
        panic("init");
    }
    apic_timer();
    smp_init(info);
    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}
