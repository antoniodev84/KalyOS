#ifndef KALYGFX_H
#define KALYGFX_H
#include <stdint.h>
#include <stddef.h>
typedef struct KGFX_Window KGFX_Window;
typedef void *KGFX_GLContext;
typedef struct {
    int x, y, width, height;
} KGFX_Rect;
typedef struct {
    int width, height, pitch;
    uint32_t *pixels;
} KGFX_Surface;
enum {
    KGFX_QUIT = 1,
    KGFX_KEYDOWN,
    KGFX_MOUSE,
    KGFX_MOUSEWHEEL,
    KGFX_RESIZED,
    KGFX_FOCUS,
    KGFX_OVERFLOW
};
typedef struct {
    int type, key, x, y, width, height, wheel;
    unsigned buttons;
    uint64_t timestamp;
} KGFX_Event;
int KGFX_Init(void);
void KGFX_Quit(void);
const char *KGFX_GetError(void);
KGFX_Window *KGFX_CreateWindow(const char *, int, int);
void KGFX_DestroyWindow(KGFX_Window *);
void KGFX_GetWindowSize(KGFX_Window *, int *, int *);
int KGFX_SetWindowSize(KGFX_Window *, int, int);
int KGFX_SetWindowPosition(KGFX_Window *, int, int);
int KGFX_SetWindowTitle(KGFX_Window *, const char *);
int KGFX_PollEvent(KGFX_Window *, KGFX_Event *);
KGFX_Surface *KGFX_GetWindowSurface(KGFX_Window *);
KGFX_Surface *KGFX_CreateSurface(int, int);
void KGFX_FreeSurface(KGFX_Surface *);
int KGFX_FillRect(KGFX_Surface *, const KGFX_Rect *, uint32_t);
int KGFX_BlitSurface(const KGFX_Surface *, const KGFX_Rect *, KGFX_Surface *, int, int);
int KGFX_DrawLine(KGFX_Surface *, int, int, int, int, uint32_t);
int KGFX_DrawText(KGFX_Window *, int, int, const char *, uint32_t);
int KGFX_UpdateWindowSurface(KGFX_Window *);
uint64_t KGFX_GetTicks(void);
void KGFX_Delay(unsigned);
KGFX_GLContext KGFX_GL_CreateContext(KGFX_Window *);
void KGFX_GL_DeleteContext(KGFX_GLContext);
int KGFX_GL_MakeCurrent(KGFX_Window *, KGFX_GLContext);
int KGFX_GL_SwapWindow(KGFX_Window *);
#endif
