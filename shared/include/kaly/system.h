#ifndef KALY_SYSTEM_H
#define KALY_SYSTEM_H
#include <stdint.h>
#define KSYSTEM_IOCTL 0xc0105301UL
#define KSYSTEM_VERSION 1
enum ksystem_operation { KSYSTEM_RESERVE_PID, KSYSTEM_START_GUI, KSYSTEM_POLL_GUI };
struct ksystem_request {
    uint32_t version, operation, pid, flags;
};
_Static_assert(sizeof(struct ksystem_request) == 16, "system request ABI");
#endif
