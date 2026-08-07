#ifndef GL_API_QUERY_H
#define GL_API_QUERY_H

#include "state.h"

void glGetBooleanv(GLenum pname, GLboolean *params);
void glGetDoublev(GLenum pname, GLdouble *params);
void glGetFloatv(GLenum pname, GLfloat *params);
void glGetIntegerv(GLenum pname, GLint *params);
GLboolean glIsEnabled(GLenum cap);
void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels);
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params);
void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params);
void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params);

#endif
