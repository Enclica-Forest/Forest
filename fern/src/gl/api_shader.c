#include "api_shader.h"
#include "../include/debuglog.h"
#include "../include/string.h"

extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);

#define GL_SHADER_POOL_SIZE 64
#define GL_PROGRAM_POOL_SIZE 32
#define GL_SHADER_SOURCE_MAX 4096

#define GL_VERTEX_SHADER   0x8B31
#define GL_FRAGMENT_SHADER 0x8B30

#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84

typedef struct {
    GLuint name;
    GLenum type;
    char *source;
    GLint compiled;
    GLboolean used;
} gl_shader_t;

typedef struct {
    GLuint name;
    GLuint attached_shaders[8];
    int attached_count;
    GLint linked;
    GLboolean used;
} gl_program_t;

static gl_shader_t g_shader_pool[GL_SHADER_POOL_SIZE];
static gl_program_t g_program_pool[GL_PROGRAM_POOL_SIZE];
static GLuint g_shader_next_name = 1;
static GLuint g_program_next_name = 1;

void gl_shader_init(void)
{
    memset(g_shader_pool, 0, sizeof(g_shader_pool));
    memset(g_program_pool, 0, sizeof(g_program_pool));
    g_shader_next_name = 1;
    g_program_next_name = 1;
}

static gl_shader_t *shader_find(GLuint name)
{
    for (int i = 0; i < GL_SHADER_POOL_SIZE; i++) {
        if (g_shader_pool[i].used && g_shader_pool[i].name == name)
            return &g_shader_pool[i];
    }
    return NULL;
}

static gl_shader_t *shader_alloc(void)
{
    for (int i = 0; i < GL_SHADER_POOL_SIZE; i++) {
        if (!g_shader_pool[i].used) {
            g_shader_pool[i].used = GL_TRUE;
            g_shader_pool[i].name = g_shader_next_name++;
            return &g_shader_pool[i];
        }
    }
    return NULL;
}

static gl_program_t *program_find(GLuint name)
{
    for (int i = 0; i < GL_PROGRAM_POOL_SIZE; i++) {
        if (g_program_pool[i].used && g_program_pool[i].name == name)
            return &g_program_pool[i];
    }
    return NULL;
}

static gl_program_t *program_alloc(void)
{
    for (int i = 0; i < GL_PROGRAM_POOL_SIZE; i++) {
        if (!g_program_pool[i].used) {
            g_program_pool[i].used = GL_TRUE;
            g_program_pool[i].name = g_program_next_name++;
            return &g_program_pool[i];
        }
    }
    return NULL;
}

GLuint glCreateShader(GLenum type)
{
    gl_shader_t *s = shader_alloc();
    if (!s) {
        debuglog(DEBUG_ERROR, "[GL_SHADER] pool exhausted\n");
        return 0;
    }
    s->type = type;
    s->source = NULL;
    s->compiled = GL_TRUE;
    return s->name;
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar **string, const GLint *length)
{
    gl_shader_t *s = shader_find(shader);
    if (!s) return;

    if (s->source) {
        kfree(s->source);
        s->source = NULL;
    }

    int total = 0;
    for (GLsizei i = 0; i < count; i++) {
        if (length && length[i] >= 0)
            total += length[i];
        else {
            const GLchar *p = string[i];
            while (*p) { total++; p++; }
        }
    }

    if (total <= 0) return;
    if (total >= GL_SHADER_SOURCE_MAX) total = GL_SHADER_SOURCE_MAX - 1;

    s->source = (char *)kmalloc((size_t)(total + 1));
    if (!s->source) return;

    int pos = 0;
    for (GLsizei i = 0; i < count; i++) {
        int len;
        if (length && length[i] >= 0)
            len = length[i];
        else {
            len = 0;
            const GLchar *p = string[i];
            while (*p) { len++; p++; }
        }
        if (pos + len >= GL_SHADER_SOURCE_MAX) {
            len = GL_SHADER_SOURCE_MAX - 1 - pos;
        }
        memcpy(s->source + pos, string[i], (size_t)len);
        pos += len;
    }
    s->source[pos] = '\0';
}

void glCompileShader(GLuint shader)
{
    gl_shader_t *s = shader_find(shader);
    if (!s) return;
    s->compiled = GL_TRUE;
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
    gl_shader_t *s = shader_find(shader);
    if (!s || !params) return;

    switch (pname) {
    case GL_COMPILE_STATUS:
        *params = s->compiled;
        break;
    case GL_INFO_LOG_LENGTH:
        *params = 0;
        break;
    case 0x8B85: /* GL_SHADER_TYPE */
        *params = s->type;
        break;
    default:
        *params = 0;
        break;
    }
}

void glGetShaderInfoLog(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog)
{
    (void)shader;
    if (length) *length = 0;
    if (maxLength > 0 && infoLog) infoLog[0] = '\0';
}

void glDeleteShader(GLuint shader)
{
    gl_shader_t *s = shader_find(shader);
    if (!s) return;
    if (s->source) {
        kfree(s->source);
        s->source = NULL;
    }
    s->used = GL_FALSE;
    s->name = 0;
}

GLuint glCreateProgram(void)
{
    gl_program_t *p = program_alloc();
    if (!p) {
        debuglog(DEBUG_ERROR, "[GL_SHADER] program pool exhausted\n");
        return 0;
    }
    p->attached_count = 0;
    p->linked = GL_TRUE;
    return p->name;
}

void glAttachShader(GLuint program, GLuint shader)
{
    gl_program_t *p = program_find(program);
    if (!p || p->attached_count >= 8) return;
    p->attached_shaders[p->attached_count++] = shader;
}

void glLinkProgram(GLuint program)
{
    gl_program_t *p = program_find(program);
    if (!p) return;
    p->linked = GL_TRUE;
}

void glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
    gl_program_t *p = program_find(program);
    if (!p || !params) return;

    switch (pname) {
    case GL_LINK_STATUS:
        *params = p->linked;
        break;
    case GL_INFO_LOG_LENGTH:
        *params = 0;
        break;
    default:
        *params = 0;
        break;
    }
}

void glGetProgramInfoLog(GLuint program, GLsizei maxLength, GLsizei *length, GLchar *infoLog)
{
    (void)program;
    if (length) *length = 0;
    if (maxLength > 0 && infoLog) infoLog[0] = '\0';
}

void glUseProgram(GLuint program)
{
    g_gl_state.current_program = program;
}

void glDeleteProgram(GLuint program)
{
    gl_program_t *p = program_find(program);
    if (!p) return;
    p->used = GL_FALSE;
    p->name = 0;
    if (g_gl_state.current_program == program)
        g_gl_state.current_program = 0;
}
