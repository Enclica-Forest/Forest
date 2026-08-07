#ifndef GL_API_LIGHT_H
#define GL_API_LIGHT_H

#include "state.h"

#define GL_AMBIENT   0x1200
#define GL_DIFFUSE   0x1201
#define GL_SPECULAR  0x1202
#define GL_POSITION  0x1203
#define GL_SPOT_DIRECTION 0x1204
#define GL_SPOT_EXPONENT  0x1205
#define GL_SPOT_CUTOFF    0x1206
#define GL_CONSTANT_ATTENUATION  0x1207
#define GL_LINEAR_ATTENUATION    0x1208
#define GL_QUADRATIC_ATTENUATION 0x1209

#define GL_LIGHTING      0x0B50
#define GL_LIGHT0        0x4000
#define GL_COLOR_MATERIAL 0x0B57
#define GL_NORMALIZE     0x0BA1

#define GL_FRONT_AND_BACK 0x0408
#define GL_AMBIENT_AND_DIFFUSE 0x1602

#define GL_SHININESS            0x1601
#define GL_EMISSION             0x1600

#define GL_LIGHT_MODEL_AMBIENT  0x0B53
#define GL_LIGHT_MODEL_LOCAL_VIEWER 0x0B51

#define GL_SMOOTH               0x1D01
#define GL_FLAT                 0x1D00

void gl_lightf(GLenum light, GLenum pname, GLfloat param);
void gl_lightfv(GLenum light, GLenum pname, const GLfloat *params);
void gl_materialf(GLenum face, GLenum pname, GLfloat param);
void gl_materialfv(GLenum face, GLenum pname, const GLfloat *params);
void gl_light_modeli(GLenum pname, GLint param);
void gl_light_modelf(GLenum pname, GLfloat param);
void gl_light_modelfv(GLenum pname, const GLfloat *params);
void gl_shade_model(GLenum mode);

#endif
