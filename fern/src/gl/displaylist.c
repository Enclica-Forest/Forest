#include "displaylist.h"
#include "api_immediate.h"
#include "api_matrix.h"
#include "api_state.h"
#include "api_texture.h"
#include <string.h>
#include <math.h>

extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);

#define DL_INITIAL_CAPACITY 64

static gl_display_list_t g_display_lists[GL_MAX_DISPLAY_LISTS];

void gl_display_list_init(void)
{
    memset(g_display_lists, 0, sizeof(g_display_lists));
    g_gl_state.dl_current_list = 0;
    g_gl_state.dl_recording = GL_FALSE;
}

static gl_display_list_t *find_list(GLuint name)
{
    for (int i = 0; i < GL_MAX_DISPLAY_LISTS; i++) {
        if (g_display_lists[i].used && g_display_lists[i].name == name)
            return &g_display_lists[i];
    }
    return (void *)0;
}

static gl_display_list_t *alloc_list(GLuint name)
{
    for (int i = 0; i < GL_MAX_DISPLAY_LISTS; i++) {
        if (!g_display_lists[i].used) {
            g_display_lists[i].name = name;
            g_display_lists[i].used = GL_TRUE;
            g_display_lists[i].command_count = 0;
            g_display_lists[i].capacity = DL_INITIAL_CAPACITY;
            g_display_lists[i].commands = (dl_cmd_t *)kmalloc(
                DL_INITIAL_CAPACITY * sizeof(dl_cmd_t));
            if (g_display_lists[i].commands)
                memset(g_display_lists[i].commands, 0,
                       DL_INITIAL_CAPACITY * sizeof(dl_cmd_t));
            return &g_display_lists[i];
        }
    }
    return (void *)0;
}

static void free_list(gl_display_list_t *list)
{
    if (list->commands) {
        kfree(list->commands);
        list->commands = (void *)0;
    }
    list->used = GL_FALSE;
    list->command_count = 0;
    list->capacity = 0;
}

static void record_command(dl_cmd_type_t type)
{
    gl_display_list_t *list = find_list(g_gl_state.dl_current_list);
    if (!list) return;

    if (list->command_count >= list->capacity) {
        int new_cap = list->capacity * 2;
        dl_cmd_t *new_buf = (dl_cmd_t *)kmalloc(new_cap * sizeof(dl_cmd_t));
        if (!new_buf) return;
        memcpy(new_buf, list->commands, list->command_count * sizeof(dl_cmd_t));
        memset(new_buf + list->capacity, 0,
               (new_cap - list->capacity) * sizeof(dl_cmd_t));
        kfree(list->commands);
        list->commands = new_buf;
        list->capacity = new_cap;
    }

    list->commands[list->command_count].type = type;
    list->command_count++;
}

static dl_cmd_t *last_command(void)
{
    gl_display_list_t *list = find_list(g_gl_state.dl_current_list);
    if (!list || list->command_count == 0) return (void *)0;
    return &list->commands[list->command_count - 1];
}

GLuint glGenLists(GLsizei range)
{
    if (range <= 0) return 0;

    GLuint base = 1;
    for (int i = 0; i < GL_MAX_DISPLAY_LISTS; i++) {
        if (!g_display_lists[i].used) {
            GLboolean conflict = GL_FALSE;
            for (GLsizei r = 0; r < range; r++) {
                if (find_list(base + r)) {
                    conflict = GL_TRUE;
                    break;
                }
            }
            if (!conflict) {
                for (GLsizei r = 0; r < range; r++) {
                    gl_display_list_t *dl = alloc_list(base + r);
                    if (!dl) {
                        for (GLsizei f = 0; f < r; f++)
                            free_list(find_list(base + f));
                        return 0;
                    }
                }
                return base;
            }
        }
    }
    return 0;
}

void glNewList(GLuint list, GLenum mode)
{
    (void)mode;
    if (g_gl_state.dl_recording) return;
    if (list == 0) return;

    gl_display_list_t *dl = find_list(list);
    if (!dl) return;

    if (dl->commands) {
        kfree(dl->commands);
    }
    dl->command_count = 0;
    dl->capacity = DL_INITIAL_CAPACITY;
    dl->commands = (dl_cmd_t *)kmalloc(DL_INITIAL_CAPACITY * sizeof(dl_cmd_t));
    if (dl->commands)
        memset(dl->commands, 0, DL_INITIAL_CAPACITY * sizeof(dl_cmd_t));

    g_gl_state.dl_current_list = list;
    g_gl_state.dl_recording = GL_TRUE;
}

void glEndList(void)
{
    if (!g_gl_state.dl_recording) return;

    record_command(DL_CMD_END_LIST);
    g_gl_state.dl_recording = GL_FALSE;
    g_gl_state.dl_current_list = 0;
}

