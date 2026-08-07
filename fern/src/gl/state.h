#ifndef GL_STATE_H
#define GL_STATE_H

#include "math.h"

#define GL_DEPTH_TEST    0x0B71
#define GL_BLEND         0x0BE2
#define GL_CULL_FACE     0x0B44
#define GL_SCISSOR_TEST  0x0C11
#define GL_STENCIL_TEST  0x0B90
#define GL_TEXTURE_2D    0x0DE1
#define GL_LIGHTING      0x0B50
#define GL_LIGHT0        0x4000
#define GL_NORMALIZE     0x0BA1
#define GL_COLOR_MATERIAL 0x0B57
#define GL_FOG           0x0B60
#define GL_FOG_MODE      0x0B61
#define GL_FOG_DENSITY   0x0B62
#define GL_FOG_START     0x0B63
#define GL_FOG_END       0x0B64
#define GL_FOG_COLOR     0x0B66
#define GL_ALPHA_TEST    0x0BC0
#define GL_COLOR_LOGIC_OP 0x0BF2

#define GL_TRUE  1
#define GL_FALSE 0

#define GL_NEVER    0x0200
#define GL_LESS     0x0201
#define GL_EQUAL    0x0202
#define GL_LEQUAL   0x0203
#define GL_GREATER  0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL   0x0206
#define GL_ALWAYS   0x0207

#define GL_ZERO                0
#define GL_ONE                 1
#define GL_SRC_COLOR           0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA           0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA           0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR           0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307

#define GL_POINTS         0x0000
#define GL_LINES          0x0001
#define GL_LINE_LOOP      0x0002
#define GL_LINE_STRIP     0x0003
#define GL_TRIANGLES      0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN   0x0006

#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_NEAREST            0x2600
#define GL_LINEAR             0x2601
#define GL_REPEAT             0x2901
#define GL_EXP               0x0800
#define GL_EXP2              0x0801
#define GL_CLAMP_TO_EDGE      0x812F
#define GL_TEXTURE0           0x84C0
#define GL_TEXTURE1           0x84C1
#define GL_TEXTURE2           0x84C2
#define GL_TEXTURE3           0x84C3
#define GL_TEXTURE4           0x84C4
#define GL_TEXTURE5           0x84C5
#define GL_TEXTURE6           0x84C6
#define GL_TEXTURE7           0x84C7
#define GL_TEXTURE_WRAP_S     0x2802
#define GL_TEXTURE_WRAP_T     0x2803

#define GL_COLOR_BUFFER_BIT   0x00004000
#define GL_DEPTH_BUFFER_BIT   0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400

#define GL_BYTE           0x1400
#define GL_UNSIGNED_BYTE  0x1401
#define GL_SHORT          0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT            0x1404
#define GL_UNSIGNED_INT   0x1405
#define GL_FLOAT          0x1406

#define GL_RED            0x1903
#define GL_RGB            0x1907
#define GL_RGBA           0x1908

#define GL_ARRAY_BUFFER         0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW          0x88E4
#define GL_DYNAMIC_DRAW         0x88E8

#define GL_FRONT          0x0404
#define GL_BACK           0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW             0x0900
#define GL_CCW            0x0901

#define GL_CLEAR          0x1500
#define GL_SET            0x150F
#define GL_COPY           0x1503
#define GL_COPY_INVERTED  0x150C
#define GL_NOOP           0x1505
#define GL_INVERT         0x150A
#define GL_AND            0x1501
#define GL_NAND           0x150E
#define GL_OR             0x1507
#define GL_NOR            0x1508
#define GL_XOR            0x1506
#define GL_EQUIV          0x1509
#define GL_AND_REVERSE    0x1502
#define GL_AND_INVERTED   0x1504
#define GL_OR_REVERSE     0x150B
#define GL_OR_INVERTED    0x150D

#define GL_MODELVIEW  0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE    0x1702

#define GL_VERTEX_ARRAY         0x8074
#define GL_COLOR_ARRAY          0x8076
#define GL_NORMAL_ARRAY         0x8075
#define GL_TEXTURE_COORD_ARRAY  0x8078

#define GL_STACK_OVERFLOW  0x0503
#define GL_STACK_UNDERFLOW 0x0504

#define GL_VENDOR   0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION  0x1F02

#define GL_STENCIL_KEEP    0x1E00
#define GL_STENCIL_ZERO    0
#define GL_STENCIL_REPLACE 0x1E01
#define GL_STENCIL_INCR    0x1E02
#define GL_STENCIL_DECR    0x1E03
#define GL_STENCIL_INVERT  0x150A
#define GL_STENCIL_INCR_WRAP 0x8507
#define GL_STENCIL_DECR_WRAP 0x8508

#define GL_POLYGON_OFFSET_FACTOR  0x8038
#define GL_POLYGON_OFFSET_UNITS   0x2A00

#define GL_STACK_MAX_DEPTH 32

#define GL_TEXTURE_ENV          0x2300
#define GL_TEXTURE_ENV_MODE     0x2200
#define GL_TEXTURE_ENV_COLOR    0x2201

