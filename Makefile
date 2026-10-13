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
KCFLAGS := $(CFLAGS) -fno-pie -Ikernel/include -Ishared/include
UCFLAGS := $(filter-out -mgeneral-regs-only,$(CFLAGS)) -fPIC -Iuser/libc/include -Ishared/include
KERNEL_C := $(shell find kernel -name '*.c' | sort)
KERNEL_ASM := $(shell find kernel -name '*.asm' | sort) boot/entry.asm
KERNEL_OBJECTS := $(KERNEL_C:%.c=build/%.o) $(KERNEL_ASM:%.asm=build/%.o) build/boot/ap_blob.o
LIB_C := $(filter-out user/libc/src/start.c,$(shell find user/libc/src -name '*.c' | sort))
LIB_OBJECTS := $(LIB_C:%.c=build/%.o) build/user/libc/src/gui_font.o build/user/libc/src/setjmp.o
PROGRAMS := init sh kalybox desktop terminal files notes
COMMANDS := echo cat ls pwd mkdir rm rmdir ln readlink chmod mount umount stat cp mv touch sleep ps uname id busy sparse hexdump as fault mem ifconfig ping wget netstat udp nslookup pendrive sound shutdown window init
.SECONDARY: $(addprefix build/user/programs/,$(addsuffix .o,$(PROGRAMS)))
.PHONY: all iso disk run run-graphic clean
QEMU_DISPLAY ?= none
QEMU_AUDIO ?= none
all: build/KalyOS.iso build/disk.img
iso: build/KalyOS.iso
disk: build/disk.img
build/kernel/%.o: kernel/%.c $(wildcard kernel/include/*.h shared/include/kaly/*.h)
	@mkdir -p $(@D)
	$(CC) $(KCFLAGS) -c $< -o $@
build/kernel/assets/font.o build/user/libc/src/gui_font.o: kernel/assets/console.psf
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
build/user/%.o: user/%.c $(shell find user/libc/include shared/include -type f)
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
build/user/kalybox: build/user/programs/window_command.o build/user/programs/dns_command.o build/user/programs/sound_command.o build/user/programs/hardware_commands.o build/user/programs/kalybox.o build/user/libc/src/crt0.o build/libc.a user/linker.ld
	$(LD) -m elf_x86_64 -static -z noexecstack -T user/linker.ld -o $@ $(filter %.o %.a,$^)
build/user/%: build/user/programs/%.o build/user/libc/src/crt0.o build/libc.a user/linker.ld
	$(LD) -m elf_x86_64 -static -z noexecstack -T user/linker.ld -o $@ $(filter %.o %.a,$^)
build/user/ld-kaly.so: build/user/programs/ld.o build/user/programs/ld_entry.o build/libc.a
	$(LD) -m elf_x86_64 -shared -Bsymbolic --hash-style=both -z noexecstack -e _dl_entry -o $@ $^
build/user/hello: build/user/programs/hello.o build/user/libc/src/crt0.o build/libc.so
	$(LD) -m elf_x86_64 -pie --hash-style=both -z noexecstack --dynamic-linker /lib/ld-kaly.so -o $@ $(filter %.o,$^) -Lbuild -lc
build/initramfs.cpio: $(addprefix build/user/,$(PROGRAMS)) build/user/hello build/user/ld-kaly.so build/libc.a build/libc.so build/user/chibicc-bin build/user/kaly-as build/user/kaly-ld build/user/rotating-cube build/libkalygl.a build/libkalygfx.a tools/initramfs.py
	mkdir -p build/root/{bin,lib,dev,tmp,proc,sys,mnt,home}
	chmod 755 build/root build/root/{bin,lib,dev,proc,sys,mnt,home}
	chmod 1777 build/root/tmp
	cp build/user/init build/root/init
	cp $(addprefix build/user/,$(filter-out init,$(PROGRAMS))) build/user/hello build/root/bin/
	cp build/libc.a build/libc.so build/user/ld-kaly.so build/root/lib/
	cp build/user/libc/src/crt0.o build/root/lib/crt0.o
	cp build/user/chibicc-bin build/root/bin/chibicc
	cp build/user/kaly-as build/user/kaly-ld build/root/bin/
	mkdir -p build/root/usr/include/chibicc
	cp -r user/libc/include/. build/root/usr/include/
	cp -r shared/include/. build/root/usr/include/
	cp user/chibicc/include/*.h build/root/usr/include/chibicc/
	cp user/programs/hello_world.c build/root/home/hello.c
	cp user/programs/rotating_cube.c build/root/home/cube.c
	cp build/user/rotating-cube build/root/bin/
	cp build/libkalygl.a build/libkalygfx.a build/root/lib/
	cp -r user/kalygfx/include/. user/kalygl/include/. build/root/usr/include/
	chmod 755 build/root/init build/root/bin/* build/root/lib/ld-kaly.so
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
build/usb.img:
	mkdir -p build
	truncate -s 64M $@
	mke2fs -t ext2 -b 4096 -I 128 -O none,filetype,sparse_super,large_file -F $@
run: all build/usb.img
	qemu-system-x86_64 -L $(QEMU_DATA) -bios $(QEMU_BIOS) \
	    -vga none -device VGA,romfile=$(dir $(QEMU_BIOS))vgabios-stdvga.bin \
	    -machine q35 -cpu max -m 512M -smp 4 -cdrom build/KalyOS.iso \
	    -drive file=build/disk.img,format=raw,if=none,id=disk \
	    -device ide-hd,drive=disk,bus=ide.0 \
	    -netdev user,id=net0 -device e1000e,netdev=net0,romfile= \
	    -device qemu-xhci,id=usb -device usb-kbd,bus=usb.0 -device usb-tablet,bus=usb.0 \
	    -drive file=build/usb.img,format=raw,if=none,id=stick \
	    -device usb-storage,bus=usb.0,drive=stick \
	    -audiodev $(QEMU_AUDIO),id=audio -device intel-hda -device hda-output,audiodev=audio \
	    -display $(QEMU_DISPLAY) -serial stdio -monitor none -no-reboot
run-graphic:
	$(MAKE) run QEMU_DISPLAY=gtk QEMU_AUDIO=pa
clean:
	rm -rf build

CHIBICC_SRCS := $(wildcard user/chibicc/*.c)
CHIBICC_OBJECTS := $(CHIBICC_SRCS:%.c=build/%.o)
XCC_COMMON := user/xcc/src/util/util.c user/xcc/src/util/table.c user/xcc/src/util/bin_util.c
XCC_AS_SRCS := $(wildcard user/xcc/src/as/*.c user/xcc/src/as/arch/x64/*.c) $(XCC_COMMON)
XCC_LD_SRCS := $(wildcard user/xcc/src/ld/*.c) user/xcc/src/util/archive.c $(XCC_COMMON)
build/user/chibicc/%.o: user/chibicc/%.c $(wildcard user/chibicc/*.h)
	@mkdir -p $(@D)
	$(CC) $(filter-out -Werror,$(UCFLAGS)) -Wno-sign-compare -Wno-unused-parameter -c $< -o $@
build/user/xcc/%.o: user/xcc/%.c $(shell find user/xcc/src user/libc/include -name "*.h")
	@mkdir -p $(@D)
	$(CC) $(filter-out -Werror,$(UCFLAGS)) -Iuser/xcc/src/util -Iuser/xcc/src/as -Wno-unused-parameter -Wno-sign-compare -c $< -o $@
build/user/chibicc-bin: $(CHIBICC_OBJECTS) build/user/libc/src/crt0.o build/libc.a user/linker.ld
	$(LD) -m elf_x86_64 -static -z noexecstack -T user/linker.ld -o $@ $(filter %.o %.a,$^)
build/user/kaly-as: $(XCC_AS_SRCS:%.c=build/%.o) build/user/libc/src/crt0.o build/libc.a user/linker.ld
	$(LD) -m elf_x86_64 -static -z noexecstack -T user/linker.ld -o $@ $(filter %.o %.a,$^)
build/user/kaly-ld: $(XCC_LD_SRCS:%.c=build/%.o) build/user/libc/src/crt0.o build/libc.a user/linker.ld
	$(LD) -m elf_x86_64 -static -z noexecstack -T user/linker.ld -o $@ $(filter %.o %.a,$^)

TINYGL_SRCS := $(wildcard user/kalygl/vendor/tinygl/src/*.c)
KALYGL_SRCS := $(wildcard user/kalygl/src/*.c)
KALYGFX_SRCS := $(wildcard user/kalygfx/src/*.c)
GFX_HEADERS := $(shell find user/kalygl/include user/kalygfx/include user/kalygfx/src user/kalygl/vendor/tinygl/include -name "*.h") user/kalygl/vendor/tinygl/src/zgl.h
GFXFLAGS := $(filter-out -fPIC,$(UCFLAGS)) -fno-pic -fno-pie -Iuser/kalygfx/include -Iuser/kalygl/include
build/user/kalygl/vendor/tinygl/src/%.o: user/kalygl/vendor/tinygl/src/%.c $(GFX_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(filter-out -Werror,$(GFXFLAGS)) -Wno-unused-parameter -Wno-unused-function -Iuser/kalygl/vendor/tinygl/include -c $< -o $@
build/user/kalygl/src/%.o: user/kalygl/src/%.c $(GFX_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(GFXFLAGS) -Wno-unused-function -c $< -o $@
build/user/kalygfx/src/%.o: user/kalygfx/src/%.c $(GFX_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(GFXFLAGS) -c $< -o $@
build/libkalygl.a: $(TINYGL_SRCS:%.c=build/%.o) $(KALYGL_SRCS:%.c=build/%.o)
	$(AR) rcs $@ $^
build/libkalygfx.a: $(KALYGFX_SRCS:%.c=build/%.o)
	$(AR) rcs $@ $^

build/user/programs/rotating_cube.o: user/programs/rotating_cube.c user/kalygfx/include/KalyGFX.h user/kalygl/include/GL/gl.h
	@mkdir -p $(@D)
	$(CC) $(GFXFLAGS) -c $< -o $@
build/user/rotating-cube: build/user/programs/rotating_cube.o build/user/libc/src/crt0.o build/libkalygl.a build/libkalygfx.a build/libc.a user/linker.ld
	$(LD) -m elf_x86_64 -static -z noexecstack -T user/linker.ld -o $@ $(filter %.o %.a,$^)
