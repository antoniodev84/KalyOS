BITS 64
SECTION .text
GLOBAL _dl_entry
EXTERN dl_start
_dl_entry:
    mov rdi, rsp
    mov r12, rsp
    and rsp, -16
    call dl_start
    mov rsp, r12
    xor edx, edx
    jmp rax
SECTION .note.GNU-stack noalloc noexec nowrite progbits
