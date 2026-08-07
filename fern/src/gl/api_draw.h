#ifndef GL_API_DRAW_H
#define GL_API_DRAW_H

#include "state.h"

void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);

#endif
