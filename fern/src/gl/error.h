#ifndef GL_ERROR_H
#define GL_ERROR_H

#include "state.h"

#define GL_NO_ERROR          0
#define GL_INVALID_ENUM      0x0500
#define GL_INVALID_VALUE     0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_STACK_OVERFLOW    0x0503
#define GL_STACK_UNDERFLOW   0x0504
#define GL_OUT_OF_MEMORY     0x0505

GLenum glGetError(void);
void gl_set_error(GLenum error);

#endif
