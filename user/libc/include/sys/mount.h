#ifndef MOUNT_H
#define MOUNT_H
int mount(const char *, const char *, const char *, unsigned long, const void *);
int umount(const char *);
#endif
