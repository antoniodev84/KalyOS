#include "kaly.h"
struct ext_group {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks, free_inodes, directories, pad;
    uint32_t reserved[3];
} PACKED;
struct ext_inode {
    uint16_t mode, uid;
    uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links;
    uint32_t sectors, flags, os;
    uint32_t blocks[15];
    uint32_t generation, acl, size_high, fragment;
    uint8_t extra[12];
} PACKED;
struct ext_dir {
    uint32_t ino;
    uint16_t length;
    uint8_t name_length, type;
    char name[];
} PACKED;
struct extfs {
    struct blockdev *dev;
    struct node *nodes;
    uint8_t super[1024];
    struct ext_group *groups;
    uint32_t block_size, group_count, inode_size, blocks_per_group, inodes_per_group, first_block,
        total_blocks, total_inodes;
};
#define SB32(f, o) (*(uint32_t *)((f)->super + (o)))
#define SB16(f, o) (*(uint16_t *)((f)->super + (o)))
static int read_block(struct extfs *f, uint32_t b, void *p) {
    if (b >= f->total_blocks) {
        return -EIO;
    }
    return block_read(f->dev, (uint64_t)b * (f->block_size / 512), f->block_size / 512, p);
}
static int write_block(struct extfs *f, uint32_t b, const void *p) {
    if (b >= f->total_blocks) {
        return -EIO;
    }
    return block_write(f->dev, (uint64_t)b * (f->block_size / 512), f->block_size / 512, p);
}
static int inode_io(struct extfs *f, uint32_t ino, struct ext_inode *i, bool write) {
    if (!ino || ino > f->total_inodes) {
        return -EIO;
    }
    uint32_t group = (ino - 1) / f->inodes_per_group;
    uint64_t off = (uint64_t)((ino - 1) % f->inodes_per_group) * f->inode_size;
    uint32_t block = f->groups[group].inode_table + off / f->block_size;
    uint8_t *buf = kalloc(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    int e = read_block(f, block, buf);
    if (!e) {
        if (write) {
            memcpy(buf + off % f->block_size, i, sizeof(*i));
            e = write_block(f, block, buf);
        } else {
            memcpy(i, buf + off % f->block_size, sizeof(*i));
        }
    }
    kfree(buf);
    return e;
}
static int sync_counts(struct extfs *f, uint32_t group) {
    int e = block_write(f->dev, 2, 2, f->super);
    if (e) {
        return e;
    }
    uint32_t table = f->first_block + 1;
    uint64_t off = (uint64_t)group * sizeof(struct ext_group);
    uint8_t *buf = kalloc(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    e = read_block(f, table + off / f->block_size, buf);
    if (!e) {
        memcpy(buf + off % f->block_size, &f->groups[group], sizeof(struct ext_group));
        e = write_block(f, table + off / f->block_size, buf);
    }
    kfree(buf);
    return e;
}
static int bitmap_change(struct extfs *f, uint32_t value, bool inode, bool allocate) {
    uint32_t base = inode ? value - 1 : value - f->first_block;
    uint32_t per = inode ? f->inodes_per_group : f->blocks_per_group;
    uint32_t group = base / per, bit = base % per;
    if (group >= f->group_count) {
        return -EIO;
    }
    struct ext_group *g = &f->groups[group];
    uint32_t block = inode ? g->inode_bitmap : g->block_bitmap;
    uint8_t *buf = kalloc(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    int e = read_block(f, block, buf);
    if (!e) {
        bool busy = buf[bit / 8] & (1 << (bit % 8));
        if (busy == allocate) {
            e = -EIO;
        } else {
            if (allocate) {
                buf[bit / 8] |= 1 << (bit % 8);
            } else {
                buf[bit / 8] &= ~(1 << (bit % 8));
            }
            e = write_block(f, block, buf);
            if (!e) {
                int change = allocate ? -1 : 1;
                if (inode) {
                    g->free_inodes += change;
                    SB32(f, 16) += change;
                } else {
                    g->free_blocks += change;
                    SB32(f, 12) += change;
                }
                e = sync_counts(f, group);
            }
        }
    }
    kfree(buf);
    return e;
}
static int allocate(struct extfs *f, bool inode, uint32_t *result) {
    uint32_t per = inode ? f->inodes_per_group : f->blocks_per_group;
    uint8_t *buf = kalloc(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    int e = -ENOSPC;
    for (uint32_t group = 0; group < f->group_count; group++) {
        struct ext_group *g = &f->groups[group];
        if (!(inode ? g->free_inodes : g->free_blocks)) {
            continue;
        }
        e = read_block(f, inode ? g->inode_bitmap : g->block_bitmap, buf);
        if (e) {
            break;
        }
        uint32_t total = inode ? f->total_inodes : f->total_blocks - f->first_block;
        uint32_t count = MIN(per, total - group * per);
        for (uint32_t b = 0; b < count; b++) {
            if (buf[b / 8] & (1 << (b % 8))) {
                continue;
            }
            uint32_t value = group * per + b + (inode ? 1 : f->first_block);
            e = bitmap_change(f, value, inode, true);
            if (!e && !inode) {
                memset(buf, 0, f->block_size);
                e = write_block(f, value, buf);
                if (e) {
                    bitmap_change(f, value, false, false);
                }
            }
            if (!e) {
                *result = value;
            }
            kfree(buf);
            return e;
        }
        e = -ENOSPC;
    }
    kfree(buf);
    return e;
}
static uint64_t inode_size(const struct ext_inode *i) {
    return i->size | ((i->mode & S_IFMT) == S_IFREG ? (uint64_t)i->size_high << 32 : 0);
}
static void set_size(struct ext_inode *i, uint64_t n) {
    i->size = n;
    if ((i->mode & S_IFMT) == S_IFREG) {
        i->size_high = n >> 32;
    }
}
static int data_block(struct extfs *f, uint32_t ino, struct ext_inode *i, uint64_t index,
                      bool create, uint32_t *result) {
    uint64_t per = f->block_size / 4;
    unsigned depth = 0, slot;
    uint32_t positions[3];
    if (index < 12) {
        slot = index;
    } else {
        index -= 12;
        if (index < per) {
            slot = 12;
            depth = 1;
            positions[0] = index;
        } else {
            index -= per;
            if (index < per * per) {
                slot = 13;
                depth = 2;
                positions[0] = index / per;
                positions[1] = index % per;
            } else {
                index -= per * per;
                if (index >= per * per * per) {
                    return -EFBIG;
                }
                slot = 14;
                depth = 3;
                positions[0] = index / (per * per);
                positions[1] = index / per % per;
                positions[2] = index % per;
            }
        }
    }
    uint32_t block = i->blocks[slot];
    int e;
    if (!block && create) {
        if ((e = allocate(f, false, &block))) {
            return e;
        }
        i->blocks[slot] = block;
        i->sectors += f->block_size / 512;
        if ((e = inode_io(f, ino, i, true))) {
            return e;
        }
    }
    if (!block || !depth) {
        *result = block;
        return 0;
    }
    uint32_t *buf = kalloc(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    for (unsigned d = 0; d < depth; d++) {
        e = read_block(f, block, buf);
        if (e) {
            kfree(buf);
            return e;
        }
        uint32_t next = buf[positions[d]];
        if (!next && create) {
            e = allocate(f, false, &next);
            if (e) {
                kfree(buf);
                return e;
            }
            buf[positions[d]] = next;
            e = write_block(f, block, buf);
            if (e) {
                bitmap_change(f, next, false, false);
                kfree(buf);
                return e;
            }
            i->sectors += f->block_size / 512;
            e = inode_io(f, ino, i, true);
            if (e) {
                kfree(buf);
                return e;
            }
        }
        block = next;
        if (!block) {
            break;
        }
    }
    kfree(buf);
    *result = block;
    return 0;
}
void ext2_cache(struct node *n) {
    n->filesystem = n->parent && n->parent->ext == n->ext ? n->parent->filesystem : n;
    for (struct node *p = n->ext->nodes; p; p = p->cache_next) {
        if (p->ino == n->ino && !p->origin) {
            n->origin = p;
            break;
        }
    }
    n->cache_next = n->ext->nodes;
    n->ext->nodes = n;
}
int ext2_refresh(struct node *n) {
    struct ext_inode i;
    int e = inode_io(n->ext, n->ino, &i, false);
    if (e) {
        return e;
    }
    n->mode = i.mode;
    n->uid = i.uid;
    n->gid = i.gid;
    n->links = i.links;
    n->size = inode_size(&i);
    return 0;
}
long ext2_read(struct node *n, uint64_t offset, void *out, size_t len) {
    struct extfs *f = n->ext;
    struct ext_inode i;
    int e = inode_io(f, n->ino, &i, false);
    if (e) {
        return e;
    }
    uint64_t size = inode_size(&i);
    if (offset >= size) {
        return 0;
    }
    len = MIN(len, size - offset);
    if ((i.mode & S_IFMT) == S_IFLNK && size <= 60 && !i.sectors) {
        memcpy(out, (uint8_t *)i.blocks + offset, len);
        return len;
    }
    uint8_t *buf = kalloc(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    size_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        size_t chunk = MIN(len - done, f->block_size - pos % f->block_size);
        uint32_t block;
        e = data_block(f, n->ino, &i, pos / f->block_size, false, &block);
        if (e) {
            break;
        }
        if (block) {
            e = read_block(f, block, buf);
        } else {
            memset(buf, 0, f->block_size);
        }
        if (e) {
            break;
        }
        memcpy((uint8_t *)out + done, buf + pos % f->block_size, chunk);
        done += chunk;
    }
    kfree(buf);
    return done ? (long)done : e;
}
long ext2_write(struct node *n, uint64_t offset, const void *input, size_t len) {
    if (offset > MAX_FILE || len > MAX_FILE - offset) {
        return -EFBIG;
    }
    struct extfs *f = n->ext;
    struct ext_inode i;
    int e = inode_io(f, n->ino, &i, false);
    if (e) {
        return e;
    }
    uint8_t *buf = kalloc(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    size_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        size_t chunk = MIN(len - done, f->block_size - pos % f->block_size);
        uint32_t block;
        e = data_block(f, n->ino, &i, pos / f->block_size, true, &block);
        if (e) {
            break;
        }
        e = read_block(f, block, buf);
        if (e) {
            break;
        }
        memcpy(buf + pos % f->block_size, (const uint8_t *)input + done, chunk);
        e = write_block(f, block, buf);
        if (e) {
            break;
        }
        done += chunk;
        if (offset + done > inode_size(&i)) {
            set_size(&i, offset + done);
        }
        i.mtime = i.ctime = kernel_time();
        e = inode_io(f, n->ino, &i, true);
        if (e) {
            break;
        }
    }
    n->size = inode_size(&i);
    kfree(buf);
    return e ? (done ? (long)done : e) : (long)done;
}
static int free_tree(struct extfs *f, uint32_t block, unsigned depth) {
    if (!block) {
        return 0;
    }
    if (depth) {
        uint32_t *buf = kalloc(f->block_size);
        if (!buf) {
            return -ENOMEM;
        }
        int e = read_block(f, block, buf);
        if (!e) {
            for (unsigned i = 0; i < f->block_size / 4; i++) {
                e = free_tree(f, buf[i], depth - 1);
                if (e) {
                    break;
                }
            }
        }
        kfree(buf);
        if (e) {
            return e;
        }
    }
    return bitmap_change(f, block, false, false);
}
int ext2_truncate(struct node *n) {
    struct ext_inode i;
    int e = inode_io(n->ext, n->ino, &i, false);
    if (e) {
        return e;
    }
    if ((i.mode & S_IFMT) != S_IFLNK || i.sectors) {
        for (unsigned j = 0; j < 15; j++) {
            e = free_tree(n->ext, i.blocks[j], j < 12 ? 0 : j - 11);
            if (e) {
                return e;
            }
            i.blocks[j] = 0;
        }
    } else {
        memset(i.blocks, 0, sizeof(i.blocks));
    }
    i.sectors = 0;
    i.mtime = i.ctime = kernel_time();
    set_size(&i, 0);
    n->size = 0;
    return inode_io(n->ext, n->ino, &i, true);
}
static unsigned dir_type(uint32_t mode) {
    switch (mode & S_IFMT) {
    case S_IFREG:
        return 1;
    case S_IFDIR:
        return 2;
    case S_IFLNK:
        return 7;
    default:
        return 0;
    }
}
static int add_dirent(struct node *parent, uint32_t ino, uint32_t mode, const char *name) {
    struct extfs *f = parent->ext;
    size_t len = strlen(name), needed = ALIGN_UP(8 + len, 4);
    uint8_t *buf = kalloc(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    int e = ext2_refresh(parent);
    if (e) {
        kfree(buf);
        return e;
    }
    for (uint64_t off = 0; off <= parent->size; off += f->block_size) {
        if (off == parent->size) {
            memset(buf, 0, f->block_size);
            ((struct ext_dir *)buf)->length = f->block_size;
        } else {
            long r = ext2_read(parent, off, buf, f->block_size);
            if (r != f->block_size) {
                e = r < 0 ? r : -EIO;
                break;
            }
        }
        for (unsigned pos = 0; pos < f->block_size;) {
            struct ext_dir *d = (void *)(buf + pos);
            if (d->length < 8 || d->length % 4 || pos + d->length > f->block_size ||
                d->name_length > d->length - 8) {
                e = -EIO;
                goto end;
            }
            unsigned used = d->ino ? ALIGN_UP(8 + d->name_length, 4) : 0;
            if (d->length - used >= needed) {
                if (used) {
                    unsigned old = d->length;
                    d->length = used;
                    d = (void *)((uint8_t *)d + used);
                    d->length = old - used;
                }
                d->ino = ino;
                d->name_length = len;
                d->type = dir_type(mode);
                memcpy(d->name, name, len);
                long r = ext2_write(parent, off, buf, f->block_size);
                e = r == f->block_size ? 0 : r < 0 ? r : -EIO;
                goto end;
            }
            pos += d->length;
        }
    }
end:
    kfree(buf);
    return e;
}
struct node *ext2_lookup(struct node *parent, const char *name, int *error) {
    struct extfs *f = parent->ext;
    *error = ext2_refresh(parent);
    if (*error) {
        return NULL;
    }
    uint8_t *buf = kalloc(f->block_size);
    if (!buf) {
        *error = -ENOMEM;
        return NULL;
    }
    for (uint64_t off = 0; off < parent->size; off += f->block_size) {
        long r = ext2_read(parent, off, buf, f->block_size);
        if (r != f->block_size) {
            *error = r < 0 ? r : -EIO;
            break;
        }
        for (unsigned pos = 0; pos < f->block_size;) {
            struct ext_dir *d = (void *)(buf + pos);
            if (d->length < 8 || d->length % 4 || pos + d->length > f->block_size ||
                d->name_length > d->length - 8) {
                *error = -EIO;
                goto end;
            }
            if (d->ino && d->name_length == strlen(name) &&
                !memcmp(d->name, name, d->name_length)) {
                struct node *n = kalloc(sizeof(*n));
                if (!n) {
                    *error = -ENOMEM;
                    goto end;
                }
                strcpy(n->name, name);
                n->ino = d->ino;
                n->ext = f;
                n->parent = parent;
                *error = ext2_refresh(n);
                if (*error) {
                    kfree(n);
                    goto end;
                }
                ext2_cache(n);
                n->next = parent->children;
                parent->children = n;
                kfree(buf);
                return n;
            }
            pos += d->length;
        }
    }
    if (!*error) {
        *error = -ENOENT;
    }
end:
    kfree(buf);
    return NULL;
}
int ext2_create(struct node *parent, struct node *n, const char *target) {
    struct extfs *f = parent->ext;
    uint32_t ino;
    int e = allocate(f, true, &ino);
    if (e) {
        return e;
    }
    struct ext_inode i = {0};
    i.mode = n->mode;
    i.uid = n->uid;
    i.gid = n->gid;
    i.links = (n->mode & S_IFMT) == S_IFDIR ? 2 : 1;
    i.atime = i.ctime = i.mtime = kernel_time();
    n->ext = f;
    n->ino = ino;
    n->links = i.links;
    e = inode_io(f, ino, &i, true);
    if (!e && (n->mode & S_IFMT) == S_IFDIR) {
        e = add_dirent(n, ino, S_IFDIR, ".");
        if (!e) {
            e = add_dirent(n, parent->ino, S_IFDIR, "..");
        }
    }
    if (!e && target) {
        size_t len = strlen(target);
        if (len <= 60) {
            memcpy(i.blocks, target, len);
            i.size = len;
            n->size = len;
            e = inode_io(f, ino, &i, true);
        } else {
            long r = ext2_write(n, 0, target, len);
            if (r != (long)len) {
                e = r < 0 ? r : -EIO;
            }
        }
    }
    if (!e) {
        e = add_dirent(parent, ino, n->mode, n->name);
    }
    if (e) {
        ext2_truncate(n);
        bitmap_change(f, ino, true, false);
        return e;
    }
    if ((n->mode & S_IFMT) == S_IFDIR) {
        e = inode_io(f, parent->ino, &i, false);
        if (!e) {
            i.links++;
            e = inode_io(f, parent->ino, &i, true);
        }
        f->groups[(ino - 1) / f->inodes_per_group].directories++;
        if (!e) {
            e = sync_counts(f, (ino - 1) / f->inodes_per_group);
        }
    }
    if (!e) {
        ext2_cache(n);
    }
    return e;
}
int ext2_link(struct node *parent, struct node *n, const char *name) {
    struct ext_inode i;
    int e = inode_io(n->ext, n->ino, &i, false);
    if (e) {
        return e;
    }
    if (i.links == 65535) {
        return -ENOSPC;
    }
    e = add_dirent(parent, n->ino, n->mode, name);
    if (!e) {
        i.links++;
        i.ctime = kernel_time();
        n->links = i.links;
        e = inode_io(n->ext, n->ino, &i, true);
    }
    return e;
}
int ext2_empty(struct node *n) {
    struct extfs *f = n->ext;
    int e = ext2_refresh(n);
    if (e) {
        return e;
    }
    uint8_t *buf = kalloc(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    for (uint64_t off = 0; off < n->size; off += f->block_size) {
        long r = ext2_read(n, off, buf, f->block_size);
        if (r != f->block_size) {
            e = r < 0 ? r : -EIO;
            break;
        }
        for (unsigned pos = 0; pos < f->block_size;) {
            struct ext_dir *d = (void *)(buf + pos);
            if (d->length < 8 || pos + d->length > f->block_size) {
                e = -EIO;
                goto end;
            }
            if (d->ino && !(d->name_length == 1 && d->name[0] == '.') &&
                !(d->name_length == 2 && d->name[0] == '.' && d->name[1] == '.')) {
                e = -ENOTEMPTY;
                goto end;
            }
            pos += d->length;
        }
    }
end:
    kfree(buf);
    return e;
}
int ext2_drop(struct node *n) {
    struct ext_inode i;
    int e = inode_io(n->ext, n->ino, &i, false);
    if (e || i.links) {
        return e;
    }
    e = ext2_truncate(n);
    if (e) {
        return e;
    }
    e = inode_io(n->ext, n->ino, &i, false);
    if (e) {
        return e;
    }
    i.dtime = kernel_time();
    e = inode_io(n->ext, n->ino, &i, true);
    if (!e) {
        e = bitmap_change(n->ext, n->ino, true, false);
    }
    if (!e) {
        uint32_t ino = n->ino;
        for (struct node *p = n->ext->nodes; p; p = p->cache_next) {
            if (p->ino == ino) {
                p->ino = 0;
            }
        }
    }
    return e;
}
int ext2_remove(struct node *parent, struct node *n) {
    struct extfs *f = n->ext;
    uint8_t *buf = kalloc(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    int e = -ENOENT;
    for (uint64_t off = 0; off < parent->size; off += f->block_size) {
        long r = ext2_read(parent, off, buf, f->block_size);
        if (r != f->block_size) {
            e = r < 0 ? r : -EIO;
            break;
        }
        struct ext_dir *previous = NULL;
        for (unsigned pos = 0; pos < f->block_size;) {
            struct ext_dir *d = (void *)(buf + pos);
            if (d->length < 8 || pos + d->length > f->block_size) {
                e = -EIO;
                goto end;
            }
            if (d->ino == n->ino && d->name_length == strlen(n->name) &&
                !memcmp(d->name, n->name, d->name_length)) {
                if (previous) {
                    previous->length += d->length;
                } else {
                    d->ino = 0;
                }
                r = ext2_write(parent, off, buf, f->block_size);
                e = r == f->block_size ? 0 : r < 0 ? r : -EIO;
                goto removed;
            }
            previous = d;
            pos += d->length;
        }
    }
    goto end;
removed:
    if (!e) {
        struct ext_inode i;
        e = inode_io(f, n->ino, &i, false);
        if (!e) {
            i.ctime = kernel_time();
            i.links = (n->mode & S_IFMT) == S_IFDIR ? 0 : i.links - 1;
            n->links = i.links;
            if (n->origin) {
                n->origin->links = i.links;
            }
            e = inode_io(f, n->ino, &i, true);
        }
        if (!e && (n->mode & S_IFMT) == S_IFDIR) {
            e = inode_io(f, parent->ino, &i, false);
            if (!e) {
                i.links--;
                e = inode_io(f, parent->ino, &i, true);
            }
            uint32_t g = (n->ino - 1) / f->inodes_per_group;
            f->groups[g].directories--;
            if (!e) {
                e = sync_counts(f, g);
            }
        }
    }
end:
    kfree(buf);
    return e;
}
int ext2_chmod(struct node *n) {
    struct ext_inode i;
    int e = inode_io(n->ext, n->ino, &i, false);
    if (!e) {
        i.mode = n->mode;
        i.ctime = kernel_time();
        e = inode_io(n->ext, n->ino, &i, true);
    }
    return e;
}
long ext2_getdents(struct node *n, uint64_t *offset, void *output, size_t capacity) {
    struct extfs *f = n->ext;
    uint8_t *buf = kalloc(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    int e = ext2_refresh(n);
    size_t done = 0;
    while (!e && *offset < n->size) {
        uint64_t base = *offset / f->block_size * f->block_size;
        long r = ext2_read(n, base, buf, f->block_size);
        if (r != f->block_size) {
            e = r < 0 ? r : -EIO;
            break;
        }
        struct ext_dir *d = (void *)(buf + *offset % f->block_size);
        if (d->length < 8 || *offset % f->block_size + d->length > f->block_size ||
            d->name_length > d->length - 8) {
            e = -EIO;
            break;
        }
        if (!d->ino) {
            *offset += d->length;
            continue;
        }
        size_t record = ALIGN_UP(19 + d->name_length + 1, 8);
        if (record > capacity - done) {
            if (!done) {
                e = -EINVAL;
            }
            break;
        }
        uint8_t *p = (uint8_t *)output + done;
        memset(p, 0, record);
        *(uint64_t *)p = d->ino;
        *(uint64_t *)(p + 8) = *offset + d->length;
        *(uint16_t *)(p + 16) = record;
        p[18] = d->type == 1 ? 8 : d->type == 2 ? 4 : d->type == 7 ? 10 : 0;
        memcpy(p + 19, d->name, d->name_length);
        *offset += d->length;
        done += record;
    }
    kfree(buf);
    return done ? (long)done : e;
}
int ext2_mount(struct node *n, struct blockdev *dev) {
    if (dev->mounted) {
        return -EBUSY;
    }
    struct extfs *f = kalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    f->dev = dev;
    int e = block_read(dev, 2, 2, f->super);
    if (e) {
        goto fail;
    }
    if (SB16(f, 56) != 0xef53 || SB32(f, 24) > 2 || (SB32(f, 96) & ~2U) || (SB32(f, 100) & ~3U) ||
        (SB32(f, 92) & 4)) {
        e = -EINVAL;
        goto fail;
    }
    f->block_size = 1024U << SB32(f, 24);
    f->total_blocks = SB32(f, 4);
    f->total_inodes = SB32(f, 0);
    f->first_block = SB32(f, 20);
    f->blocks_per_group = SB32(f, 32);
    f->inodes_per_group = SB32(f, 40);
    f->inode_size = SB32(f, 76) ? SB16(f, 88) : 128;
    if (!f->blocks_per_group || !f->inodes_per_group || f->blocks_per_group > f->block_size * 8 ||
        f->inodes_per_group > f->block_size * 8 || f->inode_size < 128 ||
        f->inode_size > f->block_size || f->block_size % f->inode_size ||
        f->first_block >= f->total_blocks ||
        (uint64_t)f->total_blocks * f->block_size > dev->sectors * 512) {
        e = -EINVAL;
        goto fail;
    }
    f->group_count =
        (f->total_blocks - f->first_block + f->blocks_per_group - 1) / f->blocks_per_group;
    if ((f->total_inodes + f->inodes_per_group - 1) / f->inodes_per_group > f->group_count) {
        e = -EINVAL;
        goto fail;
    }
    size_t bytes = ALIGN_UP((uint64_t)f->group_count * sizeof(struct ext_group), f->block_size);
    f->groups = kalloc(bytes);
    if (!f->groups) {
        e = -ENOMEM;
        goto fail;
    }
    e = block_read(dev, (uint64_t)(f->first_block + 1) * f->block_size / 512, bytes / 512,
                   f->groups);
    if (e) {
        goto fail;
    }
    n->ext = f;
    n->ino = 2;
    e = ext2_refresh(n);
    if (e || (n->mode & S_IFMT) != S_IFDIR) {
        if (!e) {
            e = -EINVAL;
        }
        goto fail;
    }
    SB16(f, 58) = 0;
    e = block_write(dev, 2, 2, f->super);
    if (e) {
        goto fail;
    }
    ext2_cache(n);
    dev->mounted = f;
    return 0;
fail:
    n->ext = NULL;
    kfree(f->groups);
    kfree(f);
    return e;
}
int ext2_sync(struct extfs *f) {
    SB16(f, 58) = 1;
    return block_write(f->dev, 2, 2, f->super);
}
void ext2_release(struct extfs *f) {
    f->dev->mounted = NULL;
    while (f->nodes) {
        struct node *n = f->nodes;
        f->nodes = n->cache_next;
        kfree(n);
    }
    kfree(f->groups);
    kfree(f);
}

int ext2_stat(struct node *n, struct kstat *st) {
    struct ext_inode i;
    int e = inode_io(n->ext, n->ino, &i, false);
    if (!e) {
        st->blocks = i.sectors;
        st->blksize = n->ext->block_size;
        st->atime = i.atime;
        st->mtime = i.mtime;
        st->ctime = i.ctime;
    }
    return e;
}
