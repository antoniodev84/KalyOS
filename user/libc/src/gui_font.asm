section .rodata
align 16
global gui_font
global gui_font_end
gui_font:
incbin "kernel/assets/console.psf"
gui_font_end:
section .note.GNU-stack noalloc noexec nowrite progbits
