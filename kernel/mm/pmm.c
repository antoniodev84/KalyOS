#include "kaly.h"
#define MAX_PAGES (1UL << 20)
static uint8_t bitmap[MAX_PAGES / 8];
static size_t page_count, cursor;
static uint64_t free_pages;
static struct lock pmm_lock;
extern char kernel_end[];
static bool used(size_t p) {
    return bitmap[p / 8] & (1 << (p % 8));
}
static void mark(size_t p, bool busy) {
    bool old = used(p);
    if (old == busy) {
        return;
    }
    if (busy) {
        bitmap[p / 8] |= 1 << (p % 8);
        free_pages--;
    } else {
        bitmap[p / 8] &= ~(1 << (p % 8));
        free_pages++;
    }
}
static void reserve(uint64_t start, uint64_t end) {
    for (size_t p = start / PAGE; p < MIN(ALIGN_UP(end, PAGE) / PAGE, page_count); p++) {
        mark(p, true);
    }
}
void pmm_init(uint64_t info) {
    uint32_t total = *(uint32_t *)info;
    memset(bitmap, 255, sizeof(bitmap));
    for (uint64_t t = info + 8; t < info + total; t += ALIGN_UP(*(uint32_t *)(t + 4), 8)) {
        uint32_t type = *(uint32_t *)t, size = *(uint32_t *)(t + 4);
        if (!type) {
            break;
        }
        if (type != 6) {
            continue;
        }
        uint32_t step = *(uint32_t *)(t + 8);
        for (uint64_t e = t + 16; e + 24 <= t + size; e += step) {
            uint64_t start = *(uint64_t *)e, len = *(uint64_t *)(e + 8);
            if (*(uint32_t *)(e + 16) != 1) {
                continue;
            }
            uint64_t end = MIN(start + len, MAX_PAGES * PAGE);
            page_count = MAX(page_count, end / PAGE);
            for (size_t p = ALIGN_UP(start, PAGE) / PAGE; p < end / PAGE; p++) {
                mark(p, false);
            }
        }
    }
    reserve(0, (uint64_t)kernel_end);
    reserve(info, info + total);
    for (uint64_t t = info + 8; t < info + total; t += ALIGN_UP(*(uint32_t *)(t + 4), 8)) {
        if (!*(uint32_t *)t) {
            break;
        }
        if (*(uint32_t *)t == 3) {
            reserve(*(uint32_t *)(t + 8), *(uint32_t *)(t + 12));
        }
    }
    cursor = ALIGN_UP((uint64_t)kernel_end, PAGE) / PAGE;
    if (!free_pages) {
        panic("memory map");
    }
}
uint64_t pmm_alloc_contiguous(size_t n) {
    if (!n) {
        return 0;
    }
    lock(&pmm_lock);
    size_t start = cursor, run = 0;
    for (size_t i = 0; i < page_count; i++) {
        size_t p = (start + i) % page_count;
        if (!p) {
            run = 0;
        }
        if (used(p)) {
            run = 0;
            continue;
        }
        if (++run != n) {
            continue;
        }
        size_t first = p + 1 - n;
        for (size_t j = first; j <= p; j++) {
            mark(j, true);
        }
        cursor = (p + 1) % page_count;
        unlock(&pmm_lock);
        memset(phys(first * PAGE), 0, n * PAGE);
        return first * PAGE;
    }
    unlock(&pmm_lock);
    return 0;
}
uint64_t pmm_alloc(void) {
    return pmm_alloc_contiguous(1);
}
void pmm_free(uint64_t p) {
    if (!p || p % PAGE || p / PAGE >= page_count) {
        panic("pmm free");
    }
    lock(&pmm_lock);
    if (!used(p / PAGE)) {
        panic("double free");
    }
    mark(p / PAGE, false);
    unlock(&pmm_lock);
}
uint64_t pmm_available(void) {
    return __atomic_load_n(&free_pages, __ATOMIC_RELAXED) * PAGE;
}
void *kalloc(size_t n) {
    if (n > SIZE_MAX - 16 - PAGE) {
        return NULL;
    }
    size_t pages = ALIGN_UP(n + 16, PAGE) / PAGE;
    uint64_t p = pmm_alloc_contiguous(pages);
    if (!p) {
        return NULL;
    }
    uint64_t *h = phys(p);
    h[0] = pages;
    return h + 2;
}
void kfree(void *v) {
    if (!v) {
        return;
    }
    uint64_t *h = (uint64_t *)v - 2;
    uint64_t p = physical(h), n = h[0];
    for (size_t i = 0; i < n; i++) {
        pmm_free(p + i * PAGE);
    }
}