void glCallList(GLuint list)
{
    gl_display_list_t *dl = find_list(list);
    if (!dl) return;

    for (int i = 0; i < dl->command_count; i++) {
        dl_cmd_t *cmd = &dl->commands[i];
        switch (cmd->type) {
        case DL_CMD_VERTEX:
            gl_immediate_vertex(cmd->data.vertex.x,
                                cmd->data.vertex.y,
                                cmd->data.vertex.z);
            break;
        case DL_CMD_COLOR:
            gl_immediate_color(cmd->data.color.r,
                               cmd->data.color.g,
                               cmd->data.color.b,
                               cmd->data.color.a);
            break;
        case DL_CMD_NORMAL:
            gl_immediate_normal(cmd->data.normal.x,
                                cmd->data.normal.y,
                                cmd->data.normal.z);
            break;
        case DL_CMD_TEXCOORD:
            gl_immediate_texcoord(cmd->data.texcoord.u,
                                  cmd->data.texcoord.v);
            break;
        case DL_CMD_BEGIN:
            gl_immediate_begin(cmd->data.enable_disable.cap);
            break;
        case DL_CMD_END:
            gl_immediate_end();
            break;
        case DL_CMD_ENABLE:
            gl_enable(cmd->data.enable_disable.cap);
            break;
        case DL_CMD_DISABLE:
            gl_disable(cmd->data.enable_disable.cap);
            break;
        case DL_CMD_BIND_TEXTURE:
            gl_bind_texture(GL_TEXTURE_2D, cmd->data.bind_texture.texture);
            break;
        case DL_CMD_MATRIX:
            switch (cmd->data.matrix.mode) {
            case GL_MODELVIEW:
                g_gl_state.matrix_mode_ptr = &g_gl_state.modelview_matrix;
                break;
            case GL_PROJECTION:
                g_gl_state.matrix_mode_ptr = &g_gl_state.projection_matrix;
                break;
            case GL_TEXTURE:
                g_gl_state.matrix_mode_ptr =
                    &g_gl_state.texture_matrix[g_gl_state.active_texture - GL_TEXTURE0];
                break;
            }
            glLoadMatrixf(cmd->data.matrix.m);
            break;
        case DL_CMD_END_LIST:
            break;
        }
    }
}

void glDeleteLists(GLuint list, GLsizei range)
{
    for (GLsizei i = 0; i < range; i++) {
        gl_display_list_t *dl = find_list(list + i);
        if (dl) free_list(dl);
    }
}

GLboolean glIsList(GLuint list)
{
    return find_list(list) != (void *)0 ? GL_TRUE : GL_FALSE;
}

GLboolean gl_dl_is_recording(void)
{
    return g_gl_state.dl_recording;
}

void gl_dl_record_begin(GLenum mode)
{
    record_command(DL_CMD_BEGIN);
    dl_cmd_t *cmd = last_command();
    if (cmd) cmd->data.enable_disable.cap = mode;
}

void gl_dl_record_end(void)
{
    record_command(DL_CMD_END);
}

void gl_dl_record_vertex(float x, float y, float z)
{
    record_command(DL_CMD_VERTEX);
    dl_cmd_t *cmd = last_command();
    if (cmd) {
        cmd->data.vertex.x = x;
        cmd->data.vertex.y = y;
        cmd->data.vertex.z = z;
    }
}

void gl_dl_record_color(float r, float g, float b, float a)
{
    record_command(DL_CMD_COLOR);
    dl_cmd_t *cmd = last_command();
    if (cmd) {
        cmd->data.color.r = r;
        cmd->data.color.g = g;
        cmd->data.color.b = b;
        cmd->data.color.a = a;
    }
}

void gl_dl_record_normal(float x, float y, float z)
{
    record_command(DL_CMD_NORMAL);
    dl_cmd_t *cmd = last_command();
    if (cmd) {
        cmd->data.normal.x = x;
        cmd->data.normal.y = y;
        cmd->data.normal.z = z;
    }
}

void gl_dl_record_texcoord(float u, float v)
{
    record_command(DL_CMD_TEXCOORD);
    dl_cmd_t *cmd = last_command();
    if (cmd) {
        cmd->data.texcoord.u = u;
        cmd->data.texcoord.v = v;
    }
}

void gl_dl_record_enable(GLenum cap)
{
    record_command(DL_CMD_ENABLE);
    dl_cmd_t *cmd = last_command();
    if (cmd) cmd->data.enable_disable.cap = cap;
}

void gl_dl_record_disable(GLenum cap)
{
    record_command(DL_CMD_DISABLE);
    dl_cmd_t *cmd = last_command();
    if (cmd) cmd->data.enable_disable.cap = cap;
}

void gl_dl_record_bind_texture(GLuint texture)
{
    record_command(DL_CMD_BIND_TEXTURE);
    dl_cmd_t *cmd = last_command();
    if (cmd) cmd->data.bind_texture.texture = texture;
}

