#include "kaly.h"
#include <kaly/system.h>
struct process processes[MAX_PROC];
volatile uint64_t ticks;
static struct lock sched_lock;
static unsigned cursor;
static unsigned next_pid = 11;
static bool start_gui;
static struct process *reserve_process(unsigned requested_pid) {
    lock(&sched_lock);
    struct process *p = NULL;
    if (requested_pid) {
        for (unsigned i = 0; i < MAX_PROC; i++) {
            if (processes[i].state && processes[i].pid == (int)requested_pid) {
                unlock(&sched_lock);
                return NULL;
            }
        }
    }
    if (!requested_pid) {
        for (unsigned attempt = 0; attempt <= MAX_PROC; attempt++) {
            unsigned candidate = next_pid;
            next_pid = next_pid == 0x7fffffff ? 11 : next_pid + 1;
            bool occupied = false;
            for (unsigned i = 0; i < MAX_PROC; i++) {
                if (processes[i].state && processes[i].pid == (int)candidate) {
                    occupied = true;
                    break;
                }
            }
            if (!occupied) {
                requested_pid = candidate;
                break;
            }
        }
        if (!requested_pid) {
            unlock(&sched_lock);
            return NULL;
        }
    }
    for (unsigned i = 0; i < MAX_PROC; i++) {
        if (!processes[i].state) {
            p = &processes[i];
            memset(p, 0, sizeof(*p));
            p->cpu = -1;
            p->state = PROC_SLEEP;
            p->pid = requested_pid;
            break;
        }
    }
    unlock(&sched_lock);
    if (!p) {
        return NULL;
    }
    p->kernel_stack = pmm_alloc_contiguous(8);
    if (!p->kernel_stack) {
        p->state = PROC_FREE;
        return NULL;
    }
    p->frame = (struct frame *)((uint64_t)phys(p->kernel_stack) + 8 * PAGE - sizeof(struct frame));
    fpu_reset(p->fpu);
    return p;
}
void sched_init(void) {
    memset(processes, 0, sizeof(processes));
}
void process_cleanup(struct process *p) {
    vm_destroy(p->cr3);
    if (p->kernel_stack) {
        for (unsigned i = 0; i < 8; i++) {
            pmm_free(p->kernel_stack + i * PAGE);
        }
    }
    lock(&sched_lock);
    memset(p, 0, sizeof(*p));
    unlock(&sched_lock);
}
int process_spawn(const char *path) {
    struct process *p = reserve_process(!strcmp(path, "/init") ? 1 : 0);
    if (!p) {
        return -ENOMEM;
    }
    p->cwd = root;
    p->mmap_next = 0x100000000UL;
    const char *argv[] = {path, NULL}, *env[] = {"PATH=/bin", "HOME=/", "TERM=kaly", NULL};
    int e = elf_load(p, path, argv, env, p->frame);
    if (e) {
        process_cleanup(p);
        return e;
    }
    lock(&sched_lock);
    p->state = PROC_READY;
    unlock(&sched_lock);
    return p->pid;
}
int process_fork(struct frame *f) {
    struct process *parent = this_cpu()->current;
    unsigned requested = parent->system_child_pid;
    parent->system_child_pid = 0;
    struct process *p = reserve_process(requested);
    if (!p) {
        return -ENOMEM;
    }
    p->cr3 = vm_clone(parent->cr3);
    if (!p->cr3) {
        process_cleanup(p);
        return -ENOMEM;
    }
    p->ppid = parent->pid;
    p->uid = parent->uid;
    p->gid = parent->gid;
    p->cwd = parent->cwd;
    p->brk = parent->brk;
    p->brk_start = parent->brk_start;
    p->mmap_next = parent->mmap_next;
    strcpy(p->name, parent->name);
    __asm__ volatile("fxsave64 %0" : "=m"(parent->fpu));
    memcpy(p->fpu, parent->fpu, sizeof(p->fpu));
    *p->frame = *f;
    p->frame->rax = 0;
    lock(&fs_lock);
    vfs_process_copy(p, parent);
    unlock(&fs_lock);
    lock(&sched_lock);
    p->state = PROC_READY;
    unlock(&sched_lock);
    return p->pid;
}
int process_exec(const char *path, const char *const *argv, const char *const *env,
                 struct frame *f) {
    struct process *p = this_cpu()->current;
    struct process replacement = {0};
    replacement.uid = p->uid;
    replacement.gid = p->gid;
    replacement.cwd = p->cwd;
    replacement.mmap_next = 0x100000000UL;
    struct frame new_frame;
    lock(&fs_lock);
    int e = elf_load(&replacement, path, argv, env, &new_frame);
    if (e) {
        unlock(&fs_lock);
        return e;
    }
    for (unsigned i = 0; i < MAX_FD; i++) {
        if (p->fds[i] && (p->fd_flags[i] & O_CLOEXEC)) {
            vfs_close(i);
        }
    }
    unlock(&fs_lock);
    wm_process_exit(p->pid);
    p->system_child_pid = 0;
    uint64_t old = p->cr3;
    p->cr3 = replacement.cr3;
    p->brk = replacement.brk;
    p->brk_start = replacement.brk_start;
    p->mmap_next = replacement.mmap_next;
    strcpy(p->name, replacement.name);
    fpu_reset(p->fpu);
    __asm__ volatile("fxrstor64 %0" ::"m"(p->fpu));
    *f = new_frame;
    __asm__ volatile("mov %0,%%cr3" ::"r"(p->cr3) : "memory");
    vm_destroy(old);
    return 0;
}
struct frame *schedule(struct frame *f) {
    struct cpu *c = this_cpu();
    lock(&sched_lock);
    struct process *old = c->current;
    if (old) {
        __asm__ volatile("fxsave64 %0" : "=m"(old->fpu));
        old->frame = f;
        if (old->state == PROC_RUNNING) {
            old->state = PROC_READY;
        }
    } else {
        __asm__ volatile("fxsave64 %0" : "=m"(c->fpu));
        c->idle_frame = f;
    }
    struct process *next = NULL;
    for (unsigned count = 0; count < MAX_PROC; count++) {
        unsigned i = (cursor + count) % MAX_PROC;
        if (processes[i].state != PROC_READY || (processes[i].cpu != -1 && &processes[i] != old)) {
            continue;
        }
        next = &processes[i];
        cursor = (i + 1) % MAX_PROC;
        break;
    }
    c->current = next;
    struct frame *result = c->idle_frame;
    uint64_t cr3 = physical(boot_pml4);
    if (next) {
        next->state = PROC_RUNNING;
        next->cpu = c->index;
        c->kernel_rsp = (uint64_t)phys(next->kernel_stack) + 8 * PAGE;
        c->tss.rsp[0] = c->kernel_rsp;
        result = next->frame;
        cr3 = next->cr3;
    }
    uint8_t *fpu = next ? next->fpu : c->fpu;
    __asm__ volatile("fxrstor64 (%0)" ::"r"(fpu) : "memory");
    __asm__ volatile("mov %0,%%cr3" ::"r"(cr3) : "memory");
    c->switch_from = old;
    c->switching = true;
    return result;
}
void process_tick(void) {
    uint64_t now = __atomic_add_fetch(&ticks, 1, __ATOMIC_RELAXED);
    lock(&sched_lock);
    for (unsigned i = 0; i < MAX_PROC; i++) {
        if (processes[i].state == PROC_SLEEP && processes[i].wake_tick &&
            processes[i].wake_tick <= now) {
            processes[i].wake_tick = 0;
            processes[i].state = PROC_READY;
        }
    }
    unlock(&sched_lock);
}
void process_account(void) {
    struct cpu *c = this_cpu();
    if (c->current) {
        c->current->runtime[c->index]++;
    }
}
struct frame *process_exit(struct frame *f, int status) {
    struct process *p = this_cpu()->current;
    wm_process_exit(p->pid);
    lock(&fs_lock);
    vfs_process_close(p);
    unlock(&fs_lock);
    lock(&sched_lock);
    p->status = (status & 255) << 8;
    p->state = PROC_DYING;
    for (unsigned i = 0; i < MAX_PROC; i++) {
        if (processes[i].state && processes[i].ppid == p->pid) {
            processes[i].ppid = 1;
        }
    }
    unlock(&sched_lock);
    return schedule(f);
}
long process_wait(int pid, uint64_t status, int options) {
    if (options & ~1) {
        return -EINVAL;
    }
    if (status && user_range(status, 4, true)) {
        return -EFAULT;
    }
    struct process *self = this_cpu()->current, *child = NULL;
    bool found = false;
    lock(&sched_lock);
    for (unsigned i = 0; i < MAX_PROC; i++) {
        struct process *p = &processes[i];
        if (!p->state || p->ppid != self->pid || (pid != -1 && p->pid != pid)) {
            continue;
        }
        found = true;
        if (p->state == PROC_ZOMBIE) {
            child = p;
            p->state = PROC_SLEEP;
            break;
        }
    }
    unlock(&sched_lock);
    if (!found) {
        return -ECHILD;
    }
    if (!child) {
        return options & 1 ? 0 : -EAGAIN;
    }
    int result = child->pid;
    if (status) {
        copy_to_user(status, &child->status, 4);
    }
    process_cleanup(child);
    return result;
}

