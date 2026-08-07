#ifndef GL_LIGHTING_H
#define GL_LIGHTING_H

#include "math.h"

void gl_compute_lighting(float nx, float ny, float nz,
                         float *eye_x, float *eye_y, float *eye_z,
                         float *r, float *g, float *b);

void gl_update_normal_matrix(void);

#endif