void gl_dl_record_translate(GLenum mode, float x, float y, float z)
{
    record_command(DL_CMD_MATRIX);
    dl_cmd_t *cmd = last_command();
    if (cmd) {
        cmd->data.matrix.mode = mode;
        mat4_t t;
        memset(&t, 0, sizeof(mat4_t));
        t.m[0] = 1.0f;
        t.m[5] = 1.0f;
        t.m[10] = 1.0f;
        t.m[15] = 1.0f;
        t.m[12] = x;
        t.m[13] = y;
        t.m[14] = z;
        mat4_t *current = (void *)0;
        switch (mode) {
        case GL_MODELVIEW:  current = &g_gl_state.modelview_matrix; break;
        case GL_PROJECTION: current = &g_gl_state.projection_matrix; break;
        case GL_TEXTURE:
            current = &g_gl_state.texture_matrix[g_gl_state.active_texture - GL_TEXTURE0];
            break;
        }
        if (current) {
            mat4_t result = mat4_multiply(*current, t);
            memcpy(cmd->data.matrix.m, result.m, sizeof(float) * 16);
        }
    }
}

void gl_dl_record_rotate(GLenum mode, float angle, float x, float y, float z)
{
    record_command(DL_CMD_MATRIX);
    dl_cmd_t *cmd = last_command();
    if (cmd) {
        cmd->data.matrix.mode = mode;
        mat4_t r;
        float rad = angle * 3.14159265358979323846f / 180.0f;
        float c = cosf(rad);
        float s = sinf(rad);
        float len = sqrtf(x * x + y * y + z * z);
        if (len < 1e-6f) { len = 1.0f; }
        x /= len;
        y /= len;
        z /= len;
        memset(&r, 0, sizeof(mat4_t));
        r.m[15] = 1.0f;
        r.m[0]  = x * x * (1.0f - c) + c;
        r.m[1]  = x * y * (1.0f - c) + z * s;
        r.m[2]  = x * z * (1.0f - c) - y * s;
        r.m[4]  = y * x * (1.0f - c) - z * s;
        r.m[5]  = y * y * (1.0f - c) + c;
        r.m[6]  = y * z * (1.0f - c) + x * s;
        r.m[8]  = z * x * (1.0f - c) + y * s;
        r.m[9]  = z * y * (1.0f - c) - x * s;
        r.m[10] = z * z * (1.0f - c) + c;
        mat4_t *current = (void *)0;
        switch (mode) {
        case GL_MODELVIEW:  current = &g_gl_state.modelview_matrix; break;
        case GL_PROJECTION: current = &g_gl_state.projection_matrix; break;
        case GL_TEXTURE:
            current = &g_gl_state.texture_matrix[g_gl_state.active_texture - GL_TEXTURE0];
            break;
        }
        if (current) {
            mat4_t result = mat4_multiply(*current, r);
            memcpy(cmd->data.matrix.m, result.m, sizeof(float) * 16);
        }
    }
}

void gl_dl_record_scale(GLenum mode, float x, float y, float z)
{
    record_command(DL_CMD_MATRIX);
    dl_cmd_t *cmd = last_command();
    if (cmd) {
        cmd->data.matrix.mode = mode;
        mat4_t s;
        memset(&s, 0, sizeof(mat4_t));
        s.m[0]  = x;
        s.m[5]  = y;
        s.m[10] = z;
        s.m[15] = 1.0f;
        mat4_t *current = (void *)0;
        switch (mode) {
        case GL_MODELVIEW:  current = &g_gl_state.modelview_matrix; break;
        case GL_PROJECTION: current = &g_gl_state.projection_matrix; break;
        case GL_TEXTURE:
            current = &g_gl_state.texture_matrix[g_gl_state.active_texture - GL_TEXTURE0];
            break;
        }
        if (current) {
            mat4_t result = mat4_multiply(*current, s);
            memcpy(cmd->data.matrix.m, result.m, sizeof(float) * 16);
        }
    }
}

void gl_dl_record_load_matrix(GLenum mode, const float *m)
{
    record_command(DL_CMD_MATRIX);
    dl_cmd_t *cmd = last_command();
    if (cmd) {
        cmd->data.matrix.mode = mode;
        memcpy(cmd->data.matrix.m, m, sizeof(float) * 16);
    }
}

void gl_dl_record_mult_matrix(GLenum mode, const float *m)
{
    record_command(DL_CMD_MATRIX);
    dl_cmd_t *cmd = last_command();
    if (cmd) {
        cmd->data.matrix.mode = mode;
        mat4_t src;
        memcpy(src.m, m, sizeof(float) * 16);
        mat4_t *current = (void *)0;
        switch (mode) {
        case GL_MODELVIEW:  current = &g_gl_state.modelview_matrix; break;
        case GL_PROJECTION: current = &g_gl_state.projection_matrix; break;
        case GL_TEXTURE:
            current = &g_gl_state.texture_matrix[g_gl_state.active_texture - GL_TEXTURE0];
            break;
        }
        if (current) {
            mat4_t result = mat4_multiply(*current, src);
            memcpy(cmd->data.matrix.m, result.m, sizeof(float) * 16);
        }
    }
}
