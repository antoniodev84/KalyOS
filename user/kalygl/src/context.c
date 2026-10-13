#include "../vendor/tinygl/src/zgl.h"
#include "../../kalygfx/src/internal.h"
#include <KalyGL.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
struct KGL_Context {
    GLContext state;
    ZBuffer *buffer;
    KGFX_Window *window;
};
static KGL_Context *current;
int KGL_MakeCurrent(KGL_Context *context) {
    if (current == context) {
        return 0;
    }
    if (current) {
        current->state = gl_ctx;
    }
    current = context;
    if (context) {
        gl_ctx = context->state;
    } else {
        memset(&gl_ctx, 0, sizeof(gl_ctx));
    }
    return 0;
}
static int resize_context(void *pointer, int width, int height) {
    KGL_Context *context = pointer;
    ZBuffer *next = ZB_open((width + 3) & ~3, height, ZB_MODE_RGBA, NULL);
    if (!next) {
        errno = ENOMEM;
        return -1;
    }
    KGL_Context *saved = current;
    KGL_MakeCurrent(context);
    ZB_close(context->buffer);
    context->buffer = next;
    gl_ctx.zb = next;
    glViewport(0, 0, width, height);
    if (saved != context) {
        KGL_MakeCurrent(saved);
    }
    return 0;
}
static void delete_context(void *pointer) {
    KGL_DestroyContext(pointer);
}
KGL_Context *KGL_CreateContext(KGFX_Window *window) {
    if (!window || window->context) {
        errno = EINVAL;
        kgfx_error("GL context owner");
        return NULL;
    }
    KGL_Context *context = calloc(1, sizeof(*context));
    if (!context) {
        kgfx_error("GL context allocation");
        return NULL;
    }
    context->buffer =
        ZB_open((window->native.width + 3) & ~3, window->native.height, ZB_MODE_RGBA, NULL);
    if (!context->buffer) {
        free(context);
        errno = ENOMEM;
        kgfx_error("GL framebuffer");
        return NULL;
    }
    if (current) {
        current->state = gl_ctx;
    }
    glInit(context->buffer);
    context->window = window;
    context->state = gl_ctx;
    current = context;
    window->context = context;
    window->resize_context = resize_context;
    window->delete_context = delete_context;
    glViewport(0, 0, window->native.width, window->native.height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return context;
}
void KGL_DestroyContext(KGL_Context *context) {
    if (!context) {
        return;
    }
    KGL_Context *saved = current == context ? NULL : current;
    KGL_MakeCurrent(context);
    glClose();
    ZB_close(context->buffer);
    context->window->context = NULL;
    context->window->resize_context = NULL;
    context->window->delete_context = NULL;
    current = NULL;
    if (saved) {
        KGL_MakeCurrent(saved);
    } else {
        memset(&gl_ctx, 0, sizeof(gl_ctx));
    }
    free(context);
}
int KGL_SwapBuffers(KGL_Context *context) {
    if (!context) {
        errno = EINVAL;
        return kgfx_error("GL swap context");
    }
    KGFX_Surface *surface = KGFX_GetWindowSurface(context->window);
    int stride = context->buffer->linesize;
    for (int y = 0; y < surface->height; y++) {
        memcpy((unsigned char *)surface->pixels + (size_t)y * surface->pitch,
               (unsigned char *)context->buffer->pbuf + (size_t)y * stride, surface->width * 4);
    }
    return KGFX_UpdateWindowSurface(context->window);
}
KGFX_GLContext KGFX_GL_CreateContext(KGFX_Window *window) {
    return KGL_CreateContext(window);
}
void KGFX_GL_DeleteContext(KGFX_GLContext context) {
    KGL_DestroyContext(context);
}
int KGFX_GL_MakeCurrent(KGFX_Window *window, KGFX_GLContext pointer) {
    KGL_Context *context = pointer;
    if (!context || context->window != window) {
        errno = EINVAL;
        return kgfx_error("GL context/window mismatch");
    }
    return KGL_MakeCurrent(context);
}
int KGFX_GL_SwapWindow(KGFX_Window *window) {
    return KGL_SwapBuffers(window->context);
}
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_value,
             GLdouble far_value) {
    if (!current) {
        return;
    }
    if (gl_ctx.in_begin) {
        gl_ctx.error_flag = GL_INVALID_OPERATION;
        return;
    }
    if (left == right || bottom == top || near_value == far_value) {
        gl_ctx.error_flag = GL_INVALID_VALUE;
        return;
    }
    GLfloat matrix[16] = {0};
    matrix[0] = 2 / (right - left);
    matrix[5] = 2 / (top - bottom);
    matrix[10] = -2 / (far_value - near_value);
    matrix[12] = -(right + left) / (right - left);
    matrix[13] = -(top + bottom) / (top - bottom);
    matrix[14] = -(far_value + near_value) / (far_value - near_value);
    matrix[15] = 1;
    glMultMatrixf(matrix);
}
