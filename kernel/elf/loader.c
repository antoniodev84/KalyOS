#include "kaly.h"
struct elf_header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstr;
} PACKED;
struct elf_program {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} PACKED;
struct image {
    uint64_t entry, phdr, base, end;
    uint16_t phnum;
    char interp[MAX_PATH];
};
static int read_exact(struct node *n, uint64_t off, void *b, size_t len) {
    long r = node_read(n, off, b, len);
    return r == (long)len ? 0 : r < 0 ? r : -ENOEXEC;
}
static int image_load(struct process *p, const char *path, uint64_t base, struct image *out,
                      bool interpreter) {
    int e;
    struct node *n = vfs_resolve(path, p->cwd, true, &e);
    if (!n) {
        return e;
    }
    if ((n->mode & S_IFMT) != S_IFREG || (e = vfs_access(n, 5))) {
        return e ? e : -ENOEXEC;
    }
    struct elf_header h;
    if ((e = read_exact(n, 0, &h, sizeof(h)))) {
        return e;
    }
    if (memcmp(h.ident, "\177ELF\2\1\1", 7) || h.machine != 62 || (h.type != 2 && h.type != 3) ||
        h.version != 1 || h.phentsize != sizeof(struct elf_program) || !h.phnum || h.phnum > 128 ||
        h.phoff > n->size || (uint64_t)h.phnum * h.phentsize > n->size - h.phoff) {
        return -ENOEXEC;
    }
    if (h.type == 2) {
        base = 0;
    }
    out->base = base;
    out->entry = h.entry + base;
    out->phnum = h.phnum;
    struct elf_program *ph = kalloc(h.phnum * sizeof(*ph));
    if (!ph) {
        return -ENOMEM;
    }
    e = read_exact(n, h.phoff, ph, h.phnum * sizeof(*ph));
    if (e) {
        goto done;
    }
    bool executable = false;
    for (unsigned j = 0; j < h.phnum; j++) {
        struct elf_program *s = &ph[j];
        if (s->type == 3) {
            if (interpreter || s->filesz < 2 || s->filesz > MAX_PATH) {
                e = -ENOEXEC;
                goto done;
            }
            e = read_exact(n, s->offset, out->interp, s->filesz);
            if (e || out->interp[s->filesz - 1]) {
                e = -ENOEXEC;
                goto done;
            }
        }
        if (s->type == 6) {
            out->phdr = s->vaddr + base;
        }
        if (s->type != 1 || !s->memsz) {
            continue;
        }
        if (s->filesz > s->memsz || s->offset > n->size || s->filesz > n->size - s->offset ||
            s->vaddr > USER_TOP - base || s->memsz > USER_TOP - base - s->vaddr ||
            s->vaddr + base < USER_MIN || ((s->vaddr - s->offset) & (PAGE - 1)) ||
            (s->flags & 3) == 3) {
            e = -ENOEXEC;
            goto done;
        }
        uint64_t start = (s->vaddr + base) & ~(PAGE - 1),
                 end = ALIGN_UP(s->vaddr + base + s->memsz, PAGE);
        uint64_t flags = (s->flags & 2 ? PTE_W : 0) | (s->flags & 1 ? 0 : PTE_NX);
        for (uint64_t va = start; va < end; va += PAGE) {
            uint64_t existing;
            if (vm_lookup(p->cr3, va, &existing)) {
                if ((!(existing & PTE_NX) && (flags & PTE_W)) ||
                    (!(flags & PTE_NX) && (existing & PTE_W))) {
                    e = -ENOEXEC;
                    goto done;
                }
                e = vm_protect(p->cr3, va, PAGE, (existing | flags) & (PTE_W | PTE_NX));
            } else {
                e = vm_alloc(p->cr3, va, PAGE, flags);
            }
            if (e) {
                goto done;
            }
        }
        for (uint64_t done = 0; done < s->filesz;) {
            uint64_t va = s->vaddr + base + done;
            size_t chunk = MIN(s->filesz - done, PAGE - va % PAGE);
            e = read_exact(n, s->offset + done, phys(vm_lookup(p->cr3, va, NULL)), chunk);
            if (e) {
                goto done;
            }
            done += chunk;
        }
        if (h.phoff >= s->offset &&
            h.phoff + (uint64_t)h.phnum * h.phentsize <= s->offset + s->filesz) {
            out->phdr = base + s->vaddr + h.phoff - s->offset;
        }
        if ((s->flags & 1) && out->entry >= base + s->vaddr &&
            out->entry < base + s->vaddr + s->memsz) {
            executable = true;
        }
        out->end = MAX(out->end, end);
    }
    if (!executable || !out->phdr) {
        e = -ENOEXEC;
    }
done:
    kfree(ph);
    return e;
}
static int stack_write(struct process *p, uint64_t va, const void *b, size_t n) {
    for (size_t done = 0; done < n;) {
        uint64_t addr = vm_lookup(p->cr3, va + done, NULL);
        if (!addr) {
            return -EFAULT;
        }
        size_t chunk = MIN(n - done, PAGE - (va + done) % PAGE);
        memcpy(phys(addr), (const uint8_t *)b + done, chunk);
        done += chunk;
    }
    return 0;
}
int elf_load(struct process *p, const char *path, const char *const *argv, const char *const *env,
             struct frame *frame) {
    struct image main = {0}, interp = {0};
    p->cr3 = vm_create();
    if (!p->cr3) {
        return -ENOMEM;
    }
    int e = image_load(p, path, 0x50000000, &main, false);
    if (e) {
        goto fail;
    }
    if (*main.interp) {
        e = image_load(p, main.interp, 0x60000000, &interp, true);
        if (e) {
            goto fail;
        }
    }
    uint64_t stack_base = USER_TOP - 0x21000, sp = USER_TOP - PAGE;
    e = vm_alloc(p->cr3, stack_base, 0x20000, PTE_W | PTE_NX);
    if (e) {
        goto fail;
    }
    uint64_t arg_addresses[65], env_addresses[65];
    unsigned argc = 0, envc = 0;
    while (argv && argv[argc]) {
        if (argc == 64) {
            e = -7;
            goto fail;
        }
        size_t len = strlen(argv[argc]) + 1;
        if (len > MAX_PATH || sp < stack_base + len) {
            e = -7;
            goto fail;
        }
        sp -= len;
        stack_write(p, sp, argv[argc], len);
        arg_addresses[argc++] = sp;
    }
    while (env && env[envc]) {
        if (envc == 64) {
            e = -7;
            goto fail;
        }
        size_t len = strlen(env[envc]) + 1;
        if (len > MAX_PATH || sp < stack_base + len) {
            e = -7;
            goto fail;
        }
        sp -= len;
        stack_write(p, sp, env[envc], len);
        env_addresses[envc++] = sp;
    }
    uint64_t values[180];
    unsigned count = 0;
    values[count++] = argc;
    for (unsigned j = 0; j < argc; j++) {
        values[count++] = arg_addresses[j];
    }
    values[count++] = 0;
    for (unsigned j = 0; j < envc; j++) {
        values[count++] = env_addresses[j];
    }
    values[count++] = 0;
    uint64_t aux[] = {3,  main.phdr,
                      4,  sizeof(struct elf_program),
                      5,  main.phnum,
                      6,  PAGE,
                      7,  interp.base,
                      9,  main.entry,
                      11, p->uid,
                      12, p->uid,
                      13, p->gid,
                      14, p->gid,
                      0,  0};
    memcpy(values + count, aux, sizeof(aux));
    count += sizeof(aux) / 8;
    sp = (sp - count * 8) & ~15UL;
    if (sp < stack_base) {
        e = -7;
        goto fail;
    }
    e = stack_write(p, sp, values, count * 8);
    if (e) {
        goto fail;
    }
    memset(frame, 0, sizeof(*frame));
    frame->rip = *main.interp ? interp.entry : main.entry;
    frame->rsp = sp;
    frame->cs = 35;
    frame->ss = 27;
    frame->flags = 0x202;
    p->brk_start = p->brk = main.end;
    const char *name = path;
    for (const char *s = path; *s; s++) {
        if (*s == '/') {
            name = s + 1;
        }
    }
    size_t len = MIN(strlen(name), sizeof(p->name) - 1);
    memcpy(p->name, name, len);
    p->name[len] = 0;
    return 0;
fail:
    vm_destroy(p->cr3);
    p->cr3 = 0;
    return e;
}
