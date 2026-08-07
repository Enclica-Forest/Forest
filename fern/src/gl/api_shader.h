#ifndef GL_API_SHADER_H
#define GL_API_SHADER_H

#include "state.h"

GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint shader, GLsizei count, const GLchar **string, const GLint *length);
void glCompileShader(GLuint shader);
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
void glGetShaderInfoLog(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog);
void glDeleteShader(GLuint shader);

GLuint glCreateProgram(void);
void glAttachShader(GLuint program, GLuint shader);
void glLinkProgram(GLuint program);
void glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void glGetProgramInfoLog(GLuint program, GLsizei maxLength, GLsizei *length, GLchar *infoLog);
void glUseProgram(GLuint program);
void glDeleteProgram(GLuint program);

void gl_shader_init(void);

#endif
