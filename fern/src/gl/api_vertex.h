#ifndef GL_API_VERTEX_H
#define GL_API_VERTEX_H

#include "state.h"

void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glVertex2d(GLdouble x, GLdouble y);
void glVertex3d(GLdouble x, GLdouble y, GLdouble z);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColor3ub(GLubyte r, GLubyte g, GLubyte b);
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
void glNormal3f(GLfloat x, GLfloat y, GLfloat z);
void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r);

#endif
