#include "error.h"

static GLenum g_gl_error = GL_NO_ERROR;

GLenum glGetError(void)
{
    GLenum err = g_gl_error;
    g_gl_error = GL_NO_ERROR;
    return err;
}

void gl_set_error(GLenum error)
{
    if (g_gl_error == GL_NO_ERROR)
        g_gl_error = error;
}
