# KalyOS

A monolithic x86_64 Unix-like kernel written from scratch in C and NASM.

## Features

- Multiboot2 boot into long mode, bitmap PMM, 4-level VMM, `kalloc`/`kfree`
- Preemptive SMP scheduler with fixed process table, APIC timer, AP bring-up via ICR
- `fork`, `execve`, `exit`, `wait`, `mmap`, `mprotect`, `brk`, `pipe`, `nanosleep`
- VFS with RAMFS, cpio initramfs, and a read/write ext2 driver (bitmaps, groups, triple indirect)
- ELF64 loader (static and PIE) plus a custom dynamic linker (`ld-kaly.so`) with relocations
- Drivers: AHCI, PCI enumeration, PS/2 keyboard, VGA + serial console, CMOS RTC
- Custom libc (stdio, stdlib, dirent, string) and a shell with pipes, redirection, `$?`, `$VAR`
- ~20 userland utilities via `kalybox`

## Build

```

make          # build ISO and disk image
make run      # boot in QEMU (serial stdio)
make clean

```

## Status

Educational kernel. No networking, signals, COW, demand paging, swap, or page cache yet.

## License

MIT License - see [LICENSE](LICENSE).
