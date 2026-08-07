#include "api_vertex.h"
#include "api_immediate.h"
#include "displaylist.h"

void glVertex2f(GLfloat x, GLfloat y)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_vertex(x, y, 0.0f);
        return;
    }
    gl_immediate_vertex(x, y, 0.0f);
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_vertex(x, y, z);
        return;
    }
    gl_immediate_vertex(x, y, z);
}

void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_vertex(x, y, z);
        return;
    }
    gl_immediate_vertex4f(x, y, z, w);
}

void glVertex2d(GLdouble x, GLdouble y)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_vertex((GLfloat)x, (GLfloat)y, 0.0f);
        return;
    }
    gl_immediate_vertex((GLfloat)x, (GLfloat)y, 0.0);
}

void glVertex3d(GLdouble x, GLdouble y, GLdouble z)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_vertex((GLfloat)x, (GLfloat)y, (GLfloat)z);
        return;
    }
    gl_immediate_vertex((GLfloat)x, (GLfloat)y, (GLfloat)z);
}

void glColor3f(GLfloat r, GLfloat g, GLfloat b)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_color(r, g, b, 1.0f);
        return;
    }
    gl_immediate_color3f(r, g, b);
}

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_color(r, g, b, a);
        return;
    }
    gl_immediate_color(r, g, b, a);
}

void glColor3ub(GLubyte r, GLubyte g, GLubyte b)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_color(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
        return;
    }
    gl_immediate_color3f(r / 255.0f, g / 255.0f, b / 255.0f);
}

void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_color(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
        return;
    }
    gl_immediate_color(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

void glNormal3f(GLfloat x, GLfloat y, GLfloat z)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_normal(x, y, z);
        return;
    }
    gl_immediate_normal(x, y, z);
}

void glTexCoord2f(GLfloat s, GLfloat t)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_texcoord(s, t);
        return;
    }
    gl_immediate_texcoord(s, t);
}

void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r_tex)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_texcoord(s, t);
        return;
    }
    gl_immediate_texcoord(s, t);
}
