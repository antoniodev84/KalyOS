#include "net.h"
static struct ram_page *ram_page(struct node *n, uint64_t index, bool create) {
    for (struct ram_page *p = n->pages; p; p = p->next) {
        if (p->index == index) {
            return p;
        }
    }
    if (!create) {
        return NULL;
    }
    struct ram_page *p = kalloc(sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->index = index;
    p->next = n->pages;
    n->pages = p;
    return p;
}
static size_t append(char *b, size_t off, const char *s) {
    size_t len = strlen(s);
    if (off + len >= PAGE) {
        return off;
    }
    memcpy(b + off, s, len);
    return off + len;
}
static size_t number(char *b, size_t off, uint64_t v) {
    char tmp[32];
    unsigned n = 0;
    do {
        tmp[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (n && off < PAGE - 1) {
        b[off++] = tmp[--n];
    }
    return off;
}
static size_t proc_data(struct node *n, char *b) {
    size_t off = 0;
    if (n->device == 17) {
        return hda_status(b, PAGE);
    }
    if (n->device == 16) {
        return net_status(b, PAGE);
    }
    if (n->device == 10) {
        off = append(b, off, "MemAvailable: ");
        off = number(b, off, pmm_available() / 1024);
        off = append(b, off, " kB\n");
    } else if (n->device == 11) {
        for (unsigned i = 0; i < cpu_count; i++) {
            off = append(b, off, "processor: ");
            off = number(b, off, i);
            off = append(b, off, " apic: ");
            off = number(b, off, cpus[i].apic_id);
            off = append(b, off, " ticks: ");
            off = number(b, off, cpus[i].ticks);
            off = append(b, off, "\n");
        }
    } else if (n->device == 12) {
        off = number(b, off, ticks / 100);
        off = append(b, off, "\n");
    } else if (n->device == 13) {
        off = append(b, off, "PID PPID UID STATE CPU_TICKS NAME\n");
        for (unsigned i = 0; i < MAX_PROC && off < PAGE - 256; i++) {
            struct process *p = &processes[i];
            if (!p->state) {
                continue;
            }
            off = number(b, off, p->pid);
            off = append(b, off, " ");
            off = number(b, off, p->ppid);
            off = append(b, off, " ");
            off = number(b, off, p->uid);
            off = append(b, off, " ");
            off = number(b, off, p->state);
            off = append(b, off, " ");
            for (unsigned c = 0; c < cpu_count; c++) {
                off = number(b, off, p->runtime[c]);
                off = append(b, off, c + 1 == cpu_count ? " " : ",");
            }
            off = append(b, off, p->name);
            off = append(b, off, "\n");
        }
    } else if (n->device == 14) {
        off = append(b, off, "0-");
        off = number(b, off, cpu_count - 1);
        off = append(b, off, "\n");
    } else if (n->device == 15) {
        for (struct blockdev *d = block_devices; d; d = d->next) {
            off = append(b, off, d->name);
            off = append(b, off, " ");
            off = number(b, off, d->sectors * 512);
            off = append(b, off, "\n");
        }
    }
    return off;
}
struct blockdev *vfs_block(struct node *n) {
    for (struct blockdev *d = block_devices; d; d = d->next) {
        if (!strcmp(d->name, n->name)) {
            return d;
        }
    }
    return NULL;
}
long node_read(struct node *n, uint64_t off, void *buf, size_t len) {
    n = node_actual(n);
    if (n->socket) {
        return net_socket_read(n->socket, buf, len);
    }
    if (n->ext) {
        return ext2_read(n, off, buf, len);
    }
    if ((n->mode & S_IFMT) == S_IFIFO) {
        if (!n->size) {
            return n->writers ? -EAGAIN : 0;
        }
        len = MIN(len, n->size);
        for (size_t i = 0; i < len; i++) {
            ((uint8_t *)buf)[i] = n->pages->bytes[n->read_position++ % PAGE];
        }
        n->size -= len;
        return len;
    }
    if ((n->mode & S_IFMT) == S_IFCHR) {
        if (n->device == 1) {
            return console_read(buf, len);
        }
        if (n->device == 4) {
            return input_read(buf, len);
        }
        if (n->device == 3) {
            memset(buf, 0, len);
            return len;
        }
        return 0;
    }
    if ((n->mode & S_IFMT) == S_IFBLK) {
        struct blockdev *d = vfs_block(n);
        if (!d || off % 512 || len % 512) {
            return -EINVAL;
        }
        int e = block_read(d, off / 512, len / 512, buf);
        return e ? e : (long)len;
    }
    if (n->device >= 10) {
        char *data = kalloc(PAGE);
        if (!data) {
            return -ENOMEM;
        }
        size_t size = proc_data(n, data), count = off < size ? MIN(len, size - off) : 0;
        memcpy(buf, data + MIN(off, size), count);
        kfree(data);
        return count;
    }
    if (off >= n->size) {
        return 0;
    }
    len = MIN(len, n->size - off);
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        size_t chunk = MIN(len - done, PAGE - pos % PAGE);
        struct ram_page *p = ram_page(n, pos / PAGE, false);
        if (p) {
            memcpy((uint8_t *)buf + done, p->bytes + pos % PAGE, chunk);
        } else {
            memset((uint8_t *)buf + done, 0, chunk);
        }
        done += chunk;
    }
    return done;
}
long node_write(struct node *n, uint64_t off, const void *buf, size_t len) {
    n = node_actual(n);
    if (n->socket) {
        return net_socket_write(n->socket, buf, len);
    }
    if (n->ext) {
        return ext2_write(n, off, buf, len);
    }
    if ((n->mode & S_IFMT) == S_IFIFO) {
        if (!n->readers) {
            return -32;
        }
        if (n->size == PAGE) {
            return -EAGAIN;
        }
        len = MIN(len, PAGE - n->size);
        for (size_t i = 0; i < len; i++) {
            n->pages->bytes[n->write_position++ % PAGE] = ((const uint8_t *)buf)[i];
        }
        n->size += len;
        return len;
    }
    if ((n->mode & S_IFMT) == S_IFCHR) {
        if (n->device == 5) {
            return hda_write(buf, len);
        }
        if (n->device == 1) {
            console_write(buf, len);
        }
        return len;
    }
    if ((n->mode & S_IFMT) == S_IFBLK) {
        struct blockdev *d = vfs_block(n);
        if (!d || off % 512 || len % 512) {
            return -EINVAL;
        }
        int e = block_write(d, off / 512, len / 512, buf);
        return e ? e : (long)len;
    }
    if (off > MAX_FILE || len > MAX_FILE - off) {
        return -EFBIG;
    }
    if (n->device >= 10) {
        return -EACCES;
    }
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        size_t chunk = MIN(len - done, PAGE - pos % PAGE);
        struct ram_page *p = ram_page(n, pos / PAGE, true);
        if (!p) {
            return done ? (long)done : -ENOMEM;
        }
        memcpy(p->bytes + pos % PAGE, (const uint8_t *)buf + done, chunk);
        done += chunk;
        n->size = MAX(n->size, off + done);
    }
    return done;
}
int node_truncate(struct node *n) {
    n = node_actual(n);
    if (n->ext) {
        return ext2_truncate(n);
    }
    while (n->pages) {
        struct ram_page *p = n->pages;
        n->pages = p->next;
        kfree(p);
    }
    n->size = 0;
    return 0;
}
