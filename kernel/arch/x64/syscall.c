#include "net.h"
#include "usb.h"
struct syscall_args {
    uint64_t a, b, c, d, e, f;
};
typedef long (*handler)(struct syscall_args *);
static struct process *self(void) {
    return this_cpu()->current;
}
static long sys_read(struct syscall_args *a) {
    return vfs_read(a->a, a->b, a->c);
}
static long sys_write(struct syscall_args *a) {
    return vfs_write(a->a, a->b, a->c);
}
static int path_read(char *p, uint64_t user) {
    return user_string(p, user, MAX_PATH);
}
static long sys_open(struct syscall_args *a) {
    char p[MAX_PATH];
    int e = path_read(p, a->a);
    return e ? e : vfs_open(p, a->b, a->c);
}
static long sys_close(struct syscall_args *a) {
    return vfs_close(a->a);
}
static long sys_stat(struct syscall_args *a) {
    char p[MAX_PATH];
    int e = path_read(p, a->a);
    return e ? e : vfs_stat(p, a->b, true);
}
static long sys_lstat(struct syscall_args *a) {
    char p[MAX_PATH];
    int e = path_read(p, a->a);
    return e ? e : vfs_stat(p, a->b, false);
}
static long sys_fstat(struct syscall_args *a) {
    return vfs_fstat(a->a, a->b);
}
static long sys_lseek(struct syscall_args *a) {
    return vfs_seek(a->a, a->b, a->c);
}
static uint64_t page_flags(unsigned prot) {
    return (prot & 2 ? PTE_W : 0) | (prot & 4 ? 0 : PTE_NX) | (!prot ? 512 : 0);
}
static long sys_mmap(struct syscall_args *a) {
    struct process *p = self();
    uint64_t size = a->b;
    if (!size || size > USER_TOP - USER_MIN || size > SIZE_MAX - PAGE || (a->c & ~7UL) ||
        ((a->c & 6) == 6) || (a->d & ~(2UL | MAP_FIXED | MAP_ANONYMOUS)) || !(a->d & 2) ||
        a->f % PAGE) {
        return -EINVAL;
    }
    size = ALIGN_UP(size, PAGE);
    uint64_t va = a->a;
    if (!(a->d & MAP_FIXED)) {
        if (!va) {
            va = p->mmap_next;
        }
        va = ALIGN_UP(MAX(va, USER_MIN), PAGE);
        while (va < USER_TOP - size) {
            bool free = true;
            for (uint64_t off = 0; off < size; off += PAGE) {
                if (vm_lookup(p->cr3, va + off, NULL)) {
                    va += off + PAGE;
                    free = false;
                    break;
                }
            }
            if (free) {
                break;
            }
        }
    }
    if (va < USER_MIN || va % PAGE || va >= USER_TOP || size > USER_TOP - va) {
        return -EINVAL;
    }
    struct file *file = NULL;
    if (!(a->d & MAP_ANONYMOUS)) {
        if (a->e >= MAX_FD || !(file = p->fds[a->e]) || (file->flags & 3) == 1) {
            return -EBADF;
        }
        if ((file->node->mode & S_IFMT) != S_IFREG) {
            return -EINVAL;
        }
    }
    if (a->d & MAP_FIXED) {
        vm_unmap(p->cr3, va, size);
    }
    int e = vm_alloc(p->cr3, va, size, page_flags(a->c));
    if (e) {
        return e;
    }
    if (file) {
        for (uint64_t off = 0; off < size; off += PAGE) {
            uint64_t pa = vm_lookup(p->cr3, va + off, NULL);
            if (!pa) {
                e = -EINVAL;
                break;
            }
            long r = node_read(file->node, a->f + off, phys(pa), PAGE);
            if (r < 0) {
                e = r;
                break;
            }
        }
        if (e) {
            vm_unmap(p->cr3, va, size);
            return e;
        }
    }
    p->mmap_next = MAX(p->mmap_next, va + size);
    return va;
}
static long sys_mprotect(struct syscall_args *a) {
    if (a->a < USER_MIN || a->a >= USER_TOP || a->a % PAGE || a->b > USER_TOP - a->a ||
        (a->c & ~7UL) || (a->c & 6) == 6) {
        return -EINVAL;
    }
    return vm_protect(self()->cr3, a->a, ALIGN_UP(a->b, PAGE), page_flags(a->c));
}
static long sys_munmap(struct syscall_args *a) {
    if (!a->b || a->a % PAGE || a->a < USER_MIN || a->a >= USER_TOP || a->b > USER_TOP - a->a) {
        return -EINVAL;
    }
    vm_unmap(self()->cr3, a->a, ALIGN_UP(a->b, PAGE));
    return 0;
}
static long sys_brk(struct syscall_args *a) {
    struct process *p = self();
    uint64_t end = a->a;
    if (!end || end < p->brk_start || end >= p->mmap_next) {
        return p->brk;
    }
    uint64_t old = ALIGN_UP(p->brk, PAGE), next = ALIGN_UP(end, PAGE);
    if (next > old && vm_alloc(p->cr3, old, next - old, PTE_W | PTE_NX)) {
        return p->brk;
    }
    if (next < old) {
        vm_unmap(p->cr3, next, old - next);
    }
    p->brk = end;
    return end;
}
static long sys_pipe(struct syscall_args *a) {
    return vfs_pipe(a->a);
}
static long sys_getuid(struct syscall_args *a) {
    (void)a;
    return self()->uid;
}
static long sys_dup2(struct syscall_args *a) {
    if (a->b >= MAX_FD) {
        return -EBADF;
    }
    return vfs_dup(a->a, a->b);
}
static long sys_getpid(struct syscall_args *a) {
    (void)a;
    return self()->pid;
}
static long sys_uname(struct syscall_args *a) {
    char u[6][65] = {"KalyOS", "kaly", "0.1", "x86_64", "x86_64", "local"};
    return copy_to_user(a->a, u, sizeof(u));
}
static long sys_getcwd(struct syscall_args *a) {
    return vfs_getcwd(a->a, a->b);
}
static long sys_chdir(struct syscall_args *a) {
    char p[MAX_PATH];
    int e = path_read(p, a->a);
    return e ? e : vfs_chdir(p);
}
static long sys_mkdir(struct syscall_args *a) {
    char p[MAX_PATH];
    int e = path_read(p, a->a);
    if (e) {
        return e;
    }
    return vfs_create(p, S_IFDIR | (a->b & 07777), NULL, &e) ? 0 : e;
}
static long sys_rmdir(struct syscall_args *a) {
    char p[MAX_PATH];
    int e = path_read(p, a->a);
    return e ? e : vfs_unlink(p, true);
}
static long sys_link(struct syscall_args *a) {
    char p[MAX_PATH], q[MAX_PATH];
    int e = path_read(p, a->a);
    if (!e) {
        e = path_read(q, a->b);
    }
    return e ? e : vfs_link(p, q);
}
static long sys_unlink(struct syscall_args *a) {
    char p[MAX_PATH];
    int e = path_read(p, a->a);
    return e ? e : vfs_unlink(p, false);
}
static long sys_symlink(struct syscall_args *a) {
    char p[MAX_PATH], q[MAX_PATH];
    int e = path_read(p, a->a);
    if (!e) {
        e = path_read(q, a->b);
    }
    return e ? e : vfs_symlink(p, q);
}
static long sys_readlink(struct syscall_args *a) {
    char p[MAX_PATH];
    int e = path_read(p, a->a);
    return e ? e : vfs_readlink(p, a->b, a->c);
}
static long sys_chmod(struct syscall_args *a) {
    char p[MAX_PATH];
    int e = path_read(p, a->a);
    return e ? e : vfs_chmod(p, a->b);
}
static long sys_setuid(struct syscall_args *a) {
    if (a->a > 65535) {
        return -EINVAL;
    }
    if (self()->uid && a->a != self()->uid) {
        return -EPERM;
    }
    self()->uid = a->a;
    return 0;
}
static long sys_mount(struct syscall_args *a) {
    char p[MAX_PATH], q[MAX_PATH], t[MAX_PATH];
    int e = path_read(p, a->a);
    if (!e) {
        e = path_read(q, a->b);
    }
    if (!e) {
        e = path_read(t, a->c);
    }
    if (a->e) {
        return -EINVAL;
    }
    return e ? e : vfs_mount(p, q, t, a->d);
}
static long sys_umount(struct syscall_args *a) {
    char p[MAX_PATH];
    int e = path_read(p, a->a);
    if (a->b) {
        return -EINVAL;
    }
    return e ? e : vfs_umount(p);
}
static long sys_reboot(struct syscall_args *a) {
    if (self()->uid) {
        return -EPERM;
    }
    if (a->a != 0xfee1dead ||
        (a->b != 672274793 && a->b != 85072278 && a->b != 369367448 && a->b != 537993216) ||
        a->c != 0x4321fedc) {
        return -EINVAL;
    }
    lock(&fs_lock);
    int e = acpi_shutdown();
    unlock(&fs_lock);
    return e;
}
static long sys_getdents(struct syscall_args *a) {
    return vfs_getdents(a->a, a->b, a->c);
}
struct syscall_entry {
    unsigned number;
    handler call;
    bool filesystem;
};
static const struct syscall_entry calls[] = {
    {169, sys_reboot, false}, {0, sys_read, true},       {1, sys_write, true},
    {2, sys_open, true},      {3, sys_close, true},      {4, sys_stat, true},
    {5, sys_fstat, true},     {6, sys_lstat, true},      {8, sys_lseek, true},
    {9, sys_mmap, true},      {10, sys_mprotect, false}, {11, sys_munmap, false},
    {12, sys_brk, false},     {22, sys_pipe, true},      {102, sys_getuid, false},
    {24, NULL, false},        {33, sys_dup2, true},      {35, NULL, false},
    {39, sys_getpid, false},  {57, NULL, false},         {59, NULL, false},
    {60, NULL, false},        {61, NULL, false},         {63, sys_uname, false},
    {79, sys_getcwd, true},   {80, sys_chdir, true},     {83, sys_mkdir, true},
    {84, sys_rmdir, true},    {86, sys_link, true},      {87, sys_unlink, true},
    {88, sys_symlink, true},  {89, sys_readlink, true},  {90, sys_chmod, true},
    {105, sys_setuid, false}, {165, sys_mount, true},    {166, sys_umount, true},
    {217, sys_getdents, true}};
