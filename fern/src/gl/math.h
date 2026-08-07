#ifndef GL_MATH_H
#define GL_MATH_H

typedef struct { float x, y, z, w; } vec4_t;
typedef struct { float x, y, z; } vec3_t;
typedef struct { float x, y; } vec2_t;
typedef struct { float m[16]; } mat4_t;
typedef struct { float m[9]; } mat3_t;

mat4_t mat4_identity(void);
mat4_t mat4_multiply(mat4_t a, mat4_t b);
mat4_t mat4_translate(float x, float y, float z);
mat4_t mat4_scale(float x, float y, float z);
mat4_t mat4_rotate_x(float angle_deg);
mat4_t mat4_rotate_y(float angle_deg);
mat4_t mat4_rotate_z(float angle_deg);
mat4_t mat4_ortho(float left, float right, float bottom, float top, float near_val, float far_val);
mat4_t mat4_perspective(float fov_y_deg, float aspect, float near_val, float far_val);
mat4_t mat4_look_at(vec3_t eye, vec3_t center, vec3_t up);
mat4_t mat4_invert(mat4_t m);
mat4_t mat4_transpose(mat4_t m);
vec4_t mat4_multiply_vec4(mat4_t m, vec4_t v);

vec3_t vec3_normalize(vec3_t v);
vec3_t vec3_cross(vec3_t a, vec3_t b);
float  vec3_dot(vec3_t a, vec3_t b);
vec3_t vec3_sub(vec3_t a, vec3_t b);
vec3_t vec3_add(vec3_t a, vec3_t b);
vec3_t vec3_scale(vec3_t v, float s);

#endif
