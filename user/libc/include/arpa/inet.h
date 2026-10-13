#ifndef ARPA_INET_H
#define ARPA_INET_H
#include <netinet/in.h>
int inet_pton(int, const char *, void *);
const char *inet_ntop(int, const void *, char *, socklen_t);
#endif