_Static_assert(sizeof(calls) / sizeof(calls[0]) == 37, "syscalls");
static int vector_copy(uint64_t user, char ***result) {
    char **v = kalloc(65 * sizeof(*v));
    if (!v) {
        return -ENOMEM;
    }
    *result = v;
    if (!user) {
        return 0;
    }
    for (unsigned i = 0; i <= 64; i++) {
        uint64_t ptr;
        if (copy_from_user(&ptr, user + i * 8, 8)) {
            return -EFAULT;
        }
        if (!ptr) {
            return 0;
        }
        if (i == 64) {
            return -7;
        }
        v[i] = kalloc(MAX_PATH);
        if (!v[i]) {
            return -ENOMEM;
        }
        int e = user_string(v[i], ptr, MAX_PATH);
        if (e) {
            return e;
        }
    }
    return -7;
}
static void vector_free(char **v) {
    if (v) {
        for (unsigned i = 0; i < 64 && v[i]; i++) {
            kfree(v[i]);
        }
        kfree(v);
    }
}
struct frame *syscall_dispatch(struct frame *f) {
    self()->frame = f;
    f->flags = (f->flags & 0x240dd5) | 0x202;
    struct syscall_args args = {f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9};
    uint64_t nr = f->rax;
    bool retry = self()->retry_syscall == nr + 1;
    self()->retry_syscall = 0;
    long result = -ENOSYS;
    if (nr == 60) {
        return process_exit(f, args.a);
    }
    if (nr == 24) {
        f->rax = 0;
        return schedule(f);
    }
    if (nr == 57) {
        result = process_fork(f);
    } else if (nr == 61) {
        if (args.d) {
            result = -EINVAL;
        } else {
            result = process_wait(args.a, args.b, args.c);
        }
    } else if (nr == 35) {
        int64_t time[2];
        result = copy_from_user(time, args.a, sizeof(time));
        if (!result &&
            (time[0] < 0 || time[1] < 0 || time[1] >= 1000000000 || time[0] > INT64_MAX / 100)) {
            result = -EINVAL;
        }
        if (!result && args.b) {
            int64_t zero[2] = {0};
            result = copy_to_user(args.b, zero, sizeof(zero));
        }
        if (!result) {
            process_sleep(time[0] * 100 + (time[1] + 9999999) / 10000000);
            f->rax = 0;
            return schedule(f);
        }
    } else if (nr == 59) {
        char path[MAX_PATH];
        char **argv = NULL, **env = NULL;
        result = path_read(path, args.a);
        if (!result) {
            result = vector_copy(args.b, &argv);
        }
        if (!result) {
            result = vector_copy(args.c, &env);
        }
        if (!result) {
            result = process_exec(path, (const char *const *)argv, (const char *const *)env, f);
        }
        vector_free(argv);
        vector_free(env);
        if (!result) {
            return f;
        }
    } else if (nr == 41) {
        lock(&fs_lock);
        result = net_socket_create(args.a, args.b, args.c);
        unlock(&fs_lock);
    } else if (nr == 42) {
        result = net_socket_connect(args.a, args.b, args.c, retry);
    } else if (nr == 49) {
        result = net_socket_bind(args.a, args.b, args.c);
    } else if (nr == 44) {
        result = net_socket_send(args.a, args.b, args.c, args.d, args.e, args.f);
    } else if (nr == 45) {
        result = net_socket_receive(args.a, args.b, args.c, args.d, args.e, args.f);
    } else if (nr == 55) {
        result = net_socket_option(args.a, args.b, args.c, args.d, args.e);
    } else if (nr == 48) {
        result = net_socket_shutdown(args.a, args.b);
    } else if (nr == 51 || nr == 52) {
        result = net_socket_name(args.a, args.b, args.c, nr == 52);
    } else if (nr == 16) {
        struct file *file = args.a < MAX_FD ? self()->fds[args.a] : NULL;
        if (!file) {
            result = -EBADF;
        } else if (file->node->device == 5) {
            result = hda_ioctl(args.b, args.c);
        } else if (file && file->node->device == 6 && args.b == 0x5501) {
            if (self()->uid) {
                result = -EPERM;
            } else {
                lock(&fs_lock);
                usb_init();
                block_scan_partitions();
                block_publish();
                unlock(&fs_lock);
                result = 0;
            }
        } else if (file->node->socket) {
            result = net_ioctl(args.a, args.b, args.c);
        } else {
            result = -25;
        }
    } else {
        for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
            if (calls[i].number == nr && calls[i].call) {
                if (calls[i].filesystem) {
                    lock(&fs_lock);
                }
                result = calls[i].call(&args);
                if (calls[i].filesystem) {
                    unlock(&fs_lock);
                }
                break;
            }
        }
    }
    bool nonblocking = (nr == 44 || nr == 45) && (args.d & 64);
    if ((nr == 0 || nr == 1 || nr == 42 || nr == 44 || nr == 45) && args.a < MAX_FD &&
        self()->fds[args.a]) {
        nonblocking |= (self()->fds[args.a]->flags & 0x800) != 0;
    }
    if (result == -EAGAIN && !nonblocking &&
        (nr == 0 || nr == 1 || nr == 42 || nr == 44 || nr == 45 || nr == 61)) {
        self()->retry_syscall = nr + 1;
        f->rip -= 2;
        process_sleep(1);
        return schedule(f);
    }
    f->rax = result;
    return f;
}
