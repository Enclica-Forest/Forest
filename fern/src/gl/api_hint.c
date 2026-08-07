#include "gl.h"

typedef struct {
    GLenum perspective_correction;
    GLenum point_smooth;
    GLenum line_smooth;
    GLenum polygon_smooth;
    GLenum fog;
    GLenum generate_mipmap;
} gl_hint_state_t;

static gl_hint_state_t g_hints = {
    .perspective_correction = GL_DONT_CARE,
    .point_smooth           = GL_DONT_CARE,
    .line_smooth            = GL_DONT_CARE,
    .polygon_smooth         = GL_DONT_CARE,
    .fog                    = GL_DONT_CARE,
    .generate_mipmap        = GL_DONT_CARE,
};

void glHint(GLenum target, GLenum mode) {
    if (mode != GL_FASTEST && mode != GL_NICEST && mode != GL_DONT_CARE)
        return;

    switch (target) {
    case GL_PERSPECTIVE_CORRECTION_HINT: g_hints.perspective_correction = mode; break;
    case GL_POINT_SMOOTH_HINT:           g_hints.point_smooth = mode; break;
    case GL_LINE_SMOOTH_HINT:            g_hints.line_smooth = mode; break;
    case GL_POLYGON_SMOOTH_HINT:         g_hints.polygon_smooth = mode; break;
    case GL_FOG_HINT:                    g_hints.fog = mode; break;
    case GL_GENERATE_MIPMAP_HINT:        g_hints.generate_mipmap = mode; break;
    default: break;
    }
}
