#ifndef GL_FRAGMENT_H
#define GL_FRAGMENT_H

#include "rasterizer.h"

extern fragment_shader_fn g_gl_fragment_shader;

void gl_default_fragment_shader(gl_fragment_t *frag);

#endif
