ENTRY(_start)
PHDRS {
    headers PT_PHDR PHDRS;
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x40000000 + SIZEOF_HEADERS;
    .text : { *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    . = ALIGN(4096);
    .data : { *(.data*) } :data
    .bss : { *(COMMON) *(.bss*) } :data
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment*) }
}
