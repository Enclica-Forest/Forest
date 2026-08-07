#include "math.h"
#include "../include/libc/math.h"

#define DEG2RAD(a) ((a) * 0.017453292519943295f)

mat4_t mat4_identity(void) {
    mat4_t r = {{0}};
    r.m[0] = 1.0f; r.m[5] = 1.0f; r.m[10] = 1.0f; r.m[15] = 1.0f;
    return r;
}

mat4_t mat4_multiply(mat4_t a, mat4_t b) {
    mat4_t r = {{0}};
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = sum;
        }
    }
    return r;
}

mat4_t mat4_translate(float x, float y, float z) {
    mat4_t r = mat4_identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

mat4_t mat4_scale(float x, float y, float z) {
    mat4_t r = {{0}};
    r.m[0] = x; r.m[5] = y; r.m[10] = z; r.m[15] = 1.0f;
    return r;
}

mat4_t mat4_rotate_x(float angle_deg) {
    float rad = DEG2RAD(angle_deg);
    float c = cosf(rad);
    float s = sinf(rad);
    mat4_t r = mat4_identity();
    r.m[5] = c;  r.m[6] = s;
    r.m[9] = -s; r.m[10] = c;
    return r;
}

mat4_t mat4_rotate_y(float angle_deg) {
    float rad = DEG2RAD(angle_deg);
    float c = cosf(rad);
    float s = sinf(rad);
    mat4_t r = mat4_identity();
    r.m[0] = c;  r.m[2] = -s;
    r.m[8] = s;  r.m[10] = c;
    return r;
}

mat4_t mat4_rotate_z(float angle_deg) {
    float rad = DEG2RAD(angle_deg);
    float c = cosf(rad);
    float s = sinf(rad);
    mat4_t r = mat4_identity();
    r.m[0] = c;  r.m[1] = s;
    r.m[4] = -s; r.m[5] = c;
    return r;
}

mat4_t mat4_ortho(float left, float right, float bottom, float top,
                   float near_val, float far_val) {
    mat4_t r = {{0}};
    r.m[0]  =  2.0f / (right - left);
    r.m[5]  =  2.0f / (top - bottom);
    r.m[10] = -2.0f / (far_val - near_val);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = -(far_val + near_val) / (far_val - near_val);
    r.m[15] = 1.0f;
    return r;
}

mat4_t mat4_perspective(float fov_y_deg, float aspect, float near_val, float far_val) {
    float rad = DEG2RAD(fov_y_deg);
    float f = 1.0f / tanf(rad * 0.5f);
    mat4_t r = {{0}};
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (far_val + near_val) / (near_val - far_val);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * far_val * near_val) / (near_val - far_val);
    return r;
}

mat4_t mat4_look_at(vec3_t eye, vec3_t center, vec3_t up) {
    vec3_t f = vec3_normalize(vec3_sub(center, eye));
    vec3_t s = vec3_normalize(vec3_cross(f, up));
    vec3_t u = vec3_cross(s, f);

    mat4_t r = mat4_identity();
    r.m[0]  =  s.x; r.m[4]  =  s.y; r.m[8]  =  s.z;
    r.m[1]  =  u.x; r.m[5]  =  u.y; r.m[9]  =  u.z;
    r.m[2]  = -f.x; r.m[6]  = -f.y; r.m[10] = -f.z;
    r.m[12] = -vec3_dot(s, eye);
    r.m[13] = -vec3_dot(u, eye);
    r.m[14] =  vec3_dot(f, eye);
    return r;
}

mat4_t mat4_transpose(mat4_t m) {
    mat4_t r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i * 4 + j] = m.m[j * 4 + i];
    return r;
}

vec4_t mat4_multiply_vec4(mat4_t m, vec4_t v) {
    vec4_t r;
    r.x = m.m[0]*v.x + m.m[4]*v.y + m.m[8]*v.z  + m.m[12]*v.w;
    r.y = m.m[1]*v.x + m.m[5]*v.y + m.m[9]*v.z  + m.m[13]*v.w;
    r.z = m.m[2]*v.x + m.m[6]*v.y + m.m[10]*v.z + m.m[14]*v.w;
    r.w = m.m[3]*v.x + m.m[7]*v.y + m.m[11]*v.z + m.m[15]*v.w;
    return r;
}

mat4_t mat4_invert(mat4_t m) {
    float* a = m.m;
    mat4_t r = {{0}};

    r.m[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15]
           + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    r.m[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15]
           - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    r.m[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15]
           + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    r.m[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14]
            - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    r.m[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15]
           - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    r.m[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15]
           + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    r.m[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15]
           - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    r.m[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14]
            + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    r.m[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15]
           + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    r.m[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15]
           - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    r.m[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15]
            + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    r.m[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14]
            - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    r.m[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11]
           - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    r.m[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11]
           + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    r.m[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11]
            - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    r.m[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10]
            + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];

    float det = a[0]*r.m[0] + a[1]*r.m[4] + a[2]*r.m[8] + a[3]*r.m[12];
    if (det == 0.0f) return mat4_identity();

    float inv_det = 1.0f / det;
    for (int i = 0; i < 16; i++) r.m[i] *= inv_det;
    return r;
}

vec3_t vec3_normalize(vec3_t v) {
    float len = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    if (len == 0.0f) return v;
    float inv = 1.0f / len;
    return (vec3_t){v.x*inv, v.y*inv, v.z*inv};
}

vec3_t vec3_cross(vec3_t a, vec3_t b) {
    return (vec3_t){
        a.y*b.z - a.z*b.y,
        a.z*b.x - a.x*b.z,
        a.x*b.y - a.y*b.x
    };
}

float vec3_dot(vec3_t a, vec3_t b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

vec3_t vec3_sub(vec3_t a, vec3_t b) {
    return (vec3_t){a.x-b.x, a.y-b.y, a.z-b.z};
}

vec3_t vec3_add(vec3_t a, vec3_t b) {
    return (vec3_t){a.x+b.x, a.y+b.y, a.z+b.z};
}

vec3_t vec3_scale(vec3_t v, float s) {
    return (vec3_t){v.x*s, v.y*s, v.z*s};
}
