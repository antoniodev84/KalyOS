#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
struct header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstr;
};
struct program {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
struct dynamic {
    int64_t tag;
    uint64_t value;
};
struct symbol {
    uint32_t name;
    uint8_t info, other;
    uint16_t section;
    uint64_t value, size;
};
struct rela {
    uint64_t offset, info;
    int64_t addend;
};
struct object {
    char name[256];
    uint64_t base;
    struct program *ph;
    size_t phnum;
    struct dynamic *dynamic;
    struct symbol *symbols;
    char *strings;
    uint32_t *hash, *gnu_hash;
    struct rela *rela, *plt;
    size_t rela_count, plt_count;
    uint64_t init, *init_array;
    size_t init_count;
    bool initialized, initializing;
};
static struct object objects[32];
static unsigned object_count;
char **environ;
extern struct dynamic _DYNAMIC[] __attribute__((visibility("hidden")));
static _Noreturn void die(const char *s) {
    write(2, s, strlen(s));
    write(2, "\n", 1);
    _exit(127);
}
static uint64_t dynamic_value(struct object *o, int64_t tag) {
    for (struct dynamic *d = o->dynamic; d && d->tag; d++) {
        if (d->tag == tag) {
            return d->value;
        }
    }
    return 0;
}
static void inspect(struct object *o) {
    for (unsigned i = 0; i < o->phnum; i++) {
        if (o->ph[i].type == 2) {
            o->dynamic = (void *)(o->base + o->ph[i].vaddr);
        }
    }
    if (!o->dynamic) {
        die("missing dynamic section");
    }
    o->symbols = (void *)(o->base + dynamic_value(o, 6));
    o->strings = (void *)(o->base + dynamic_value(o, 5));
    uint64_t value = dynamic_value(o, 4);
    if (value) {
        o->hash = (void *)(o->base + value);
    }
    value = dynamic_value(o, 0x6ffffef5);
    if (value) {
        o->gnu_hash = (void *)(o->base + value);
    }
    value = dynamic_value(o, 7);
    if (value) {
        o->rela = (void *)(o->base + value);
    }
    o->rela_count = dynamic_value(o, 8) / sizeof(struct rela);
    value = dynamic_value(o, 23);
    if (value) {
        o->plt = (void *)(o->base + value);
    }
    o->plt_count = dynamic_value(o, 2) / sizeof(struct rela);
    if ((o->rela_count && dynamic_value(o, 9) != sizeof(struct rela)) ||
        (o->plt_count && dynamic_value(o, 20) != 7) || dynamic_value(o, 22)) {
        die("unsupported ELF relocation format");
    }
    value = dynamic_value(o, 12);
    if (value) {
        o->init = o->base + value;
    }
    value = dynamic_value(o, 25);
    if (value) {
        o->init_array = (void *)(o->base + value);
    }
    o->init_count = dynamic_value(o, 27) / 8;
}
static void read_exact(int fd, uint64_t off, void *buffer, size_t n) {
    if (lseek(fd, off, SEEK_SET) < 0) {
        die("dynamic seek failed");
    }
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (uint8_t *)buffer + done, n - done);
        if (r <= 0) {
            die("dynamic read failed");
        }
        done += r;
    }
}
static struct object *load_library(const char *name) {
    for (unsigned i = 0; i < object_count; i++) {
        if (!strcmp(objects[i].name, name)) {
            return &objects[i];
        }
    }
    if (object_count == 32 || strlen(name) >= 256 || strchr(name, '/')) {
        die("invalid shared library");
    }
    char path[512];
    strcpy(path, "/lib/");
    strcat(path, name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        die(path);
    }
    struct header h;
    read_exact(fd, 0, &h, sizeof(h));
    if (memcmp(h.ident, "\177ELF\2\1\1", 7) || h.type != 3 || h.machine != 62 ||
        h.phentsize != sizeof(struct program) || !h.phnum || h.phnum > 128) {
        die("invalid shared ELF");
    }
    struct object *o = &objects[object_count++];
    strcpy(o->name, name);
    o->base = 0x61000000 + (uint64_t)(object_count - 2) * 0x1000000;
    o->phnum = h.phnum;
    o->ph = mmap(NULL, h.phnum * sizeof(struct program), PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (o->ph == MAP_FAILED) {
        die("dynamic allocation failed");
    }
    read_exact(fd, h.phoff, o->ph, h.phnum * sizeof(struct program));
    for (unsigned i = 0; i < o->phnum; i++) {
        struct program *p = &o->ph[i];
        if (p->type != 1 || !p->memsz) {
            continue;
        }
        if (p->filesz > p->memsz || p->vaddr + p->memsz > 0x1000000 ||
            (p->vaddr & 4095) != (p->offset & 4095) || (p->flags & 3) == 3) {
            die("invalid shared segment");
        }
        uint64_t start = (o->base + p->vaddr) & ~4095UL,
                 size = ((o->base + p->vaddr + p->memsz + 4095) & ~4095UL) - start;
        if (mmap((void *)start, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
            die("shared mapping failed");
        }
        read_exact(fd, p->offset, (void *)(o->base + p->vaddr), p->filesz);
    }
    close(fd);
    inspect(o);
    return o;
}
static void dependencies(struct object *o) {
    for (struct dynamic *d = o->dynamic; d->tag; d++) {
        if (d->tag == 1) {
            unsigned before = object_count;
            struct object *dep = load_library(o->strings + d->value);
            if (object_count != before) {
                dependencies(dep);
            }
        }
    }
}
static uint32_t sysv_hash(const char *name) {
    uint32_t h = 0;
    while (*name) {
        h = (h << 4) + (uint8_t)*name++;
        uint32_t g = h & 0xf0000000;
        if (g) {
            h ^= g >> 24;
        }
        h &= ~g;
    }
    return h;
}
static struct symbol *lookup(struct object *o, const char *name) {
    if (o->hash) {
        uint32_t buckets = o->hash[0], count = o->hash[1];
        if (!buckets) {
            return NULL;
        }
        uint32_t *bucket = o->hash + 2, *chain = bucket + buckets;
        for (uint32_t n = bucket[sysv_hash(name) % buckets], steps = 0;
             n && n < count && steps++ < count; n = chain[n]) {
            struct symbol *s = &o->symbols[n];
            if (s->section && !strcmp(o->strings + s->name, name)) {
                return s;
            }
        }
    } else if (o->gnu_hash) {
        uint32_t *h = o->gnu_hash, hash = 5381;
        for (const char *s = name; *s; s++) {
            hash = hash * 33 + (uint8_t)*s;
        }
        uint64_t *bloom = (void *)(h + 4);
        uint32_t *bucket = (void *)(bloom + h[2]), *chain = bucket + h[0];
        if (!h[0]) {
            return NULL;
        }
        uint32_t n = bucket[hash % h[0]];
        if (n < h[1]) {
            return NULL;
        }
        for (unsigned steps = 0; steps < 1000000; steps++, n++) {
            uint32_t value = chain[n - h[1]];
            struct symbol *s = &o->symbols[n];
            if ((value | 1) == (hash | 1) && s->section && !strcmp(o->strings + s->name, name)) {
                return s;
            }
            if (value & 1) {
                break;
            }
        }
    }
    return NULL;
}
static uint64_t symbol_address(struct object *from, unsigned index, bool copy) {
    struct symbol *source = &from->symbols[index];
    if (source->section &&
        ((source->info >> 4) == 0 || (source->other & 3) == 2 || (source->other & 3) == 3)) {
        return source->section == 0xfff1 ? source->value : from->base + source->value;
    }
    const char *name = from->strings + source->name;
    uint64_t weak = 0;
    for (unsigned i = 0; i < object_count; i++) {
        struct object *o = &objects[i];
        if (copy && o == from) {
            continue;
        }
        struct symbol *s = lookup(o, name);
        if (!s || (s->other & 3) == 2 || (s->info >> 4) == 0) {
            continue;
        }
        uint64_t value = s->section == 0xfff1 ? s->value : o->base + s->value;
        if ((s->info >> 4) == 2) {
            if (!weak) {
                weak = value;
            }
            continue;
        }
        return value;
    }
    if (weak) {
        return weak;
    }
    if ((source->info >> 4) == 2) {
        return 0;
    }
    die(name);
}
static void relocate_table(struct object *o, struct rela *r, size_t count) {
    for (size_t i = 0; i < count; i++) {
        unsigned type = r[i].info, sym = r[i].info >> 32;
        uint64_t *destination = (void *)(o->base + r[i].offset);
        if (!type) {
            continue;
        }
        if (type == 8) {
            *destination = o->base + r[i].addend;
            continue;
        }
        uint64_t value = symbol_address(o, sym, type == 5);
        if (type == 1 || type == 6 || type == 7) {
            *destination = value + r[i].addend;
        } else if (type == 5) {
            memcpy(destination, (void *)value, o->symbols[sym].size);
        } else {
            die("unsupported relocation");
        }
    }
}
static void protect(struct object *o) {
    for (unsigned i = 0; i < o->phnum; i++) {
        struct program *p = &o->ph[i];
        if (p->type != 1 || !p->memsz) {
            continue;
        }
        uint64_t start = (o->base + p->vaddr) & ~4095UL,
                 end = (o->base + p->vaddr + p->memsz + 4095) & ~4095UL;
        int flags = (p->flags & 4 ? PROT_READ : 0) | (p->flags & 2 ? PROT_WRITE : 0) |
                    (p->flags & 1 ? PROT_EXEC : 0);
        if (mprotect((void *)start, end - start, flags)) {
            die("shared protection failed");
        }
    }
    for (unsigned i = 0; i < o->phnum; i++) {
        if (o->ph[i].type == 0x6474e552) {
            uint64_t start = (o->base + o->ph[i].vaddr) & ~4095UL,
                     end = (o->base + o->ph[i].vaddr + o->ph[i].memsz) & ~4095UL;
            if (end > start && mprotect((void *)start, end - start, PROT_READ)) {
                die("RELRO failed");
            }
        }
    }
}
static void initialize(struct object *o, int argc, char **argv) {
    if (o->initialized || o->initializing) {
        return;
    }
    o->initializing = true;
    for (struct dynamic *d = o->dynamic; d->tag; d++) {
        if (d->tag == 1) {
            for (unsigned i = 1; i < object_count; i++) {
                if (!strcmp(objects[i].name, o->strings + d->value)) {
                    initialize(&objects[i], argc, argv);
                }
            }
        }
    }
    if (o->init) {
        ((void (*)(int, char **, char **))o->init)(argc, argv, environ);
    }
    for (unsigned i = 0; i < o->init_count; i++) {
        if (o->init_array[i] && o->init_array[i] != UINT64_MAX) {
            ((void (*)(int, char **, char **))o->init_array[i])(argc, argv, environ);
        }
    }
    o->initialized = true;
}
static void dl_fini(void) {
    for (unsigned n = object_count; n; n--) {
        struct object *o = &objects[n - 1];
        if (!o->initialized) {
            continue;
        }
        uint64_t offset = dynamic_value(o, 26), count = dynamic_value(o, 28) / 8;
        uint64_t *functions = (void *)(o->base + offset);
        if (offset) {
            while (count) {
                uint64_t f = functions[--count];
                if (f && f != UINT64_MAX) {
                    ((void (*)(void))f)();
                }
            }
        }
        offset = dynamic_value(o, 13);
        if (offset) {
            ((void (*)(void))(o->base + offset))();
        }
    }
}
uint64_t dl_start(uint64_t *stack) {
    uint64_t argc = *stack, *aux = stack + argc + 2;
    while (*aux) {
        aux++;
    }
    aux++;
    uint64_t base = 0, phdr = 0, phnum = 0, entry = 0;
    for (uint64_t *a = aux; *a; a += 2) {
        if (a[0] == 7) {
            base = a[1];
        }
        if (a[0] == 3) {
            phdr = a[1];
        }
        if (a[0] == 5) {
            phnum = a[1];
        }
        if (a[0] == 9) {
            entry = a[1];
        }
    }
    uint64_t rela_address = 0, rela_size = 0;
    for (struct dynamic *d = _DYNAMIC; d->tag; d++) {
        if (d->tag == 7) {
            rela_address = base + d->value;
        }
        if (d->tag == 8) {
            rela_size = d->value;
        }
    }
    struct rela *self = (void *)rela_address;
    for (size_t i = 0; i < rela_size / sizeof(*self); i++) {
        if ((uint32_t)self[i].info == 8) {
            *(uint64_t *)(base + self[i].offset) = base + self[i].addend;
        }
    }
    environ = (char **)(stack + argc + 2);
    struct object *main = &objects[0];
    object_count = 1;
    main->ph = (void *)phdr;
    main->phnum = phnum;
    for (unsigned i = 0; i < phnum; i++) {
        if (main->ph[i].type == 6) {
            main->base = phdr - main->ph[i].vaddr;
        }
    }
    inspect(main);
    dependencies(main);
    for (unsigned i = object_count; i; i--) {
        struct object *o = &objects[i - 1];
        relocate_table(o, o->rela, o->rela_count);
        relocate_table(o, o->plt, o->plt_count);
        protect(o);
    }
    for (unsigned i = 0; i < object_count; i++) {
        struct symbol *s = lookup(&objects[i], "atexit");
        if (s) {
            int (*register_exit)(void (*)(void)) = (void *)(objects[i].base + s->value);
            if (register_exit(dl_fini)) {
                die("dynamic finalizer registration failed");
            }
            break;
        }
    }
    initialize(main, argc, (char **)(stack + 1));
    return entry;
}