#define GL_MODULATE             0x2100
#define GL_TEX_REPLACE          0x2101
#define GL_ADD                  0x0104
#define GL_ADD_SIGNED           0x8574
#define GL_INTERPOLATE          0x8575
#define GL_SUBTRACT             0x84E7
#define GL_DOT3_RGB             0x86AE
#define GL_DOT3_RGBA            0x86AF
#define GL_DECAL                0x2109
#define GL_COMBINE              0x8570
#define GL_COMBINE_RGB          0x8571
#define GL_COMBINE_ALPHA        0x8572
#define GL_SRC0_RGB             0x8580
#define GL_SRC1_RGB             0x8581
#define GL_SRC2_RGB             0x8582
#define GL_SRC0_ALPHA           0x8588
#define GL_SRC1_ALPHA           0x8589
#define GL_SRC2_ALPHA           0x858A
#define GL_OPERAND0_RGB         0x8590
#define GL_OPERAND1_RGB         0x8591
#define GL_OPERAND2_RGB         0x8592
#define GL_OPERAND0_ALPHA       0x8598
#define GL_OPERAND1_ALPHA       0x8599
#define GL_OPERAND2_ALPHA       0x859A
#define GL_RGB_SCALE            0x8573
#define GL_ALPHA_SCALE          0x0D1C
#define GL_CONSTANT             0x8576
#define GL_PRIMARY_COLOR        0x8577
#define GL_PREVIOUS             0x8578

typedef unsigned int GLboolean;
typedef unsigned int GLenum;
typedef int GLint;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef double GLdouble;
typedef unsigned char GLubyte;
typedef char GLbyte;
typedef short GLshort;
typedef unsigned short GLushort;
typedef void GLvoid;
typedef long GLsizeiptr;
typedef long GLintptr;
typedef float GLclampf;
typedef char GLchar;

#define GL_IMMEDIATE_MAX_VERTICES 4096

typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float s, t, r_tex;
    float nx, ny, nz;
} gl_immediate_vertex_t;

typedef struct {
    gl_immediate_vertex_t vertices[GL_IMMEDIATE_MAX_VERTICES];
    int count;
} gl_immediate_buffer_t;

typedef struct {
    GLint viewport[4];

    GLfloat clear_color[4];
    GLdouble clear_depth;
    GLint clear_stencil;

    GLboolean depth_test;
    GLboolean blend;
    GLboolean cull_face;
    GLboolean scissor_test;
    GLboolean stencil_test;
    GLboolean texture_2d;
    GLboolean lighting;
    GLboolean normalize;
    GLboolean color_material;
    GLboolean light_enabled[8];

    GLboolean fog_enabled;
    GLenum fog_mode;
    GLfloat fog_color[4];
    GLfloat fog_start;
    GLfloat fog_end;
    GLfloat fog_density;

    GLboolean alpha_test_enabled;
    GLenum alpha_func;
    GLfloat alpha_ref;

    GLboolean color_logic_op_enabled;
    GLenum logic_op;

    GLenum depth_func;

    GLenum blend_src;
    GLenum blend_dst;

    GLenum cull_face_mode;
    GLenum front_face;

    GLint scissor_box[4];

    GLenum stencil_func;
    GLint  stencil_ref;
    GLuint stencil_val_mask;
    GLuint stencil_write_mask;
    GLenum stencil_sfail;
    GLenum stencil_dpfail;
    GLenum stencil_dppass;

    GLfloat polygon_offset_factor;
    GLfloat polygon_offset_units;

    GLuint active_texture;
    GLuint bound_textures[8];

    GLenum tex_env_mode[8];
    GLfloat tex_env_color[8][4];
    GLfloat multi_texcoord[8][2];

    mat4_t modelview_matrix;
    mat4_t projection_matrix;
    mat4_t texture_matrix[8];
    mat4_t *matrix_mode_ptr;

    mat4_t modelview_stack[GL_STACK_MAX_DEPTH];
    mat4_t projection_stack[GL_STACK_MAX_DEPTH];
    mat4_t texture_stack[8][GL_STACK_MAX_DEPTH];
    int modelview_stack_top;
    int projection_stack_top;
    int texture_stack_top[8];

    GLfloat current_color[4];
    GLfloat current_normal[3];
    GLfloat current_texcoord[2];

    GLfloat light_ambient[8][4];
    GLfloat light_diffuse[8][4];
    GLfloat light_specular[8][4];
    GLfloat light_position[8][4];
    GLfloat material_ambient[4];
    GLfloat material_diffuse[4];
    GLfloat material_specular[4];
    GLfloat material_shininess;
    GLfloat global_ambient[4];

    mat3_t normal_matrix;
    GLboolean normal_matrix_dirty;

    GLboolean vertex_array_enabled;
    GLboolean color_array_enabled;
    GLboolean normal_array_enabled;
    GLboolean texcoord_array_enabled;

    const GLvoid *vertex_array_pointer;
    GLint   vertex_array_size;
    GLenum  vertex_array_type;
    GLsizei vertex_array_stride;

    const GLvoid *color_array_pointer;
    GLint   color_array_size;
    GLenum  color_array_type;
    GLsizei color_array_stride;

    const GLvoid *normal_array_pointer;
    GLenum  normal_array_type;
    GLsizei normal_array_stride;

    const GLvoid *texcoord_array_pointer;
    GLint   texcoord_array_size;
    GLenum  texcoord_array_type;
    GLsizei texcoord_array_stride;

    GLuint current_program;

    GLuint bound_array_buffer;
    GLuint bound_element_buffer;

    gl_immediate_buffer_t immediate;

    GLboolean dl_recording;
    GLuint dl_current_list;
} gl_state_t;

extern gl_state_t g_gl_state;
void gl_state_init(void);

void gl_immediate_init(void);
void gl_immediate_flush(void);

#endif
