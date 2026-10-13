#ifndef KALYGL_H
#define KALYGL_H
#include <KalyGFX.h>
#include <GL/gl.h>
typedef struct KGL_Context KGL_Context;
KGL_Context *KGL_CreateContext(KGFX_Window *);
void KGL_DestroyContext(KGL_Context *);
int KGL_MakeCurrent(KGL_Context *);
int KGL_SwapBuffers(KGL_Context *);
#endif
