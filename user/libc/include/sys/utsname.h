#ifndef UTSNAME_H
#define UTSNAME_H
struct utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
int uname(struct utsname *);
#endif
