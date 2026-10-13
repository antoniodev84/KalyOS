BITS 64
SECTION .text
GLOBAL setjmp
GLOBAL longjmp
setjmp:
    mov [rdi], rbx
    mov [rdi+8], rbp
    mov [rdi+16], r12
    mov [rdi+24], r13
    mov [rdi+32], r14
    mov [rdi+40], r15
    lea rax, [rsp+8]
    mov [rdi+48], rax
    mov rax, [rsp]
    mov [rdi+56], rax
    xor eax, eax
    ret
longjmp:
    mov eax, esi
    test eax, eax
    jnz .value
    inc eax
.value:
    mov rbx, [rdi]
    mov rbp, [rdi+8]
    mov r12, [rdi+16]
    mov r13, [rdi+24]
    mov r14, [rdi+32]
    mov r15, [rdi+40]
    mov rsp, [rdi+48]
    jmp [rdi+56]
SECTION .note.GNU-stack noalloc noexec nowrite progbits
