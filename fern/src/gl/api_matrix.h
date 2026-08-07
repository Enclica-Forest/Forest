#ifndef GL_API_MATRIX_H
#define GL_API_MATRIX_H

#include "../include/types.h"
#include "state.h"

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glLoadMatrixd(const GLdouble *m);
void glMultMatrixf(const GLfloat *m);
void glMultMatrixd(const GLdouble *m);
void glPushMatrix(void);
void glPopMatrix(void);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glScaled(GLdouble x, GLdouble y, GLdouble z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble near_val, GLdouble far_val);
void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble near_val, GLdouble far_val);
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar);
void gluLookAt(GLdouble eyeX, GLdouble eyeY, GLdouble eyeZ,
               GLdouble centerX, GLdouble centerY, GLdouble centerZ,
               GLdouble upX, GLdouble upY, GLdouble upZ);

#endif
