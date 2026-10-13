# KalyOS

A monolithic x86_64 Unix-like kernel written from scratch in C and NASM.

Educational hobby kernel: boots via Multiboot2 into long mode, brings up
SMP, runs a preemptive scheduler, loads ELF64 binaries (including PIE with
a custom dynamic linker), and ships a small userspace with its own libc
and shell.

## Features

- Multiboot2 boot into long mode; bitmap PMM; 4-level VMM; `kalloc`/`kfree`
- Preemptive SMP scheduler; APIC timer; AP bring-up via ICR
- VFS with RAMFS, cpio initramfs, and ext2 read/write
- MBR (primary only) and GPT (CRC-validated) partition tables
- ELF64 loader (static + PIE) and custom dynamic linker (`ld-kaly.so`)
- Drivers: AHCI, UHCI/EHCI/xHCI, USB HID, USB Mass Storage, Intel HDA
  (playback), RTL8168/8169, Intel e1000/e1000e, PS/2, touchpad,
  framebuffer console, PCI, ACPI (S5 shutdown), CMOS RTC
- Networking: IPv4, ARP, ICMP, UDP, TCP (client), DNS in userspace
- Custom libc, shell with pipes/redirection, `init`, `kalybox` (~20 utils)

## Build

```sh
make          # build ISO and disk image
make run      # boot in QEMU (serial stdio)
make clean
```

## Status

Educational kernel, under active development. Tested under QEMU only.

Not yet: signals, COW, demand paging, swap, page cache, DHCP,
TCP server, FAT/exFAT, reboot.

## License

MIT License
