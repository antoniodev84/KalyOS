ENTRY(_start)
SECTIONS {
    . = 1M;
    kernel_start = .;
    .multiboot : { KEEP(*(.multiboot)) }
    .text ALIGN(4096) : { *(.text*) }
    .rodata ALIGN(4096) : { *(.rodata*) }
    .data ALIGN(4096) : { *(.data*) }
    .bss ALIGN(4096) : { *(COMMON) *(.bss*) }
    kernel_end = ALIGN(4096);
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment*) }
}
