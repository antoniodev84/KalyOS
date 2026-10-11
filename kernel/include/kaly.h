#ifndef KALY_H
#define KALY_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#define PAGE 4096UL
#define DIRECT 0xffff800000000000UL
#define USER_MIN 0x40000000UL
#define USER_TOP 0x8000000000UL
#define MAX_CPU 16
#define MAX_PROC 128
#define MAX_FD 64
#define MAX_PATH 512
#define MAX_FILE (8UL * 1024 * 1024 * 1024)
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PTE_P 1UL
#define PTE_W 2UL
#define PTE_U 4UL
#define PTE_NX (1UL << 63)
#define S_IFIFO 0010000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFMT 0170000
#define EINVAL 22
#define ENOMEM 12
#define ENOENT 2
#define EIO 5
#define EACCES 13
#define EBADF 9
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define ENOSPC 28
#define ENOSYS 38
#define EFAULT 14
#define EBUSY 16
#define ENOTEMPTY 39
#define ELOOP 40
#define ENOEXEC 8
#define ECHILD 10
#define EAGAIN 11
#define EPERM 1
#define EFBIG 27
#define EXDEV 18
#define O_CREAT 64
#define O_TRUNC 512
#define O_APPEND 1024
#define O_DIRECTORY 65536
#define O_NOFOLLOW 131072
#define O_CLOEXEC 524288
#define MAP_FIXED 16
#define MAP_ANONYMOUS 32
#define PACKED __attribute__((packed))
struct lock {
    volatile uint32_t value;
};
static inline void lock(struct lock *l) {
    while (__atomic_exchange_n(&l->value, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->value, __ATOMIC_RELAXED)) {
            __asm__ volatile("pause");
        }
    }
}
static inline void unlock(struct lock *l) {
    __atomic_store_n(&l->value, 0, __ATOMIC_RELEASE);
}
static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void outl(uint16_t p, uint32_t v) {
    __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint32_t inl(uint16_t p) {
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint64_t rdmsr(uint32_t r) {
    uint32_t a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(r));
    return ((uint64_t)d << 32) | a;
}
static inline void wrmsr(uint32_t r, uint64_t v) {
    __asm__ volatile("wrmsr" ::"c"(r), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline void *phys(uint64_t p) {
    return (void *)(p + DIRECT);
}
static inline uint64_t physical(const void *p) {
    uint64_t v = (uint64_t)p;
    return v >= DIRECT ? v - DIRECT : v;
}
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *);
char *strchr(const char *, int);
uint64_t strtou(const char *, unsigned);
void console_init(void);
void console_putc(char);
void console_write(const char *, size_t);
int console_read(char *, size_t);
void kprintf(const char *, ...);
_Noreturn void panic(const char *);
void keyboard_poll(void);
void rtc_init(void);
uint64_t kernel_time(void);
void pmm_init(uint64_t);
uint64_t pmm_alloc(void);
uint64_t pmm_alloc_contiguous(size_t);
void pmm_free(uint64_t);
uint64_t pmm_available(void);
void *kalloc(size_t);
void kfree(void *);
uint64_t vm_create(void);
int vm_map(uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t vm_lookup(uint64_t, uint64_t, uint64_t *);
int vm_alloc(uint64_t, uint64_t, size_t, uint64_t);
void vm_unmap(uint64_t, uint64_t, size_t);
int vm_protect(uint64_t, uint64_t, size_t, uint64_t);
void vm_destroy(uint64_t);
uint64_t vm_clone(uint64_t);
int user_range(uint64_t, size_t, bool);
int copy_from_user(void *, uint64_t, size_t);
int copy_to_user(uint64_t, const void *, size_t);
int user_string(char *, uint64_t, size_t);
struct frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vector, error, rip, cs, flags, rsp, ss;
};
struct tss {
    uint32_t reserved;
    uint64_t rsp[3], reserved1, ist[7], reserved2;
    uint16_t reserved3, iomap;
} PACKED;
struct process;
struct cpu {
    uint64_t kernel_rsp, user_rsp;
    struct cpu *self;
    uint32_t index, apic_id;
    struct process *current, *switch_from;
    bool switching;
    struct frame *idle_frame;
    uint64_t gdt[7];
    struct tss tss;
    volatile uint32_t online;
    uint64_t ticks;
    uint8_t fpu[512] __attribute__((aligned(16)));
};
extern struct cpu cpus[MAX_CPU];
extern unsigned cpu_count;
static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile("movq %%gs:16,%0" : "=r"(c));
    return c;
}
void arch_init(struct cpu *);
void apic_init(void);
void apic_eoi(void);
void apic_timer(void);
void smp_init(uint64_t);
_Noreturn void ap_main(uint64_t);
struct frame *interrupt_dispatch(struct frame *);
struct frame *syscall_dispatch(struct frame *);
_Noreturn void frame_enter(struct frame *);
extern uint64_t boot_pml4[];
struct node;
struct file {
    struct node *node;
    uint64_t offset;
    int flags, refs;
};
struct process {
    int pid, ppid, state, status, cpu;
    unsigned uid, gid;
    uint64_t cr3, kernel_stack, brk_start, brk, mmap_next, wake_tick;
    uint64_t runtime[MAX_CPU];
    struct frame *frame;
    struct file *fds[MAX_FD];
    unsigned fd_flags[MAX_FD];
    struct node *cwd;
    char name[32];
    uint8_t fpu[512] __attribute__((aligned(16)));
};
enum { PROC_FREE, PROC_READY, PROC_RUNNING, PROC_SLEEP, PROC_ZOMBIE, PROC_DYING };
extern struct process processes[MAX_PROC];
extern volatile uint64_t ticks;
void sched_init(void);
struct frame *schedule(struct frame *);
int process_spawn(const char *);
int process_fork(struct frame *);
int process_exec(const char *, const char *const *, const char *const *, struct frame *);
struct frame *process_exit(struct frame *, int);
long process_wait(int, uint64_t, int);
void process_tick(void);
struct frame *schedule_finish(struct frame *);
void process_cleanup(struct process *);
void process_account(void);
void process_sleep(uint64_t);
void fpu_reset(uint8_t *);
struct blockdev {
    char name[16];
    uint64_t sectors;
    uint32_t sector_size;
    void *private;
    struct extfs *mounted;
    int (*transfer)(struct blockdev *, uint64_t, uint32_t, void *, bool);
    struct blockdev *next;
};
extern struct blockdev *block_devices;
void block_register(struct blockdev *);
int block_read(struct blockdev *, uint64_t, uint32_t, void *);
int block_write(struct blockdev *, uint64_t, uint32_t, const void *);
uint32_t pci_read(uint8_t, uint8_t, uint8_t, uint8_t);
void pci_write(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
void pci_init(void);
void ahci_probe(uint8_t, uint8_t, uint8_t);
struct ram_page {
    uint64_t index;
    struct ram_page *next;
    uint8_t bytes[PAGE];
};
struct extfs;
struct node {
    char name[256];
    uint32_t mode, uid, gid, links, ino;
    uint64_t size;
    unsigned refs, device, readers, writers;
    uint64_t read_position, write_position;
    struct node *parent, *children, *next, *mounted, *origin, *cache_next, *filesystem;
    struct ram_page *pages;
    struct extfs *ext;
};
struct kstat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    int64_t reserved[3];
};
extern struct node *root;
extern struct lock fs_lock;
void vfs_init(uint64_t);
struct node *node_actual(struct node *);
struct node *node_new(const char *, uint32_t, struct node *);
struct blockdev *vfs_block(struct node *);
struct node *vfs_resolve(const char *, struct node *, bool, int *);
int vfs_access(struct node *, unsigned);
struct node *vfs_create(const char *, uint32_t, const char *, int *);
long vfs_open(const char *, int, unsigned);
long vfs_close(int);
long vfs_read(int, uint64_t, size_t);
long vfs_write(int, uint64_t, size_t);
long vfs_seek(int, int64_t, int);
long vfs_stat(const char *, uint64_t, bool);
long vfs_fstat(int, uint64_t);
int node_stat(struct node *, struct kstat *);
long node_read(struct node *, uint64_t, void *, size_t);
long node_write(struct node *, uint64_t, const void *, size_t);
int node_truncate(struct node *);
long vfs_unlink(const char *, bool);
long vfs_link(const char *, const char *);
long vfs_symlink(const char *, const char *);
long vfs_readlink(const char *, uint64_t, size_t);
long vfs_chmod(const char *, unsigned);
long vfs_mount(const char *, const char *, const char *, unsigned);
long vfs_umount(const char *);
long vfs_getcwd(uint64_t, size_t);
long vfs_chdir(const char *);
long vfs_dup(int, int);
long vfs_pipe(uint64_t);
void vfs_process_close(struct process *);
void vfs_process_copy(struct process *, struct process *);
int ext2_mount(struct node *, struct blockdev *);
int ext2_refresh(struct node *);
void ext2_cache(struct node *);
int ext2_stat(struct node *, struct kstat *);
struct node *ext2_lookup(struct node *, const char *, int *);
int ext2_create(struct node *, struct node *, const char *);
long ext2_read(struct node *, uint64_t, void *, size_t);
long ext2_write(struct node *, uint64_t, const void *, size_t);
int ext2_truncate(struct node *);
int ext2_remove(struct node *, struct node *);
int ext2_link(struct node *, struct node *, const char *);
int ext2_chmod(struct node *);
int ext2_empty(struct node *);
int ext2_drop(struct node *);
int ext2_sync(struct extfs *);
long ext2_getdents(struct node *, uint64_t *, void *, size_t);
long vfs_getdents(int, uint64_t, size_t);
void ext2_release(struct extfs *);
int elf_load(struct process *, const char *, const char *const *, const char *const *,
             struct frame *);
void syscall_init(void);
#endif
