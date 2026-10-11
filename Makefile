SHELL := /bin/bash
LOCAL_TOOLS := $(abspath ../.tools/root)
ifneq ($(wildcard $(LOCAL_TOOLS)/usr/bin/nasm),)
export PATH := $(LOCAL_TOOLS)/usr/bin:$(LOCAL_TOOLS)/usr/sbin:$(PATH)
export LD_LIBRARY_PATH := $(LOCAL_TOOLS)/usr/lib/x86_64-linux-gnu:$(LD_LIBRARY_PATH)
GRUB_DIR := $(LOCAL_TOOLS)/usr/lib/grub/i386-pc
GRUB_IMAGE := $(LOCAL_TOOLS)/usr/bin/grub-mkimage
QEMU_DATA := $(LOCAL_TOOLS)/usr/share/qemu
QEMU_BIOS := $(LOCAL_TOOLS)/usr/share/seabios/bios.bin
export MKE2FS_CONFIG := $(LOCAL_TOOLS)/etc/mke2fs.conf
else
GRUB_DIR := /usr/lib/grub/i386-pc
GRUB_IMAGE := grub-mkimage
QEMU_DATA := /usr/share/qemu
QEMU_BIOS := /usr/share/seabios/bios.bin
endif
CROSS ?=
CC := clang -target x86_64-elf
LD := ld.lld
AR := llvm-ar
OBJCOPY := llvm-objcopy
NASM := nasm
CFLAGS := -m64 -std=gnu11 -O2 -g -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -mgeneral-regs-only -Wall -Wextra -Werror
KCFLAGS := $(CFLAGS) -fno-pie -Ikernel/include
UCFLAGS := $(filter-out -mgeneral-regs-only,$(CFLAGS)) -fPIC -Iuser/libc/include
KERNEL_C := $(shell find kernel -name '*.c' | sort)
KERNEL_ASM := $(shell find kernel -name '*.asm' | sort) boot/entry.asm
KERNEL_OBJECTS := $(KERNEL_C:%.c=build/%.o) $(KERNEL_ASM:%.asm=build/%.o) build/boot/ap_blob.o
LIB_C := $(filter-out user/libc/src/start.c,$(shell find user/libc/src -name '*.c' | sort))
LIB_OBJECTS := $(LIB_C:%.c=build/%.o)
PROGRAMS := init sh kalybox
COMMANDS := echo cat ls pwd mkdir rm rmdir ln readlink chmod mount umount stat cp mv touch sleep ps uname id busy sparse hexdump as fault mem
.PHONY: all iso disk run clean
all: build/KalyOS.iso build/disk.img
iso: build/KalyOS.iso
disk: build/disk.img
build/kernel/%.o: kernel/%.c kernel/include/kaly.h
	@mkdir -p $(@D)
	$(CC) $(KCFLAGS) -c $< -o $@
build/%.o: %.asm
	@mkdir -p $(@D)
	$(NASM) -f elf64 $< -o $@
build/boot/ap.bin: boot/ap.asm
	@mkdir -p $(@D)
	$(NASM) -f bin $< -o $@
build/boot/ap_blob.o: build/boot/ap.bin
	cd build/boot && $(LD) -m elf_x86_64 -r -b binary ap.bin -o ap_blob.raw.o
	$(OBJCOPY) --rename-section .data=.rodata,alloc,load,readonly,data,contents --redefine-sym _binary_ap_bin_start=ap_blob_start --redefine-sym _binary_ap_bin_end=ap_blob_end build/boot/ap_blob.raw.o $@
build/kernel.elf: $(KERNEL_OBJECTS) boot/linker.ld
	$(LD) -m elf_x86_64 -nostdlib -z noexecstack -T boot/linker.ld -o $@ $(KERNEL_OBJECTS)
	grub-file --is-x86-multiboot2 $@
build/user/%.o: user/%.c $(shell find user/libc/include -type f)
	@mkdir -p $(@D)
	$(CC) $(UCFLAGS) -c $< -o $@
build/libc.a: $(LIB_OBJECTS) build/user/libc/src/start.o
	$(AR) rcs $@ $^
build/libc.so: $(LIB_OBJECTS) build/user/libc/src/start.o
	$(LD) -m elf_x86_64 -shared --hash-style=both -soname libc.so -z noexecstack -o $@ $^
build/user/init: build/user/programs/init.o build/user/libc/src/crt0.o build/libc.a user/linker.ld
	$(LD) -m elf_x86_64 -static -z noexecstack -T user/linker.ld -o $@ $(filter %.o %.a,$^)
build/user/sh: build/user/programs/sh.o build/user/libc/src/crt0.o build/libc.a user/linker.ld
	$(LD) -m elf_x86_64 -static -z noexecstack -T user/linker.ld -o $@ $(filter %.o %.a,$^)
build/user/kalybox: build/user/programs/kalybox.o build/user/libc/src/crt0.o build/libc.a user/linker.ld
	$(LD) -m elf_x86_64 -static -z noexecstack -T user/linker.ld -o $@ $(filter %.o %.a,$^)
build/user/ld-kaly.so: build/user/programs/ld.o build/user/programs/ld_entry.o build/libc.a
	$(LD) -m elf_x86_64 -shared -Bsymbolic --hash-style=both -z noexecstack -e _dl_entry -o $@ $^
build/user/hello: build/user/programs/hello.o build/user/libc/src/crt0.o build/libc.so
	$(LD) -m elf_x86_64 -pie --hash-style=both -z noexecstack --dynamic-linker /lib/ld-kaly.so -o $@ $(filter %.o,$^) -Lbuild -lc
build/initramfs.cpio: $(addprefix build/user/,$(PROGRAMS)) build/user/hello build/user/ld-kaly.so build/libc.a build/libc.so tools/initramfs.py
	mkdir -p build/root/{bin,lib,dev,tmp,proc,sys,mnt,home}
	chmod 755 build/root build/root/{bin,lib,dev,proc,sys,mnt,home}
	chmod 1777 build/root/tmp
	cp build/user/init build/root/init
	cp build/user/sh build/user/kalybox build/user/hello build/root/bin/
	cp build/libc.a build/libc.so build/user/ld-kaly.so build/root/lib/
	chmod 755 build/root/init build/root/bin/{sh,kalybox,hello} build/root/lib/ld-kaly.so
	chmod 644 build/root/lib/{libc.a,libc.so}
	for cmd in $(COMMANDS); do ln -sf kalybox build/root/bin/$$cmd; done
	python3 tools/initramfs.py build/root $@
build/KalyOS.iso: build/kernel.elf build/initramfs.cpio boot/grub.cfg
	mkdir -p build/iso/boot/grub
	cp build/kernel.elf build/initramfs.cpio build/iso/boot/
	cp boot/grub.cfg build/iso/boot/grub/
	grub-mkrescue -d $(GRUB_DIR) --grub-mkimage=$(GRUB_IMAGE) -o $@ build/iso
build/disk.img:
	mkdir -p build
	truncate -s 256M $@
	mke2fs -t ext2 -b 4096 -I 128 -i 1048576 -O none,filetype,sparse_super,large_file -F $@
run: all
	qemu-system-x86_64 -L $(QEMU_DATA) -bios $(QEMU_BIOS) -vga none -device VGA,romfile=$(dir $(QEMU_BIOS))vgabios-stdvga.bin -nic none -machine q35 -cpu max -m 512M -smp 4 -cdrom build/KalyOS.iso -drive file=build/disk.img,format=raw,if=none,id=disk -device ide-hd,drive=disk,bus=ide.0 -display none -serial stdio -monitor none -no-reboot
clean:
	rm -rf build
