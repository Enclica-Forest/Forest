#include "api_light.h"
#include <string.h>

static int validate_light(GLenum light)
{
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + 8) return -1;
    return (int)(light - GL_LIGHT0);
}

void gl_lightf(GLenum light, GLenum pname, GLfloat param)
{
    int idx = validate_light(light);
    if (idx < 0) return;

    switch (pname) {
    case GL_SPOT_EXPONENT:
        break;
    case GL_SPOT_CUTOFF:
        break;
    case GL_CONSTANT_ATTENUATION:
        break;
    case GL_LINEAR_ATTENUATION:
        break;
    case GL_QUADRATIC_ATTENUATION:
        break;
    default:
        break;
    }
}

void gl_lightfv(GLenum light, GLenum pname, const GLfloat *params)
{
    int idx = validate_light(light);
    if (idx < 0 || !params) return;

    switch (pname) {
    case GL_AMBIENT:
        g_gl_state.light_ambient[idx][0] = params[0];
        g_gl_state.light_ambient[idx][1] = params[1];
        g_gl_state.light_ambient[idx][2] = params[2];
        g_gl_state.light_ambient[idx][3] = params[3];
        break;
    case GL_DIFFUSE:
        g_gl_state.light_diffuse[idx][0] = params[0];
        g_gl_state.light_diffuse[idx][1] = params[1];
        g_gl_state.light_diffuse[idx][2] = params[2];
        g_gl_state.light_diffuse[idx][3] = params[3];
        break;
    case GL_SPECULAR:
        g_gl_state.light_specular[idx][0] = params[0];
        g_gl_state.light_specular[idx][1] = params[1];
        g_gl_state.light_specular[idx][2] = params[2];
        g_gl_state.light_specular[idx][3] = params[3];
        break;
    case GL_POSITION:
        g_gl_state.light_position[idx][0] = params[0];
        g_gl_state.light_position[idx][1] = params[1];
        g_gl_state.light_position[idx][2] = params[2];
        g_gl_state.light_position[idx][3] = params[3];
        break;
    default:
        break;
    }
}

void gl_materialf(GLenum face, GLenum pname, GLfloat param)
{
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK)
        return;

    switch (pname) {
    case GL_SHININESS:
        g_gl_state.material_shininess = param;
        break;
    default:
        break;
    }
}

void gl_materialfv(GLenum face, GLenum pname, const GLfloat *params)
{
    if (!params) return;
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK)
        return;

    switch (pname) {
    case GL_AMBIENT:
        g_gl_state.material_ambient[0] = params[0];
        g_gl_state.material_ambient[1] = params[1];
        g_gl_state.material_ambient[2] = params[2];
        g_gl_state.material_ambient[3] = params[3];
        break;
    case GL_DIFFUSE:
        g_gl_state.material_diffuse[0] = params[0];
        g_gl_state.material_diffuse[1] = params[1];
        g_gl_state.material_diffuse[2] = params[2];
        g_gl_state.material_diffuse[3] = params[3];
        break;
    case GL_SPECULAR:
        g_gl_state.material_specular[0] = params[0];
        g_gl_state.material_specular[1] = params[1];
        g_gl_state.material_specular[2] = params[2];
        g_gl_state.material_specular[3] = params[3];
        break;
    case GL_EMISSION:
        break;
    case GL_SHININESS:
        g_gl_state.material_shininess = params[0];
        break;
    default:
        break;
    }
}

void gl_light_modeli(GLenum pname, GLint param)
{
    (void)pname;
    (void)param;
}

void gl_light_modelf(GLenum pname, GLfloat param)
{
    (void)pname;
    (void)param;
}

void gl_light_modelfv(GLenum pname, const GLfloat *params)
{
    if (!params) return;

    switch (pname) {
    case GL_LIGHT_MODEL_AMBIENT:
        g_gl_state.global_ambient[0] = params[0];
        g_gl_state.global_ambient[1] = params[1];
        g_gl_state.global_ambient[2] = params[2];
        g_gl_state.global_ambient[3] = params[3];
        break;
    default:
        break;
    }
}

void gl_shade_model(GLenum mode)
{
    (void)mode;
}
