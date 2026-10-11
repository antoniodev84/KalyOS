BITS 16
ORG 0x8000
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    lgdt [gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 8:protected
BITS 32
protected:
    mov ax, 16
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr4
    or eax, 0x620
    mov cr4, eax
    mov eax, [0x8f00]
    mov cr3, eax
    mov ecx, 0xc0000080
    rdmsr
    or eax, 0x900
    wrmsr
    mov eax, cr0
    and eax, 0xfffffffb
    or eax, 0x80010002
    mov cr0, eax
    jmp 24:long_mode
BITS 64
long_mode:
    mov ax, 16
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [0x8f08]
    mov rdi, [0x8f10]
    mov rax, [0x8f18]
    call rax
.halt:
    hlt
    jmp .halt
ALIGN 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
    dq 0x00af9a000000ffff
gdtr:
    dw 31
    dd gdt
