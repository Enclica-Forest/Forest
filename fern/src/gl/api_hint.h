#ifndef GL_API_HINT_H
#define GL_API_HINT_H

#include "state.h"

#define GL_FASTEST   0x1101
#define GL_NICEST    0x1102
#define GL_DONT_CARE 0x1100

#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#define GL_POINT_SMOOTH_HINT           0x0C51
#define GL_LINE_SMOOTH_HINT            0x0C52
#define GL_POLYGON_SMOOTH_HINT         0x0C53
#define GL_FOG_HINT                    0x0C54
#define GL_GENERATE_MIPMAP_HINT        0x8192

void glHint(GLenum target, GLenum mode);

#endif
