#include "msghandling.h"
#include "zgl.h"

void glPolygonStipple(void* a) {
#if TGL_FEATURE_POLYGON_STIPPLE == 1
	GLContext* c = gl_get_context();
#include "error_check.h"
	ZBuffer* zb = c->zb;

	memcpy(zb->stipplepattern, a, TGL_POLYGON_STIPPLE_BYTES);
	for (GLint i = 0; i < TGL_POLYGON_STIPPLE_BYTES; i++) {
		zb->stipplepattern[i] = ((GLubyte*)a)[i];
	}
#endif
}

void glopViewport(GLParam* p) {
	GLContext* c = gl_get_context();
	GLint xsize, ysize, xmin, ymin, xsize_req, ysize_req;

	xmin = p[1].i;
	ymin = p[2].i;
	xsize = p[3].i;
	ysize = p[4].i;
    if (xmin < 0 || ymin < 0 || xsize <= 0 || ysize <= 0 ||
        xsize > c->zb->xsize || ysize > c->zb->ysize ||
        xmin > c->zb->xsize - xsize || ymin > c->zb->ysize - ysize) {
        c->error_flag = GL_INVALID_VALUE;
        return;
    }

	/* we may need to resize the zbuffer */

	if (c->viewport.xmin != xmin || c->viewport.ymin != ymin || c->viewport.xsize != xsize || c->viewport.ysize != ysize) {

		xsize_req = xmin + xsize;
		ysize_req = ymin + ysize;

		if (c->gl_resize_viewport && c->gl_resize_viewport(&xsize_req, &ysize_req) != 0) {
			gl_fatal_error("glViewport: error while resizing display");
		}
		if (xsize <= 0 || ysize <= 0) {
			gl_fatal_error("glViewport: size too small");
		}

		
		c->viewport.xmin = xmin;
		c->viewport.ymin = ymin;
		c->viewport.xsize = xsize;
		c->viewport.ysize = ysize;

		
		gl_eval_viewport();
		
	}
}
void glBlendFunc(GLenum sfactor, GLenum dfactor) {
	GLParam p[3];
#include "error_check_no_context.h"
	p[0].op = OP_BlendFunc;
	p[1].i = sfactor;
	p[2].i = dfactor;
	gl_add_op(p);
	return;
}
void glopBlendFunc(GLParam* p) {
	GLContext* c = gl_get_context();
	c->zb->sfactor = p[1].i;
	c->zb->dfactor = p[2].i;
}

void glBlendEquation(GLenum mode) {
	GLParam p[2];
#include "error_check_no_context.h"
	p[0].op = OP_BlendEquation;
	p[1].i = mode;
	gl_add_op(p);
}
void glopBlendEquation(GLParam* p) {
	GLContext* c = gl_get_context();
	c->zb->blendeq = p[1].i;
}

void glopPointSize(GLParam* p) {
	GLContext* c = gl_get_context();
	c->zb->pointsize = p[1].f;
}
void glPointSize(GLfloat f) {
	GLParam p[2];
	p[0].op = OP_PointSize;
#include "error_check_no_context.h"
	p[1].f = f;
	gl_add_op(p);
}

void glopEnableDisable(GLParam* p) {
	GLContext* c = gl_get_context();
	GLint code = p[1].i;
	GLint v = p[2].i;

	switch (code) {
	case GL_CULL_FACE:
		c->cull_face_enabled = v;
		break;
	case GL_LIGHTING:
		c->lighting_enabled = v;
		break;
	case GL_COLOR_MATERIAL:
		c->color_material_enabled = v;
		break;
	case GL_TEXTURE_2D:
		c->texture_2d_enabled = v;
		break;
	case GL_BLEND:
		c->zb->enable_blend = v;
		break;
	case GL_NORMALIZE:
		c->normalize_enabled = v;
		break;
	case GL_DEPTH_TEST:
		c->zb->depth_test = v;
		break;
	case GL_POLYGON_OFFSET_FILL:
		if (v)
			c->offset_states |= TGL_OFFSET_FILL;
		else
			c->offset_states &= ~TGL_OFFSET_FILL;
		break;
	case GL_POLYGON_STIPPLE:
#if TGL_FEATURE_POLYGON_STIPPLE == 1
		c->zb->dostipple = v;
#endif
		break;
	case GL_POLYGON_OFFSET_POINT:
		if (v)
			c->offset_states |= TGL_OFFSET_POINT;
		else
			c->offset_states &= ~TGL_OFFSET_POINT;
		break;
	case GL_POLYGON_OFFSET_LINE:
		if (v)
			c->offset_states |= TGL_OFFSET_LINE;
		else
			c->offset_states &= ~TGL_OFFSET_LINE;
		break;
	default:
		if (code >= GL_LIGHT0 && code < GL_LIGHT0 + MAX_LIGHTS) {
			gl_enable_disable_light(code - GL_LIGHT0, v);
		} else {
			tgl_warning("glEnableDisable: 0x%X not supported.\n", code);
		}
		break;
	}
}

