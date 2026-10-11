BITS 32
SECTION .multiboot
ALIGN 8
header:
    dd 0xe85250d6
    dd 0
    dd header_end - header
    dd -(0xe85250d6 + (header_end - header))
    dw 0, 0
    dd 8
header_end:
SECTION .text
GLOBAL _start
EXTERN kernel_main
_start:
    cli
    mov esp, boot_stack_top
    mov edi, ebx
    mov esi, eax
    mov eax, boot_pdpt
    or eax, 3
    mov [boot_pml4], eax
    mov [boot_pml4 + 256 * 8], eax
    mov eax, boot_pd
    or eax, 3
    mov [boot_pdpt], eax
    add eax, 4096
    mov [boot_pdpt + 8], eax
    add eax, 4096
    mov [boot_pdpt + 16], eax
    add eax, 4096
    mov [boot_pdpt + 24], eax
    xor ecx, ecx
.map:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83
    mov [boot_pd + ecx * 8], eax
    inc ecx
    cmp ecx, 2048
    jne .map
    mov eax, cr4
    or eax, 0x620
    mov cr4, eax
    mov eax, boot_pml4
    mov cr3, eax
    mov ecx, 0xc0000080
    rdmsr
    or eax, 0x900
    wrmsr
    mov eax, cr0
    and eax, 0xfffffffb
    or eax, 0x80010002
    mov cr0, eax
    lgdt [boot_gdtr]
    jmp 8:long_start
BITS 64
long_start:
    mov ax, 16
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, boot_stack_top
    xor rbp, rbp
    call kernel_main
.halt:
    cli
    hlt
    jmp .halt
SECTION .rodata
ALIGN 8
boot_gdt:
    dq 0
    dq 0x00af9a000000ffff
    dq 0x00cf92000000ffff
boot_gdtr:
    dw 23
    dq boot_gdt
SECTION .bss
ALIGN 4096
GLOBAL boot_pml4
boot_pml4: resb 4096
boot_pdpt: resb 4096
boot_pd: resb 16384
ALIGN 16
boot_stack: resb 65536
boot_stack_top:
SECTION .note.GNU-stack noalloc noexec nowrite progbits
