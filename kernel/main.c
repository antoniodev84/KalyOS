#include "kaly.h"
#include "usb.h"
void kernel_main(uint64_t info, uint64_t magic) {
    console_init();
    if (magic != 0x36d76289) {
        panic("multiboot2");
    }
    kprintf("KalyOS x86_64\n");
    pmm_init(info);
    hw_init();
    framebuffer_init(info);
    acpi_init(info);
    touchpad_init();
    arch_init(&cpus[0]);
    apic_init();
    sched_init();
    rtc_init();
    pci_init();
    usb_init();
    block_scan_partitions();
    vfs_init(info);
    wm_init();
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
