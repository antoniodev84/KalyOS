BITS 64
SECTION .text
GLOBAL _start
EXTERN libc_start
_start:
    xor ebp, ebp
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    lea rdx, [rsi + rdi * 8 + 8]
    and rsp, -16
    call libc_start wrt ..plt
SECTION .note.GNU-stack noalloc noexec nowrite progbits
