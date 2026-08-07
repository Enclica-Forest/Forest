#include "gl.h"

const GLubyte* glGetString(GLenum name)
{
    switch (name) {
        case GL_VENDOR:   return (const GLubyte*)"Forest OS";
        case GL_RENDERER: return (const GLubyte*)"Software OpenGL 1.1";
        case GL_VERSION:  return (const GLubyte*)"1.1 Forest Software";
        default: return (const GLubyte*)"";
    }
}