void fpu_reset(uint8_t *state) {
    memset(state, 0, 512);
    *(uint16_t *)state = 0x037f;
    *(uint32_t *)(state + 24) = 0x1f80;
    *(uint32_t *)(state + 28) = 0xffff;
}

void process_sleep(uint64_t duration) {
    struct process *p = this_cpu()->current;
    lock(&sched_lock);
    p->wake_tick = ticks + MAX(duration, 1);
    p->state = PROC_SLEEP;
    unlock(&sched_lock);
}

struct frame *schedule_finish(struct frame *frame) {
    struct cpu *c = this_cpu();
    if (c->switching) {
        struct process *old = c->switch_from;
        if (old && old != c->current) {
            old->cpu = -1;
            if (old->state == PROC_DYING) {
                old->state = PROC_ZOMBIE;
            }
        }
        c->switch_from = NULL;
        c->switching = false;
        unlock(&sched_lock);
    }
    if ((frame->cs & 3) && (frame->rip < USER_MIN || frame->rip >= USER_TOP ||
                            frame->rsp < USER_MIN || frame->rsp >= USER_TOP)) {
        return process_exit(frame, 139);
    }
    return frame;
}

long system_ioctl(unsigned long request, uint64_t address) {
    if (request != KSYSTEM_IOCTL) {
        return -25;
    }
    struct ksystem_request r;
    if (user_range(address, sizeof(r), true) || copy_from_user(&r, address, sizeof(r))) {
        return -EFAULT;
    }
    if (r.version != KSYSTEM_VERSION) {
        return -EINVAL;
    }
    struct process *p = this_cpu()->current;
    long result = 0;
    lock(&sched_lock);
    switch (r.operation) {
    case KSYSTEM_RESERVE_PID:
        if (p->uid || r.pid < 2 || r.pid > 10 ||
            !((p->pid == 1 && r.pid != 4 && r.pid != 5) || (p->pid == 3 && r.pid == 4) ||
              (p->pid == 4 && r.pid == 5))) {
            result = -EPERM;
            break;
        }
        for (unsigned i = 0; i < MAX_PROC; i++) {
            if (processes[i].state && processes[i].pid == (int)r.pid) {
                result = -EBUSY;
                break;
            }
        }
        if (!result) {
            p->system_child_pid = r.pid;
        }
        break;
    case KSYSTEM_START_GUI:
        start_gui = true;
        break;
    case KSYSTEM_POLL_GUI:
        if (p->pid != 1 || p->uid) {
            result = -EPERM;
        } else {
            r.flags = start_gui;
            start_gui = false;
        }
        break;
    default:
        result = -EINVAL;
    }
    unlock(&sched_lock);
    if (!result && copy_to_user(address, &r, sizeof(r))) {
        result = -EFAULT;
    }
    return result;
}
