#include "kaly.h"
struct node *root;
struct lock fs_lock;
static uint32_t next_ino = 1;
struct node *node_actual(struct node *n) {
    return n->origin ? n->origin : n;
}
static struct process *current(void) {
    return this_cpu()->current;
}
struct node *node_new(const char *name, uint32_t mode, struct node *parent) {
    struct node *n = kalloc(sizeof(*n));
    if (!n) {
        return NULL;
    }
    strcpy(n->name, name);
    n->mode = mode;
    n->links = (mode & S_IFMT) == S_IFDIR ? 2 : 1;
    n->ino = next_ino++;
    n->parent = parent;
    n->filesystem = parent ? parent->filesystem : n;
    if (parent) {
        if ((mode & S_IFMT) == S_IFDIR) {
            parent->links++;
        }
        n->next = parent->children;
        parent->children = n;
    }
    return n;
}
int vfs_access(struct node *n, unsigned want) {
    n = node_actual(n);
    if (n->ext) {
        int e = ext2_refresh(n);
        if (e) {
            return e;
        }
    }
    struct process *p = current();
    if (!p || !p->uid) {
        return (want & 1) && (n->mode & S_IFMT) == S_IFREG && !(n->mode & 0111) ? -EACCES : 0;
    }
    unsigned bits = (n->mode >> (n->uid == p->uid ? 6 : n->gid == p->gid ? 3 : 0)) & 7;
    return (bits & want) == want ? 0 : -EACCES;
}
static struct node *resolve(const char *path, struct node *start, bool follow, unsigned depth,
                            int *error) {
    if (depth > 40) {
        *error = -ELOOP;
        return NULL;
    }
    if (!*path) {
        *error = -ENOENT;
        return NULL;
    }
    struct node *n = *path == '/' ? root : start;
    if (!n) {
        n = root;
    }
    const char *p = path;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (n->mounted) {
            n = n->mounted;
        }
        if ((n->mode & S_IFMT) != S_IFDIR) {
            *error = -ENOTDIR;
            return NULL;
        }
        if ((*error = vfs_access(n, 1))) {
            return NULL;
        }
        char name[256];
        size_t len = 0;
        while (*p && *p != '/') {
            if (len == 255) {
                *error = -EINVAL;
                return NULL;
            }
            name[len++] = *p++;
        }
        name[len] = 0;
        if (!strcmp(name, ".")) {
            continue;
        }
        if (!strcmp(name, "..")) {
            if (n->parent) {
                n = n->parent;
            }
            continue;
        }
        struct node *child;
        for (child = n->children; child; child = child->next) {
            if (!strcmp(child->name, name)) {
                break;
            }
        }
        if (!child && n->ext) {
            child = ext2_lookup(n, name, error);
        }
        if (!child) {
            if (!*error) {
                *error = -ENOENT;
            }
            return NULL;
        }
        if (child->ext && (*error = ext2_refresh(child))) {
            return NULL;
        }
        bool remaining = *p != 0;
        if ((child->mode & S_IFMT) == S_IFLNK && (follow || remaining)) {
            char expanded[MAX_PATH];
            long r = node_read(child, 0, expanded, sizeof(expanded) - 1);
            if (r < 0) {
                *error = r;
                return NULL;
            }
            if ((size_t)r + strlen(p) >= sizeof(expanded)) {
                *error = -EINVAL;
                return NULL;
            }
            strcpy(expanded + r, p);
            return resolve(expanded, n, follow, depth + 1, error);
        }
        n = child;
    }
    if (n->mounted) {
        n = n->mounted;
    }
    if (path[strlen(path) - 1] == '/' && (n->mode & S_IFMT) != S_IFDIR) {
        *error = -ENOTDIR;
        return NULL;
    }
    *error = 0;
    return n;
}
struct node *vfs_resolve(const char *path, struct node *start, bool follow, int *error) {
    *error = 0;
    return resolve(path, start, follow, 0, error);
}
static struct node *parent_path(const char *path, struct node *start, char *name, int *error) {
    size_t len = strlen(path);
    if (!len || len >= MAX_PATH) {
        *error = -EINVAL;
        return NULL;
    }
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    size_t split = len;
    while (split && path[split - 1] != '/') {
        split--;
    }
    if (len - split > 255 || len == split) {
        *error = -EINVAL;
        return NULL;
    }
    memcpy(name, path + split, len - split);
    name[len - split] = 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
        *error = -EINVAL;
        return NULL;
    }
    char prefix[MAX_PATH];
    if (!split) {
        strcpy(prefix, ".");
    } else {
        memcpy(prefix, path, split);
        prefix[split] = 0;
    }
    struct node *p = vfs_resolve(prefix, start, true, error);
    if (p && (p->mode & S_IFMT) != S_IFDIR) {
        *error = -ENOTDIR;
        return NULL;
    }
    return p;
}
static struct node *create_at(const char *path, struct node *start, uint32_t mode,
                              const char *target, int *error) {
    if (*path && path[strlen(path) - 1] == '/' && (mode & S_IFMT) != S_IFDIR) {
        *error = -ENOTDIR;
        return NULL;
    }
    char name[256];
    struct node *p = parent_path(path, start, name, error);
    if (!p) {
        return NULL;
    }
    if ((*error = vfs_access(p, 3))) {
        return NULL;
    }
    struct node *old = vfs_resolve(path, start, false, error);
    if (old) {
        *error = -EEXIST;
        return NULL;
    }
    if (*error != -ENOENT) {
        return NULL;
    }
    struct node *n = kalloc(sizeof(*n));
    if (!n) {
        *error = -ENOMEM;
        return NULL;
    }
    strcpy(n->name, name);
    n->parent = p;
    n->filesystem = p->filesystem;
    n->mode = mode;
    n->links = (mode & S_IFMT) == S_IFDIR ? 2 : 1;
    n->ino = next_ino++;
    if (current()) {
        n->uid = current()->uid;
        n->gid = current()->gid;
    }
    *error = p->ext ? ext2_create(p, n, target) : 0;
    if (!*error && !p->ext && target) {
        long r = node_write(n, 0, target, strlen(target));
        if (r != (long)strlen(target)) {
            *error = r < 0 ? r : -EIO;
        }
    }
    if (*error) {
        kfree(n);
        return NULL;
    }
    if (!p->ext && (mode & S_IFMT) == S_IFDIR) {
        p->links++;
    }
    n->next = p->children;
    p->children = n;
    return n;
}
struct node *vfs_create(const char *path, uint32_t mode, const char *target, int *error) {
    return create_at(path, current() ? current()->cwd : root, mode, target, error);
}
static struct node *create_file(const char *path, struct node *start, unsigned mode, bool exclusive,
                                unsigned depth, int *error) {
    if (depth > 40) {
        *error = -ELOOP;
        return NULL;
    }
    struct node *n = vfs_resolve(path, start, false, error);
    if (n) {
        if (exclusive) {
            *error = -EEXIST;
            return NULL;
        }
        if ((n->mode & S_IFMT) != S_IFLNK) {
            return n;
        }
        char target[MAX_PATH];
        long size = node_read(n, 0, target, sizeof(target) - 1);
        if (size < 0) {
            *error = size;
            return NULL;
        }
        target[size] = 0;
        return create_file(target, n->parent, mode, false, depth + 1, error);
    }
    if (*error != -ENOENT) {
        return NULL;
    }
    return create_at(path, start, S_IFREG | (mode & 0777), NULL, error);
}
static void put_file(struct file *f) {
    if (--f->refs) {
        return;
    }
    struct node *n = node_actual(f->node);
    n->refs--;
    if ((n->mode & S_IFMT) == S_IFIFO) {
        if (f->flags & 3) {
            n->writers--;
        } else {
            n->readers--;
        }
    }
    bool release = !n->links && !n->refs;
    if (release) {
        if (n->ext) {
            if (ext2_drop(n)) {
                kprintf("EXT2 release error\n");
            }
        } else {
            node_truncate(n);
        }
    }
    kfree(f);
    if (release && !n->ext) {
        kfree(n);
    }
}
long vfs_open(const char *path, int flags, unsigned mode) {
    int e, fd;
    if ((flags & 3) == 3 || (flags & ~(3 | O_CREAT | 128 | O_TRUNC | O_APPEND | O_DIRECTORY |
                                       O_NOFOLLOW | O_CLOEXEC))) {
        return -EINVAL;
    }
    for (fd = 0; fd < MAX_FD && current()->fds[fd]; fd++)
        ;
    if (fd == MAX_FD) {
        return -24;
    }
    struct node *n = vfs_resolve(path, current()->cwd, !(flags & O_NOFOLLOW), &e);
    if (!n && e == -ENOENT && (flags & O_CREAT)) {
        n = create_file(path, current()->cwd, mode, flags & 128, 0, &e);
    } else if (n && (flags & O_CREAT) && (flags & 128)) {
        return -EEXIST;
    }
    if (!n) {
        return e;
    }
    if ((flags & O_NOFOLLOW) && (n->mode & S_IFMT) == S_IFLNK) {
        return -ELOOP;
    }
    if ((flags & O_DIRECTORY) && (n->mode & S_IFMT) != S_IFDIR) {
        return -ENOTDIR;
    }
    if ((n->mode & S_IFMT) == S_IFDIR && (flags & 3)) {
        return -EISDIR;
    }
    unsigned access = (flags & 3) == 0 ? 4 : (flags & 3) == 1 ? 2 : 6;
    if ((e = vfs_access(n, access))) {
        return e;
    }
    struct file *f = kalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    if ((flags & O_TRUNC) && (flags & 3) && (n->mode & S_IFMT) == S_IFREG &&
        (e = node_truncate(n))) {
        kfree(f);
        return e;
    }
    f->node = node_actual(n);
    f->flags = flags & ~O_CLOEXEC;
    current()->fd_flags[fd] = flags & O_CLOEXEC;
    f->refs = 1;
    node_actual(n)->refs++;
    current()->fds[fd] = f;
    return fd;
}
long vfs_close(int fd) {
    if (fd < 0 || fd >= MAX_FD || !current()->fds[fd]) {
        return -EBADF;
    }
    put_file(current()->fds[fd]);
    current()->fds[fd] = NULL;
    current()->fd_flags[fd] = 0;
    return 0;
}
static long file_io(int fd, uint64_t user, size_t len, bool write) {
    if (fd < 0 || fd >= MAX_FD || !current()->fds[fd]) {
        return -EBADF;
    }
    struct file *f = current()->fds[fd];
    if ((write && !(f->flags & 3)) || (!write && (f->flags & 3) == 1)) {
        return -EBADF;
    }
    if ((f->node->mode & S_IFMT) == S_IFDIR) {
        return -EISDIR;
    }
    if (user_range(user, len, !write)) {
        return -EFAULT;
    }
    if (!len) {
        return 0;
    }
    uint64_t off = f->offset;
    if (write && (f->flags & O_APPEND)) {
        if (f->node->ext) {
            int e = ext2_refresh(f->node);
            if (e) {
                return e;
            }
        }
        off = node_actual(f->node)->size;
    }
    char *buf = kalloc(PAGE);
    if (!buf) {
        return -ENOMEM;
    }
    size_t done = 0;
    long e = 0;
    while (done < len) {
        size_t n = MIN(len - done, PAGE);
        if (write) {
            e = copy_from_user(buf, user + done, n);
            if (e) {
                break;
            }
        }
        e = write ? node_write(f->node, off + done, buf, n)
                  : node_read(f->node, off + done, buf, n);
        if (e <= 0) {
            break;
        }
        if (!write && copy_to_user(user + done, buf, e)) {
            e = -EFAULT;
            break;
        }
        done += e;
        if ((size_t)e < n || (f->node->mode & S_IFMT) == S_IFCHR) {
            break;
        }
    }
    kfree(buf);
    if ((f->node->mode & S_IFMT) != S_IFCHR && (f->node->mode & S_IFMT) != S_IFIFO) {
        f->offset = off + done;
    }
    if (!write && !done && e == 0 && f->node->device == 1) {
        return -EAGAIN;
    }
    return done ? (long)done : e;
}
long vfs_read(int fd, uint64_t b, size_t n) {
    return file_io(fd, b, n, false);
}
long vfs_write(int fd, uint64_t b, size_t n) {
    return file_io(fd, b, n, true);
}
long vfs_seek(int fd, int64_t off, int whence) {
    if (fd < 0 || fd >= MAX_FD || !current()->fds[fd]) {
        return -EBADF;
    }
    struct file *f = current()->fds[fd];
    if ((f->node->mode & S_IFMT) == S_IFCHR || (f->node->mode & S_IFMT) == S_IFIFO) {
        return -29;
    }
    if (f->node->ext) {
        int e = ext2_refresh(f->node);
        if (e) {
            return e;
        }
    }
    uint64_t base = whence == 0   ? 0
                    : whence == 1 ? f->offset
                    : whence == 2 ? node_actual(f->node)->size
                                  : UINT64_MAX;
    if (base == UINT64_MAX || (off < 0 && (uint64_t)(-(off + 1)) + 1 > base) ||
        (off >= 0 && (uint64_t)off > MAX_FILE - base)) {
        return -EINVAL;
    }
    f->offset = base + off;
    return f->offset;
}
int node_stat(struct node *n, struct kstat *s) {
    n = node_actual(n);
    if (n->ext) {
        int e = ext2_refresh(n);
        if (e) {
            return e;
        }
    }
    memset(s, 0, sizeof(*s));
    s->dev = n->ext ? 1 : 0;
    s->ino = n->ino;
    s->nlink = n->links;
    s->mode = n->mode;
    s->uid = n->uid;
    s->gid = n->gid;
    s->size = n->size;
    s->blksize = PAGE;
    if (n->ext) {
        return ext2_stat(n, s);
    }
    for (struct ram_page *p = n->pages; p; p = p->next) {
        s->blocks += PAGE / 512;
    }
    return 0;
}
long vfs_stat(const char *path, uint64_t user, bool follow) {
    int e;
    struct node *n = vfs_resolve(path, current()->cwd, follow, &e);
    if (!n) {
        return e;
    }
    struct kstat st;
    e = node_stat(n, &st);
    return e ? e : copy_to_user(user, &st, sizeof(st));
}
long vfs_fstat(int fd, uint64_t user) {
    if (fd < 0 || fd >= MAX_FD || !current()->fds[fd]) {
        return -EBADF;
    }
    struct kstat st;
    int e = node_stat(current()->fds[fd]->node, &st);
    return e ? e : copy_to_user(user, &st, sizeof(st));
}
static bool inode_open(struct node *n) {
    for (unsigned i = 0; i < MAX_PROC; i++) {
        struct process *p = &processes[i];
        if (!p->state) {
            continue;
        }
        if (p->cwd == n) {
            return true;
        }
        for (unsigned j = 0; j < MAX_FD; j++) {
            if (p->fds[j]) {
                struct node *f = p->fds[j]->node;
                if (node_actual(f) == node_actual(n) ||
                    (n->ext && f->ext == n->ext && f->ino == n->ino)) {
                    return true;
                }
            }
        }
    }
    return false;
}
long vfs_unlink(const char *path, bool directory) {
    int e;
    struct node *n = vfs_resolve(path, current()->cwd, false, &e);
    if (!n) {
        return e;
    }
    if (!n->parent || n->parent->mounted == n || n->mounted) {
        return -EBUSY;
    }
    struct node *p = n->parent;
    if ((e = vfs_access(p, 3))) {
        return e;
    }
    if ((p->mode & 01000) && current()->uid && current()->uid != p->uid &&
        current()->uid != node_actual(n)->uid) {
        return -EPERM;
    }
    bool dir = (n->mode & S_IFMT) == S_IFDIR;
    if (directory != dir) {
        return dir ? -EISDIR : -ENOTDIR;
    }
    if (dir) {
        if (inode_open(n)) {
            return -EBUSY;
        }
        if (n->ext) {
            if ((e = ext2_empty(n))) {
                return e;
            }
        } else if (n->children) {
            return -ENOTEMPTY;
        }
    }
    if (n->ext) {
        if ((e = ext2_remove(p, n))) {
            return e;
        }
    } else {
        if (dir) {
            node_actual(n)->links = 0;
            p->links--;
        } else {
            node_actual(n)->links--;
        }
    }
    struct node **link = &p->children;
    while (*link && *link != n) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = n->next;
    }
    n->parent = NULL;
    struct node *inode = node_actual(n);
    bool alias = !n->ext && n != inode;
    if (!inode->links && !inode_open(inode)) {
        if (inode->ext) {
            return ext2_drop(inode);
        }
        node_truncate(inode);
        kfree(inode);
    }
    if (alias) {
        kfree(n);
    }
    return 0;
}
long vfs_link(const char *source, const char *dest) {
    int e;
    struct node *n = vfs_resolve(source, current()->cwd, false, &e);
    if (!n) {
        return e;
    }
    if ((n->mode & S_IFMT) == S_IFDIR) {
        return -EPERM;
    }
    if (current()->uid && current()->uid != node_actual(n)->uid && vfs_access(n, 6)) {
        return -EPERM;
    }
    char name[256];
    struct node *p = parent_path(dest, current()->cwd, name, &e);
    if (!p) {
        return e;
    }
    if ((e = vfs_access(p, 3))) {
        return e;
    }
    if (p->filesystem != n->filesystem) {
        return -EXDEV;
    }
    if (vfs_resolve(dest, current()->cwd, false, &e)) {
        return -EEXIST;
    }
    if (e != -ENOENT) {
        return e;
    }
    struct node *alias = kalloc(sizeof(*alias));
    if (!alias) {
        return -ENOMEM;
    }
    if (p->ext) {
        e = ext2_link(p, n, name);
        if (e) {
            kfree(alias);
            return e;
        }
    } else {
        node_actual(n)->links++;
    }
    alias->mode = node_actual(n)->mode;
    alias->uid = node_actual(n)->uid;
    alias->gid = node_actual(n)->gid;
    alias->filesystem = p->filesystem;
    alias->ino = n->ino;
    alias->ext = n->ext;
    if (!n->ext) {
        alias->origin = node_actual(n);
    } else {
        ext2_cache(alias);
    }
    strcpy(alias->name, name);
    alias->parent = p;
    alias->next = p->children;
    p->children = alias;
    return 0;
}
long vfs_symlink(const char *target, const char *path) {
    if (strlen(target) >= MAX_PATH) {
        return -EINVAL;
    }
    int e;
    return vfs_create(path, S_IFLNK | 0777, target, &e) ? 0 : e;
}
long vfs_readlink(const char *path, uint64_t user, size_t len) {
    int e;
    struct node *n = vfs_resolve(path, current()->cwd, false, &e);
    if (!n) {
        return e;
    }
    if ((n->mode & S_IFMT) != S_IFLNK) {
        return -EINVAL;
    }
    char b[MAX_PATH];
    long r = node_read(n, 0, b, MIN(len, sizeof(b)));
    if (r < 0) {
        return r;
    }
    return copy_to_user(user, b, r) ? -EFAULT : r;
}
long vfs_chmod(const char *path, unsigned mode) {
    int e;
    struct node *n = vfs_resolve(path, current()->cwd, true, &e);
    if (!n) {
        return e;
    }
    n = node_actual(n);
    if (current()->uid && current()->uid != node_actual(n)->uid) {
        return -EPERM;
    }
    n->mode = (n->mode & S_IFMT) | (mode & 07777);
    return n->ext ? ext2_chmod(n) : 0;
}
long vfs_chdir(const char *path) {
    int e;
    struct node *n = vfs_resolve(path, current()->cwd, true, &e);
    if (!n) {
        return e;
    }
    if ((n->mode & S_IFMT) != S_IFDIR) {
        return -ENOTDIR;
    }
    if ((e = vfs_access(n, 1))) {
        return e;
    }
    current()->cwd = n;
    return 0;
}
long vfs_getcwd(uint64_t user, size_t capacity) {
    char b[MAX_PATH];
    size_t pos = sizeof(b) - 1;
    b[pos] = 0;
    struct node *n = current()->cwd;
    while (n && n != root) {
        size_t len = strlen(n->name);
        if (pos < len + 1) {
            return -34;
        }
        pos -= len;
        memcpy(b + pos, n->name, len);
        b[--pos] = '/';
        n = n->parent;
    }
    if (pos == sizeof(b) - 1) {
        b[--pos] = '/';
    }
    size_t len = sizeof(b) - pos;
    if (capacity < len) {
        return -34;
    }
    return copy_to_user(user, b + pos, len) ? -EFAULT : (long)len;
}
long vfs_dup(int fd, int target) {
    if (fd < 0 || fd >= MAX_FD || !current()->fds[fd]) {
        return -EBADF;
    }
    if (target < 0) {
        for (target = 0; target < MAX_FD && current()->fds[target]; target++)
            ;
    }
    if (target >= MAX_FD) {
        return -EBADF;
    }
    if (fd == target) {
        return target;
    }
    if (current()->fds[target]) {
        vfs_close(target);
    }
    current()->fd_flags[target] = 0;
    current()->fds[target] = current()->fds[fd];
    current()->fds[target]->refs++;
    return target;
}
void vfs_process_close(struct process *p) {
    for (unsigned i = 0; i < MAX_FD; i++) {
        if (p->fds[i]) {
            put_file(p->fds[i]);
            p->fds[i] = NULL;
        }
    }
}
void vfs_process_copy(struct process *dest, struct process *src) {
    memcpy(dest->fd_flags, src->fd_flags, sizeof(dest->fd_flags));
    for (unsigned i = 0; i < MAX_FD; i++) {
        if ((dest->fds[i] = src->fds[i])) {
            dest->fds[i]->refs++;
        }
    }
}
static void destroy_tree(struct node *n) {
    if (n->ext) {
        return;
    }
    while (n->children) {
        struct node *c = n->children;
        n->children = c->next;
        destroy_tree(c);
    }
    if (!n->ext && !n->origin) {
        node_truncate(n);
    }
    kfree(n);
}
long vfs_mount(const char *source, const char *target, const char *type, unsigned flags) {
    if (current()->uid) {
        return -EPERM;
    }
    if (flags) {
        return -EINVAL;
    }
    int e;
    struct node *p = vfs_resolve(target, current()->cwd, true, &e);
    if (!p) {
        return e;
    }
    if ((p->mode & S_IFMT) != S_IFDIR) {
        return -ENOTDIR;
    }
    if (p == root || p->mounted || (p->parent && p->parent->mounted == p)) {
        return -EBUSY;
    }
    struct node *n = kalloc(sizeof(*n));
    if (!n) {
        return -ENOMEM;
    }
    strcpy(n->name, p->name);
    n->mode = S_IFDIR | 0755;
    n->links = 2;
    n->ino = next_ino++;
    n->parent = p->parent;
    n->filesystem = n;
    if (!strcmp(type, "ext2")) {
        struct node *device = vfs_resolve(source, current()->cwd, true, &e);
        struct blockdev *dev = device ? vfs_block(device) : NULL;
        if (!dev || (device->mode & S_IFMT) != S_IFBLK) {
            kfree(n);
            return device ? -EINVAL : e;
        }
        for (unsigned i = 0; i < MAX_PROC; i++) {
            if (processes[i].state) {
                for (unsigned j = 0; j < MAX_FD; j++) {
                    if (processes[i].fds[j] && processes[i].fds[j]->node == device) {
                        kfree(n);
                        return -EBUSY;
                    }
                }
            }
        }
        e = ext2_mount(n, dev);
        if (e) {
            kfree(n);
            return e;
        }
    } else if (strcmp(type, "tmpfs")) {
        kfree(n);
        return -EINVAL;
    }
    p->mounted = n;
    return 0;
}
static bool nested_mount(struct node *n) {
    for (struct node *c = n->children; c; c = c->next) {
        if (c->mounted || nested_mount(c)) {
            return true;
        }
    }
    return false;
}
long vfs_umount(const char *path) {
    if (current()->uid) {
        return -EPERM;
    }
    int e;
    struct node *n = vfs_resolve(path, current()->cwd, true, &e);
    if (!n) {
        return e;
    }
    struct node *point = NULL;
    if (n->parent) {
        for (struct node *p = n->parent->children; p; p = p->next) {
            if (p->mounted == n) {
                point = p;
            }
        }
    }
    if (!point) {
        return -EINVAL;
    }
    if (nested_mount(n)) {
        return -EBUSY;
    }
    for (unsigned i = 0; i < MAX_PROC; i++) {
        if (processes[i].state) {
            struct node *cwd = processes[i].cwd;
            for (struct node *p = cwd; p; p = p->parent) {
                if (p == n) {
                    return -EBUSY;
                }
            }
            for (unsigned j = 0; j < MAX_FD; j++) {
                if (processes[i].fds[j]) {
                    if (processes[i].fds[j]->node->filesystem == n) {
                        return -EBUSY;
                    }
                    for (struct node *p = processes[i].fds[j]->node; p; p = p->parent) {
                        if (p == n) {
                            return -EBUSY;
                        }
                    }
                }
            }
        }
    }
    struct extfs *ext = n->ext;
    if (ext && (e = ext2_sync(ext))) {
        return e;
    }
    point->mounted = NULL;
    destroy_tree(n);
    if (ext) {
        ext2_release(ext);
    }
    return 0;
}
long vfs_getdents(int fd, uint64_t user, size_t len) {
    if (fd < 0 || fd >= MAX_FD || !current()->fds[fd]) {
        return -EBADF;
    }
    struct file *f = current()->fds[fd];
    if ((f->node->mode & S_IFMT) != S_IFDIR) {
        return -ENOTDIR;
    }
    if (vfs_access(f->node, 4)) {
        return -EACCES;
    }
    len = MIN(len, PAGE);
    if (user_range(user, len, true)) {
        return -EFAULT;
    }
    uint8_t *buf = kalloc(PAGE);
    if (!buf) {
        return -ENOMEM;
    }
    long result;
    if (f->node->ext) {
        result = ext2_getdents(f->node, &f->offset, buf, len);
    } else {
        size_t done = 0;
        uint64_t index = 0;
        result = 0;
        for (struct node *n = f->node->children; n; n = n->next, index++) {
            if (index < f->offset) {
                continue;
            }
            size_t record = ALIGN_UP(20 + strlen(n->name), 8);
            if (record > len - done) {
                if (!done) {
                    result = -EINVAL;
                }
                break;
            }
            uint8_t *p = buf + done;
            memset(p, 0, record);
            *(uint64_t *)p = node_actual(n)->ino;
            *(uint64_t *)(p + 8) = index + 1;
            *(uint16_t *)(p + 16) = record;
            p[18] = (n->mode & S_IFMT) >> 12;
            strcpy((char *)p + 19, n->name);
            done += record;
            f->offset = index + 1;
        }
        if (done) {
            result = done;
        }
    }
    if (result > 0 && copy_to_user(user, buf, result)) {
        result = -EFAULT;
    }
    kfree(buf);
    return result;
}
long vfs_pipe(uint64_t user) {
    if (user_range(user, 8, true)) {
        return -EFAULT;
    }
    int fds[2] = {-1, -1};
    for (int i = 0; i < MAX_FD; i++) {
        if (!current()->fds[i]) {
            if (fds[0] < 0) {
                fds[0] = i;
            } else {
                fds[1] = i;
                break;
            }
        }
    }
    if (fds[1] < 0) {
        return -24;
    }
    struct node *n = kalloc(sizeof(*n));
    struct file *r = kalloc(sizeof(*r)), *w = kalloc(sizeof(*w));
    struct ram_page *page = kalloc(sizeof(*page));
    if (!n || !r || !w || !page) {
        kfree(n);
        kfree(r);
        kfree(w);
        kfree(page);
        return -ENOMEM;
    }
    n->mode = S_IFIFO | 0600;
    n->ino = next_ino++;
    n->pages = page;
    n->refs = 2;
    n->readers = n->writers = 1;
    r->node = w->node = n;
    r->refs = w->refs = 1;
    w->flags = 1;
    current()->fds[fds[0]] = r;
    current()->fds[fds[1]] = w;
    return copy_to_user(user, fds, sizeof(fds));
}
