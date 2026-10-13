
# KalyOS

A monolithic x86_64 Unix-like kernel written from scratch in C and NASM.

## Features

### Boot and architecture
- Multiboot2 entry, 32-bit stub configuring PAE, long mode and 4-level paging
- Bitmap physical memory manager with contiguous allocation
- 4-level virtual memory manager with copy-on-write-less `vm_clone`, `vm_map`, `vm_protect`, `vm_unmap`
- `kalloc` / `kfree` for kernel heap
- Preemptive SMP scheduler with fixed process table, APIC timer and AP bring-up via ICR
- Per-CPU TSS, GDT, IST for double fault, FPU state save/restore via `fxsave64`/`fxrstor64`

### Processes and syscalls
- `fork`, `execve`, `exit`, `wait`, `mmap`, `mprotect`, `munmap`, `brk`, `pipe`, `nanosleep`
- File syscalls: `open`, `close`, `read`, `write`, `lseek`, `stat`, `fstat`, `lstat`, `dup2`, `fcntl`, `getdents`
- Filesystem syscalls: `mkdir`, `rmdir`, `unlink`, `link`, `symlink`, `readlink`, `chmod`, `mount`, `umount`
- Process/misc: `getpid`, `getuid`, `setuid`, `uname`, `getcwd`, `chdir`, `reboot`
- ELF64 loader (static, PIE and dynamic) plus a custom dynamic linker (`ld-kaly.so`) with SysV and GNU hash, `R_X86_64_RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `COPY`

### Filesystems
- VFS with reference-counted nodes, symlink resolution, mount points, permissions
- RAMFS with sparse page-backed storage and procfs-style generated files (`/proc/meminfo`, `/proc/cpuinfo`, `/proc/uptime`, `/proc/processes`, `/proc/net`, `/proc/sound`)
- cpio initramfs parsing
- Read/write ext2 driver with block groups, bitmaps, triple indirect, directory entries, hard and symbolic links, truncate, sync
- MBR and GPT partition scanning, with protective MBR detection
- Block device abstraction shared between AHCI, USB mass storage and partitions

### Drivers
- AHCI SATA with port setup, IDENTIFY, 48-bit LBA, read/write and cache flush
- PCI enumeration routing devices to their drivers by class
- UHCI, EHCI and xHCI USB host controllers, hub enumeration (multi-TT), USB mass storage (bulk-only transport, SCSI READ/WRITE/INQUIRY/REQUEST SENSE), USB HID keyboards and pointers (boot protocol and parsed report descriptors)
- PS/2 keyboard with modifiers, extended scancodes, arrow escape sequences
- Synaptics/PS/2 touchpad with absolute and relative modes, `/dev/event0` evdev-style input
- Intel HDA audio with CORB/RIRB, widget graph discovery, output routing, `/dev/dsp`
- e1000/e1000e and RTL8168/8169 NICs
- Framebuffer from Multiboot2 with PSF1/PSF2 fonts and Unicode mapping
- VGA text + serial console, CMOS RTC

### Networking
- IPv4 with ARP cache, ICMP echo, UDP, TCP with a small state machine (SYN_SENT, ESTABLISHED, FIN_WAIT, CLOSE_WAIT, TIME_WAIT)
- Sockets API: `socket`, `connect`, `bind`, `send`, `recv`, `sendto`, `recvfrom`, `shutdown`, `getsockopt`, `getsockname`, `getpeername`
- `ioctl` for interface configuration (`SIOCSIFADDR`, `SIOCSIFNETMASK`, `SIOCSIFFLAGS`, `SIOCADDRT`, `SIOCGIFHWADDR`)
- Loopback of packets through `/proc/net`, raw ICMP sockets for `ping`, TCP client for `wget`, UDP for DNS

### Graphics and window manager
- Kernel-space compositor (`/dev/wm`) with damage tracking, window stacking, focus, drag, resize, maximize, minimize, dock and start menu
- Clipboard, event queue per window, key/mouse/wheel/size/close/activate events
- Central theme header (`shared/include/kaly/theme.h`) driving all UI colors
- Userspace GUI library (`libgui`) with fonts, rectangles, lines, buttons, blitting and event decoding

### Userland
- Custom libc: `stdio`, `stdlib`, `string`, `dirent`, `unistd`, `fcntl`, `socket`, `mman`, `time`, plus syscall wrappers
- `init` (session manager), `sh` (shell with pipes, redirection, `$?`, `$VAR`, background jobs), `desktop` (start menu + app launcher), `kalybox` (multi-call utility)
- Programs: `terminal`, `files`, `notes`, `hello`
- Commands: `echo`, `cat`, `ls`, `pwd`, `mkdir`, `rm`, `rmdir`, `ln`, `readlink`, `chmod`, `mount`, `umount`, `stat`, `cp`, `mv`, `touch`, `sleep`, `ps`, `uname`, `id`, `busy`, `sparse`, `hexdump`, `as`, `fault`, `mem`, `ifconfig`, `ping`, `wget`, `netstat`, `udp`, `nslookup`, `pendrive`, `sound`, `shutdown`, `window`, `init`

## Build

```sh
make            # build ISO and disk image
make run        # boot in QEMU (serial stdio, no display)
make run-graphic # boot in QEMU with GTK display and PulseAudio
make clean
```

Requires clang targeting x86_64-elf, ld.lld, nasm, grub-mkrescue, qemu-system-x86_64 and Python 3 for the initramfs tool.

## Layout

```
boot/          Multiboot2 entry, long mode stub, AP trampoline, linker script
kernel/
  arch/x64/    GDT/TSS/IDT, APIC, SMP, interrupts, syscall dispatch
  drivers/     console, keyboard, touchpad, framebuffer, PCI, RTC, ACPI, AHCI, HDA, USB, NIC
  fs/          VFS, RAMFS, ext2, initramfs
  gui/         compositor and 2D graphics primitives
  mm/          physical and virtual memory managers
  net/         IPv4/ARP/ICMP/UDP/TCP stack and sockets
  sched/       process table, scheduler, fork/exec/wait
  elf/         ELF64 loader
user/
  libc/        C library and syscall wrappers
  programs/    init, sh, desktop, terminal, files, notes, kalybox and utilities
shared/include/kaly/  WM ABI, system ABI and theme palette
```

## Status

Educational kernel. No signals, no copy-on-write, no demand paging, no swap, no page cache, no users/groups beyond uid/gid fields. Networking is single-interface focused and TCP does not implement congestion control beyond a fixed MSS and retransmission timer. The window manager is single-session and intended for a single graphical user.

## License

MIT License - see LICENSE.
