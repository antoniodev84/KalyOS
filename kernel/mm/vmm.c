#include "kaly.h"
#define ADDR 0x000ffffffffff000UL
uint64_t vm_create(void) {
    uint64_t p = pmm_alloc(), pdp = pmm_alloc();
    if (!p || !pdp) {
        if (p) {
            pmm_free(p);
        }
        if (pdp) {
            pmm_free(pdp);
        }
        return 0;
    }
    uint64_t *root = phys(p), *boot = boot_pml4;
    root[256] = boot[256];
    root[0] = pdp | PTE_P | PTE_W | PTE_U;
    uint64_t *bp = phys(boot[0] & ADDR);
    ((uint64_t *)phys(pdp))[0] = bp[0] & ~PTE_U;
    return p;
}
static uint64_t *walk(uint64_t root, uint64_t va, bool create) {
    uint64_t *table = phys(root);
    for (int shift = 39; shift >= 21; shift -= 9) {
        unsigned i = (va >> shift) & 511;
        if (!(table[i] & PTE_P)) {
            if (!create) {
                return NULL;
            }
            uint64_t p = pmm_alloc();
            if (!p) {
                return NULL;
            }
            table[i] = p | PTE_P | PTE_W | PTE_U;
        }
        if (table[i] & 128) {
            return NULL;
        }
        table = phys(table[i] & ADDR);
    }
    return &table[(va >> 12) & 511];
}
int vm_map(uint64_t root, uint64_t va, uint64_t pa, uint64_t flags) {
    if (va < USER_MIN || va >= USER_TOP || va % PAGE || pa % PAGE) {
        return -EINVAL;
    }
    uint64_t *pte = walk(root, va, true);
    if (!pte) {
        return -ENOMEM;
    }
    if (*pte & PTE_P) {
        return -EEXIST;
    }
    *pte = pa | (flags & ~512UL) | PTE_P | (flags & 512 ? 0 : PTE_U);
    return 0;
}
uint64_t vm_lookup(uint64_t root, uint64_t va, uint64_t *flags) {
    if (va < USER_MIN || va >= USER_TOP) {
        return 0;
    }
    uint64_t *pte = walk(root, va, false);
    if (!pte || !(*pte & PTE_P)) {
        return 0;
    }
    if (flags) {
        *flags = *pte & ~ADDR;
    }
    return (*pte & ADDR) | (va & (PAGE - 1));
}
int vm_alloc(uint64_t root, uint64_t va, size_t n, uint64_t flags) {
    if (va % PAGE || va < USER_MIN || n > USER_TOP - va) {
        return -EINVAL;
    }
    size_t done = 0;
    for (; done < n; done += PAGE) {
        uint64_t p = pmm_alloc();
        if (!p) {
            break;
        }
        int e = vm_map(root, va + done, p, flags);
        if (e) {
            pmm_free(p);
            break;
        }
    }
    if (done < n) {
        vm_unmap(root, va, done);
        return -ENOMEM;
    }
    return 0;
}
static void prune(uint64_t root, uint64_t va) {
    uint64_t *tables[4], entries[3];
    tables[0] = phys(root);
    for (unsigned level = 0; level < 3; level++) {
        entries[level] = (va >> (39 - level * 9)) & 511;
        uint64_t value = tables[level][entries[level]];
        if (!(value & PTE_P) || (value & 128)) {
            return;
        }
        tables[level + 1] = phys(value & ADDR);
    }
    for (unsigned level = 3; level > 0; level--) {
        bool empty = true;
        for (unsigned i = 0; i < 512; i++) {
            if (tables[level][i]) {
                empty = false;
                break;
            }
        }
        if (!empty) {
            break;
        }
        uint64_t value = tables[level - 1][entries[level - 1]];
        tables[level - 1][entries[level - 1]] = 0;
        pmm_free(value & ADDR);
    }
}
void vm_unmap(uint64_t root, uint64_t va, size_t n) {
    for (size_t i = 0; i < n; i += PAGE) {
        uint64_t *pte = walk(root, va + i, false);
        if (!pte || !(*pte & PTE_P)) {
            continue;
        }
        uint64_t page = *pte & ADDR;
        *pte = 0;
        __asm__ volatile("invlpg (%0)" ::"r"(va + i) : "memory");
        pmm_free(page);
        prune(root, va + i);
    }
}
static void destroy_table(uint64_t p, int level, bool first) {
    uint64_t *t = phys(p);
    for (unsigned i = 0; i < 512; i++) {
        if (!(t[i] & PTE_P)) {
            continue;
        }
        if (level == 4 && i == 256) {
            continue;
        }
        if (level == 3 && first && i == 0) {
            continue;
        }
        if (level == 1) {
            pmm_free(t[i] & ADDR);
        } else {
            destroy_table(t[i] & ADDR, level - 1, first && i == 0);
        }
    }
    pmm_free(p);
}
void vm_destroy(uint64_t root) {
    if (root) {
        destroy_table(root, 4, true);
    }
}
static int clone_table(uint64_t dst, uint64_t src, int level, uint64_t base) {
    uint64_t *s = phys(src);
    for (unsigned i = 0; i < 512; i++) {
        if (!(s[i] & PTE_P)) {
            continue;
        }
        uint64_t va = base | ((uint64_t)i << (12 + (level - 1) * 9));
        if (va >= USER_TOP || (level == 3 && va < USER_MIN)) {
            continue;
        }
        if (level != 1) {
            if (clone_table(dst, s[i] & ADDR, level - 1, va)) {
                return -ENOMEM;
            }
            continue;
        }
        uint64_t p = pmm_alloc();
        if (!p) {
            return -ENOMEM;
        }
        memcpy(phys(p), phys(s[i] & ADDR), PAGE);
        if (vm_map(dst, va, p, (s[i] & ~ADDR) | (!(s[i] & PTE_U) ? 512 : 0))) {
            pmm_free(p);
            return -ENOMEM;
        }
    }
    return 0;
}
uint64_t vm_clone(uint64_t src) {
    uint64_t dst = vm_create();
    if (dst && clone_table(dst, src, 4, 0)) {
        vm_destroy(dst);
        dst = 0;
    }
    return dst;
}
int user_range(uint64_t va, size_t n, bool write) {
    if (va < USER_MIN || va >= USER_TOP || n > USER_TOP - va) {
        return -EFAULT;
    }
    uint64_t root = this_cpu()->current->cr3;
    uint64_t end = va + n;
    for (uint64_t p = va & ~(PAGE - 1); p < end; p += PAGE) {
        uint64_t f;
        if (!vm_lookup(root, p, &f) || !(f & PTE_U) || (write && !(f & PTE_W))) {
            return -EFAULT;
        }
    }
    return 0;
}
static int copy_user(void *kernel, uint64_t user, size_t n, bool write) {
    if (user_range(user, n, write)) {
        return -EFAULT;
    }
    uint64_t root = this_cpu()->current->cr3;
    for (size_t done = 0; done < n;) {
        size_t chunk = MIN(n - done, PAGE - ((user + done) % PAGE));
        void *p = phys(vm_lookup(root, user + done, NULL));
        if (write) {
            memcpy(p, (uint8_t *)kernel + done, chunk);
        } else {
            memcpy((uint8_t *)kernel + done, p, chunk);
        }
        done += chunk;
    }
    return 0;
}
int copy_from_user(void *d, uint64_t s, size_t n) {
    return copy_user(d, s, n, false);
}
int copy_to_user(uint64_t d, const void *s, size_t n) {
    return copy_user((void *)s, d, n, true);
}
int user_string(char *d, uint64_t s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (copy_from_user(d + i, s + i, 1)) {
            return -EFAULT;
        }
        if (!d[i]) {
            return 0;
        }
    }
    return -EINVAL;
}
int vm_protect(uint64_t root, uint64_t va, size_t n, uint64_t flags) {
    for (size_t i = 0; i < n; i += PAGE) {
        if (!walk(root, va + i, false) || !vm_lookup(root, va + i, NULL)) {
            return -ENOMEM;
        }
    }
    for (size_t i = 0; i < n; i += PAGE) {
        uint64_t *p = walk(root, va + i, false);
        *p = (*p & ADDR) | (flags & ~512UL) | PTE_P | (flags & 512 ? 0 : PTE_U);
        __asm__ volatile("invlpg (%0)" ::"r"(va + i) : "memory");
    }
    return 0;
}
