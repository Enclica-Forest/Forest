#ifndef GL_DISPLAYLIST_H
#define GL_DISPLAYLIST_H

#include "../include/types.h"
#include "state.h"

#define GL_MAX_DISPLAY_LISTS 256

typedef enum {
    DL_CMD_VERTEX,
    DL_CMD_COLOR,
    DL_CMD_NORMAL,
    DL_CMD_TEXCOORD,
    DL_CMD_BEGIN,
    DL_CMD_END,
    DL_CMD_ENABLE,
    DL_CMD_DISABLE,
    DL_CMD_BIND_TEXTURE,
    DL_CMD_MATRIX,
    DL_CMD_END_LIST,
} dl_cmd_type_t;

typedef struct {
    dl_cmd_type_t type;
    union {
        struct { float x, y, z; } vertex;
        struct { float r, g, b, a; } color;
        struct { float x, y, z; } normal;
        struct { float u, v; } texcoord;
        struct { GLenum cap; } enable_disable;
        struct { GLuint texture; } bind_texture;
        struct { GLenum mode; float x, y, z; } translate;
        struct { GLenum mode; float angle, x, y, z; } rotate;
        struct { GLenum mode; float x, y, z; } scale;
        struct { GLenum mode; float m[16]; } matrix;
    } data;
} dl_cmd_t;

typedef struct {
    GLuint name;
    dl_cmd_t *commands;
    int command_count;
    int capacity;
    GLboolean used;
} gl_display_list_t;

void gl_display_list_init(void);

GLuint glGenLists(GLsizei range);
void glNewList(GLuint list, GLenum mode);
void glEndList(void);
void glCallList(GLuint list);
void glDeleteLists(GLuint list, GLsizei range);
GLboolean glIsList(GLuint list);

void gl_dl_record_begin(GLenum mode);
void gl_dl_record_end(void);
void gl_dl_record_vertex(float x, float y, float z);
void gl_dl_record_color(float r, float g, float b, float a);
void gl_dl_record_normal(float x, float y, float z);
void gl_dl_record_texcoord(float u, float v);
void gl_dl_record_enable(GLenum cap);
void gl_dl_record_disable(GLenum cap);
void gl_dl_record_bind_texture(GLuint texture);
void gl_dl_record_translate(GLenum mode, float x, float y, float z);
void gl_dl_record_rotate(GLenum mode, float angle, float x, float y, float z);
void gl_dl_record_scale(GLenum mode, float x, float y, float z);
void gl_dl_record_load_matrix(GLenum mode, const float *m);
void gl_dl_record_mult_matrix(GLenum mode, const float *m);

GLboolean gl_dl_is_recording(void);

#endif
