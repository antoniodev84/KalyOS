#ifndef KALYGFX_INTERNAL_H
#define KALYGFX_INTERNAL_H
#include <KalyGFX.h>
#include <gui.h>
struct KGFX_Window {
    struct gui native;
    KGFX_Surface surface;
    void *context;
    int (*resize_context)(void *, int, int);
    void (*delete_context)(void *);
    struct KGFX_Window *next;
};
int kgfx_error(const char *);
#endif