void glopShadeModel(GLParam* p) {
	GLContext* c = gl_get_context();
	GLint code = p[1].i;
	c->current_shade_model = code;
}

void glopCullFace(GLParam* p) {
	GLContext* c = gl_get_context();
	GLint code = p[1].i;
	c->current_cull_face = code;
}

void glopFrontFace(GLParam* p) {
	GLContext* c = gl_get_context();
	GLint code = p[1].i;
	c->current_front_face = code;
}

void glopPolygonMode(GLParam* p) {
	GLContext* c = gl_get_context();
	GLint face = p[1].i;
	GLint mode = p[2].i;

	switch (face) {
	case GL_BACK:
		c->polygon_mode_back = mode;
		break;
	case GL_FRONT:
		c->polygon_mode_front = mode;
		break;
	case GL_FRONT_AND_BACK:
		c->polygon_mode_front = mode;
		c->polygon_mode_back = mode;
		break;
	default:
		break;
	}
}

void glopPolygonOffset(GLParam* p) {
	GLContext* c = gl_get_context();
	c->offset_factor = p[1].f;
	c->offset_units = p[2].f;
}

GLenum glGetError() {
#if TGL_FEATURE_ERROR_CHECK == 1
	GLContext* c = gl_get_context();
	GLenum eflag = c->error_flag;
	if (eflag != GL_OUT_OF_MEMORY) 
		c->error_flag = GL_NO_ERROR;
	return eflag;
#else
	return GL_NO_ERROR;
#endif
}

void glDrawBuffer(GLenum mode) {
    GLContext *c = gl_get_context();
    if (c->in_begin) {
        c->error_flag = GL_INVALID_OPERATION;
        return;
    }
    if (mode != GL_FRONT) {
        c->error_flag = GL_INVALID_ENUM;
        return;
    }
    c->drawbuffer = mode;
}

void glReadBuffer(GLenum mode) {
	GLContext* c = gl_get_context();
#include "error_check.h"
	if ((mode != GL_FRONT && mode != GL_NONE) || c->in_begin) {
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_INVALID_OPERATION
#include "error_check.h"
#else
		return;
#endif
	}
	c->readbuffer = mode;
}


void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                  GLenum type, void *data) {
    GLContext *c = gl_get_context();
    if (!c->zb || c->in_begin || c->readbuffer != GL_FRONT) {
        c->error_flag = GL_INVALID_OPERATION;
        return;
    }
    if (width < 0 || height < 0 || (!data && width && height)) {
        c->error_flag = GL_INVALID_VALUE;
        return;
    }
    if (format != GL_RGB && format != GL_RGBA && format != GL_DEPTH_COMPONENT) {
        c->error_flag = GL_INVALID_ENUM;
        return;
    }
    if ((format == GL_DEPTH_COMPONENT && type != GL_FLOAT && type != GL_UNSIGNED_SHORT) ||
        (format != GL_DEPTH_COMPONENT && type != GL_UNSIGNED_BYTE)) {
        c->error_flag = GL_INVALID_ENUM;
        return;
    }
    size_t components = format == GL_RGBA ? 4 : format == GL_RGB ? 3 : 1;
    size_t bytes = type == GL_FLOAT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
    size_t pitch = ((size_t)width * components * bytes + 3) & ~(size_t)3;
    for (GLint row = 0; row < height; row++) {
        unsigned char *dest = (unsigned char *)data + (size_t)row * pitch;
        for (GLint col = 0; col < width; col++) {
            long px = (long)x + col;
            long py = (long)y + row;
            if (px < 0 || px >= c->zb->xsize || py < 0 || py >= c->zb->ysize) {
                continue;
            }
            size_t index = (size_t)(c->zb->ysize - 1 - py) * c->zb->xsize + px;
            if (format == GL_DEPTH_COMPONENT) {
                unsigned short depth = 65535 - c->zb->zbuf[index];
                if (type == GL_FLOAT) {
                    float value = depth / 65535.0f;
                    memcpy(dest + (size_t)col * 4, &value, 4);
                } else {
                    memcpy(dest + (size_t)col * 2, &depth, 2);
                }
            } else {
                PIXEL pixel = c->zb->pbuf[index];
                dest[col * components] = GET_RED(pixel);
                dest[col * components + 1] = GET_GREEN(pixel);
                dest[col * components + 2] = GET_BLUE(pixel);
                if (components == 4) {
                    dest[col * components + 3] = 255;
                }
            }
        }
    }
}

void glFinish() { return; }
