BITS 64
SECTION .rodata
GLOBAL console_font
GLOBAL console_font_end
console_font:
    incbin "kernel/assets/console.psf"
console_font_end:
SECTION .note.GNU-stack noalloc noexec nowrite progbits
