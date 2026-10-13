#ifndef KALY_WM_H
#define KALY_WM_H
#include <stdint.h>
#define KWM_IOCTL 0xc0805701UL
#define KWM_VERSION 1
#define KWM_EVENT_CAPACITY 128
#define KWM_APP_LIMIT 256
#define KWM_APP_SYSTEM 1
#define KWM_UP 0x100
#define KWM_DOWN 0x101
#define KWM_LEFT 0x102
#define KWM_RIGHT 0x103
enum kwm_operation {
    KWM_INFO,
    KWM_CREATE,
    KWM_DESTROY,
    KWM_MOVE,
    KWM_RESIZE,
    KWM_BLIT,
    KWM_READ,
    KWM_FILL,
    KWM_TEXT,
    KWM_EVENT,
    KWM_FOCUS,
    KWM_TITLE,
    KWM_SESSION,
    KWM_LINE,
    KWM_APPLICATIONS
};
enum kwm_event_type {
    KWM_KEY = 1,
    KWM_MOUSE,
    KWM_WHEEL,
    KWM_SIZE,
    KWM_CLOSE,
    KWM_ACTIVATE,
    KWM_OVERFLOW,
    KWM_LAUNCH,
    KWM_MENU_REFRESH
};
struct kwm_application {
    uint32_t id, flags;
    char label[64];
};
_Static_assert(sizeof(struct kwm_application) == 72, "wm application ABI");
struct kwm_request {
    uint32_t version, operation;
    uint64_t window;
    int32_t x, y, width, height;
    uint32_t color, flags;
    uint64_t data, length, argument;
    char title[64];
};
struct kwm_event {
    uint32_t type, code;
    int32_t x, y, width, height;
    uint32_t buttons, reserved;
};
_Static_assert(sizeof(struct kwm_request) == 128, "wm request ABI");
_Static_assert(sizeof(struct kwm_event) == 32, "wm event ABI");
#endif
