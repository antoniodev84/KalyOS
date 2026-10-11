BITS 64
SECTION .text
EXTERN interrupt_dispatch
EXTERN syscall_dispatch
EXTERN schedule_finish
GLOBAL frame_enter
GLOBAL syscall_entry
GLOBAL isr_table
%macro PUSH_REGS 0
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro
%assign i 0
%rep 256
isr_%+i:
%if i != 8 && i != 10 && i != 11 && i != 12 && i != 13 && i != 14 && i != 17 && i != 21 && i != 29 && i != 30
    push qword 0
%endif
    push qword i
    jmp interrupt_common
%assign i i+1
%endrep
interrupt_common:
    test byte [rsp + 24], 3
    jz .kernel
    swapgs
.kernel:
    PUSH_REGS
    cld
    mov rdi, rsp
    and rsp, -16
    call interrupt_dispatch
    mov rsp, rax
    jmp prepare_frame
syscall_entry:
    swapgs
    mov [gs:8], rsp
    mov rsp, [gs:0]
    push qword 27
    push qword [gs:8]
    push r11
    push qword 35
    push rcx
    push qword 0
    push qword 256
    PUSH_REGS
    cld
    mov rdi, rsp
    and rsp, -16
    call syscall_dispatch
    mov rsp, rax
    jmp prepare_frame
frame_enter:
    mov rsp, rdi
prepare_frame:
    mov rbx, rsp
    mov rdi, rbx
    and rsp, -16
    call schedule_finish
    mov rsp, rax
    cmp rax, rbx
    jne prepare_frame
return_frame:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    test byte [rsp + 8], 3
    jz .kernel
    swapgs
.kernel:
    iretq
SECTION .rodata
ALIGN 8
isr_table:
%assign i 0
%rep 256
    dq isr_%+i
%assign i i+1
%endrep
SECTION .note.GNU-stack noalloc noexec nowrite progbits
