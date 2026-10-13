#include "net.h"
#include "usb.h"
struct idt_gate {
    uint16_t low, selector;
    uint8_t ist, type;
    uint16_t middle;
    uint32_t high, zero;
} PACKED;
struct descriptor {
    uint16_t limit;
    uint64_t base;
} PACKED;
static struct idt_gate idt[256];
extern void *isr_table[256];
extern void syscall_entry(void);
void syscall_init(void) {
    wrmsr(0xc0000080, rdmsr(0xc0000080) | 1);
    wrmsr(0xc0000081, (16UL << 48) | (8UL << 32));
    wrmsr(0xc0000082, (uint64_t)syscall_entry);
    wrmsr(0xc0000084, 0x44700);
}
void arch_init(struct cpu *c) {
    c->self = c;
    fpu_reset(c->fpu);
    __asm__ volatile("fxrstor64 %0" ::"m"(c->fpu));
    c->gdt[0] = 0;
    c->gdt[1] = 0x00af9a000000ffff;
    c->gdt[2] = 0x00cf92000000ffff;
    c->gdt[3] = 0x00cff2000000ffff;
    c->gdt[4] = 0x00affa000000ffff;
    uint64_t base = (uint64_t)&c->tss;
    c->gdt[5] = (sizeof(c->tss) - 1) | ((base & 0xffffff) << 16) | (0x89UL << 40) |
                ((base & 0xff000000) << 32);
    c->gdt[6] = base >> 32;
    c->tss.iomap = sizeof(c->tss);
    uint64_t emergency = pmm_alloc_contiguous(4);
    if (!emergency) {
        panic("ist memory");
    }
    c->tss.ist[0] = (uint64_t)phys(emergency) + 4 * PAGE;
    struct descriptor gdtr = {sizeof(c->gdt) - 1, (uint64_t)c->gdt};
    __asm__ volatile("lgdt %0" ::"m"(gdtr));
    __asm__ volatile("pushq $8; leaq 1f(%%rip),%%rax; pushq %%rax; lretq; 1:" ::: "rax", "memory");
    __asm__ volatile(
        "mov $16,%%ax; mov %%ax,%%ds; mov %%ax,%%es; mov %%ax,%%ss; mov $40,%%ax; ltr %%ax" ::
            : "rax", "memory");
    if (!c->index) {
        for (unsigned i = 0; i < 256; i++) {
            uint64_t a = (uint64_t)isr_table[i];
            idt[i] = (struct idt_gate){a, 8, i == 8 ? 1 : 0, 0x8e, a >> 16, a >> 32, 0};
        }
    }
    struct descriptor idtr = {sizeof(idt) - 1, (uint64_t)idt};
    __asm__ volatile("lidt %0" ::"m"(idtr));
    wrmsr(0xc0000101, (uint64_t)c);
    wrmsr(0xc0000102, 0);
    syscall_init();
}
struct frame *interrupt_dispatch(struct frame *f) {
    struct cpu *c = this_cpu();
    if (f->vector == 32) {
        process_account();
        c->ticks++;
        if (!c->index) {
            hda_poll();
            usb_poll();
            wm_poll();
            net_poll();
            process_tick();
        }
        apic_eoi();
        return schedule(f);
    }
    if (f->vector == 255) {
        return f;
    }
    if (f->vector < 32) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        kprintf("fault %u cpu %u rip %x addr %x error %x\n", f->vector, (uint64_t)c->index, f->rip,
                cr2, f->error);
        if ((f->cs & 3) && c->current) {
            return process_exit(f, 128 + f->vector);
        }
        panic("exception");
    }
    apic_eoi();
    return f;
}
