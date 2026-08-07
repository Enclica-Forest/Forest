#include "gl.h"
#include "test.h"
#include "math.h"
#include "rasterizer.h"
#include "texture.h"
#include "texture_sample.h"
#include "framebuffer.h"
#include "../include/debuglog.h"
#include <string.h>

extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);

#define TEST_PASS  1
#define TEST_FAIL  0

#define ASSERT_MSG(cond, msg) do { \
    if (!(cond)) { \
        debuglog(DEBUG_ERROR, "  ASSERT FAILED: %s (line %d)\n", msg, __LINE__); \
        return TEST_FAIL; \
    } \
} while(0)

#define ASSERT_NEAR(a, b, eps, msg) do { \
    float _diff = (a) - (b); \
    if (_diff < 0.0f) _diff = -_diff; \
    if (_diff > (eps)) { \
        debuglog(DEBUG_ERROR, "  ASSERT FAILED: %s (%f != %f, eps=%f, line %d)\n", \
                 msg, (double)(a), (double)(b), (double)(eps), __LINE__); \
        return TEST_FAIL; \
    } \
} while(0)

static void print_result(const char *name, int passed) {
    if (passed) {
        debuglog(DEBUG_INFO, "  [PASS] %s\n", name);
    } else {
        debuglog(DEBUG_ERROR, "  [FAIL] %s\n", name);
    }
}

static gl_framebuffer_t* create_test_fb(int w, int h) {
    gl_framebuffer_t *fb = (gl_framebuffer_t*)kmalloc(sizeof(gl_framebuffer_t));
    if (!fb) return 0;

    int stride = w;
    fb->color_buffer = (unsigned int*)kmalloc(stride * h * sizeof(unsigned int));
    fb->depth_buffer = (float*)kmalloc(stride * h * sizeof(float));
    fb->stencil_buffer = (unsigned char*)kmalloc(stride * h * sizeof(unsigned char));

    if (!fb->color_buffer || !fb->depth_buffer || !fb->stencil_buffer) {
        kfree(fb->color_buffer);
        kfree(fb->depth_buffer);
        kfree(fb->stencil_buffer);
        kfree(fb);
        return 0;
    }

    fb->width = w;
    fb->height = h;
    fb->stride = stride;
    return fb;
}

static void destroy_test_fb(gl_framebuffer_t *fb) {
    if (!fb) return;
    kfree(fb->color_buffer);
    kfree(fb->depth_buffer);
    kfree(fb->stencil_buffer);
    kfree(fb);
}

/* ============================================================
 * MATH TESTS
 * ============================================================ */

static int test_identity_multiply(void) {
    mat4_t I = mat4_identity();
    mat4_t R = mat4_multiply(I, I);

    for (int i = 0; i < 16; i++) {
        float expected = (i % 5 == 0) ? 1.0f : 0.0f;
        ASSERT_NEAR(R.m[i], expected, 0.0001f, "I*I diagonal");
    }
    return TEST_PASS;
}

static int test_translation(void) {
    mat4_t T = mat4_translate(1.0f, 2.0f, 3.0f);
    vec4_t p = {0.0f, 0.0f, 0.0f, 1.0f};
    vec4_t r = mat4_multiply_vec4(T, p);

    ASSERT_NEAR(r.x, 1.0f, 0.0001f, "translate x");
    ASSERT_NEAR(r.y, 2.0f, 0.0001f, "translate y");
    ASSERT_NEAR(r.z, 3.0f, 0.0001f, "translate z");
    ASSERT_NEAR(r.w, 1.0f, 0.0001f, "translate w");
    return TEST_PASS;
}

static int test_scale(void) {
    mat4_t S = mat4_scale(2.0f, 3.0f, 4.0f);
    vec4_t p = {1.0f, 1.0f, 1.0f, 1.0f};
    vec4_t r = mat4_multiply_vec4(S, p);

    ASSERT_NEAR(r.x, 2.0f, 0.0001f, "scale x");
    ASSERT_NEAR(r.y, 3.0f, 0.0001f, "scale y");
    ASSERT_NEAR(r.z, 4.0f, 0.0001f, "scale z");
    ASSERT_NEAR(r.w, 1.0f, 0.0001f, "scale w");
    return TEST_PASS;
}

static int test_rotation_z(void) {
    mat4_t R = mat4_rotate_z(90.0f);
    vec4_t p = {1.0f, 0.0f, 0.0f, 1.0f};
    vec4_t r = mat4_multiply_vec4(R, p);

    ASSERT_NEAR(r.x, 0.0f, 0.01f, "rotZ x");
    ASSERT_NEAR(r.y, 1.0f, 0.01f, "rotZ y");
    ASSERT_NEAR(r.z, 0.0f, 0.01f, "rotZ z");
    ASSERT_NEAR(r.w, 1.0f, 0.0001f, "rotZ w");
    return TEST_PASS;
}

static int test_perspective_ndc(void) {
    mat4_t P = mat4_perspective(90.0f, 1.0f, 0.1f, 100.0f);

    vec4_t near_center = {0.0f, 0.0f, -0.1f, 1.0f};
    vec4_t clip = mat4_multiply_vec4(P, near_center);

    if (clip.w == 0.0f) return TEST_FAIL;
    float ndc_x = clip.x / clip.w;
    float ndc_y = clip.y / clip.w;

    ASSERT_NEAR(ndc_x, 0.0f, 0.01f, "perspective ndc_x center");
    ASSERT_NEAR(ndc_y, 0.0f, 0.01f, "perspective ndc_y center");

    vec4_t far_point = {0.0f, 0.0f, -100.0f, 1.0f};
    clip = mat4_multiply_vec4(P, far_point);
    if (clip.w == 0.0f) return TEST_FAIL;
    ndc_x = clip.x / clip.w;
    ndc_y = clip.y / clip.w;
    ASSERT_NEAR(ndc_x, 0.0f, 0.05f, "perspective far ndc_x");
    ASSERT_NEAR(ndc_y, 0.0f, 0.05f, "perspective far ndc_y");

    return TEST_PASS;
}

static int test_matrix_inverse(void) {
    mat4_t M = mat4_translate(3.0f, -2.0f, 5.0f);
    mat4_t R = mat4_rotate_y(37.0f);
    mat4_t S = mat4_scale(1.5f, 0.7f, 2.2f);

    mat4_t A = mat4_multiply(M, R);
    A = mat4_multiply(A, S);

    mat4_t Ainv = mat4_invert(A);
    mat4_t I = mat4_multiply(A, Ainv);

    for (int i = 0; i < 16; i++) {
        float expected = (i % 5 == 0) ? 1.0f : 0.0f;
        ASSERT_NEAR(I.m[i], expected, 0.01f, "M*M^-1 identity");
    }
    return TEST_PASS;
}

static int test_vec3_dot(void) {
    vec3_t a = {1.0f, 2.0f, 3.0f};
    vec3_t b = {4.0f, 5.0f, 6.0f};
    float d = vec3_dot(a, b);
    ASSERT_NEAR(d, 32.0f, 0.0001f, "dot product");
    return TEST_PASS;
}

static int test_vec3_cross(void) {
    vec3_t a = {1.0f, 0.0f, 0.0f};
    vec3_t b = {0.0f, 1.0f, 0.0f};
    vec3_t c = vec3_cross(a, b);
    ASSERT_NEAR(c.x, 0.0f, 0.0001f, "cross x");
    ASSERT_NEAR(c.y, 0.0f, 0.0001f, "cross y");
    ASSERT_NEAR(c.z, 1.0f, 0.0001f, "cross z");
    return TEST_PASS;
}

static int test_vec3_normalize(void) {
    vec3_t v = {3.0f, 4.0f, 0.0f};
    vec3_t n = vec3_normalize(v);
    ASSERT_NEAR(n.x, 0.6f, 0.0001f, "normalize x");
    ASSERT_NEAR(n.y, 0.8f, 0.0001f, "normalize y");
    ASSERT_NEAR(n.z, 0.0f, 0.0001f, "normalize z");

    float len = n.x * n.x + n.y * n.y + n.z * n.z;
    ASSERT_NEAR(len, 1.0f, 0.0001f, "normalize length=1");
    return TEST_PASS;
}

int gl_test_math(void) {
    int pass = 1;

    int r;
    r = test_identity_multiply();  print_result("mat4 identity multiply", r);  if (!r) pass = 0;
    r = test_translation();        print_result("mat4 translate", r);          if (!r) pass = 0;
    r = test_scale();              print_result("mat4 scale", r);             if (!r) pass = 0;
    r = test_rotation_z();         print_result("mat4 rotate_z 90", r);       if (!r) pass = 0;
    r = test_perspective_ndc();    print_result("mat4 perspective NDC", r);    if (!r) pass = 0;
    r = test_matrix_inverse();     print_result("mat4 inverse M*M^-1=I", r);  if (!r) pass = 0;
    r = test_vec3_dot();           print_result("vec3 dot product", r);       if (!r) pass = 0;
    r = test_vec3_cross();         print_result("vec3 cross product", r);     if (!r) pass = 0;
    r = test_vec3_normalize();     print_result("vec3 normalize", r);         if (!r) pass = 0;

    return pass;
}

/* ============================================================
 * STATE TESTS
 * ============================================================ */

int gl_test_state(void) {
    int pass = 1;

    gl_enable(GL_DEPTH_TEST);
    ASSERT_MSG(g_gl_state.depth_test == GL_TRUE, "depth test enable");
    gl_disable(GL_DEPTH_TEST);
    ASSERT_MSG(g_gl_state.depth_test == GL_FALSE, "depth test disable");
    print_result("gl_enable/gl_disable depth", 1);

    gl_enable(GL_BLEND);
    ASSERT_MSG(g_gl_state.blend == GL_TRUE, "blend enable");
    gl_disable(GL_BLEND);
    ASSERT_MSG(g_gl_state.blend == GL_FALSE, "blend disable");
    print_result("gl_enable/gl_disable blend", 1);

    gl_clear_color(0.5f, 0.25f, 0.125f, 1.0f);
    ASSERT_NEAR(g_gl_state.clear_color[0], 0.5f, 0.001f, "clear_color r");
    ASSERT_NEAR(g_gl_state.clear_color[1], 0.25f, 0.001f, "clear_color g");
    ASSERT_NEAR(g_gl_state.clear_color[2], 0.125f, 0.001f, "clear_color b");
    print_result("gl_clear_color", 1);

    gl_depth_func(GL_ALWAYS);
    ASSERT_MSG(g_gl_state.depth_func == GL_ALWAYS, "depth_func always");
    gl_depth_func(GL_LESS);
    ASSERT_MSG(g_gl_state.depth_func == GL_LESS, "depth_func less");
    print_result("gl_depth_func", 1);

    gl_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ASSERT_MSG(g_gl_state.blend_src == GL_SRC_ALPHA, "blend_src");
    ASSERT_MSG(g_gl_state.blend_dst == GL_ONE_MINUS_SRC_ALPHA, "blend_dst");
    print_result("gl_blend_func", 1);

    gl_viewport(10, 20, 640, 480);
    ASSERT_MSG(g_gl_state.viewport[0] == 10, "viewport x");
    ASSERT_MSG(g_gl_state.viewport[1] == 20, "viewport y");
    ASSERT_MSG(g_gl_state.viewport[2] == 640, "viewport w");
    ASSERT_MSG(g_gl_state.viewport[3] == 480, "viewport h");
    print_result("gl_viewport", 1);

    return pass;
}

/* ============================================================
 * TEXTURE TESTS
 * ============================================================ */

int gl_test_texture(void) {
    int pass = 1;

    gl_texture_init();

    GLuint tex = gl_texture_create();
    ASSERT_MSG(tex != 0, "texture create");
    print_result("gl_texture_create", 1);

    gl_texture_bind(GL_TEXTURE_2D, tex);

    int w = 4, h = 4;
    unsigned char pixels[4 * 4 * 4];
    for (int i = 0; i < 4 * 4; i++) {
        pixels[i * 4 + 0] = 255;
        pixels[i * 4 + 1] = 0;
        pixels[i * 4 + 2] = 0;
        pixels[i * 4 + 3] = 255;
    }

    gl_texture_image2d(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    gl_texture_t *t = gl_texture_get(tex);
    ASSERT_MSG(t != 0, "texture get");
    ASSERT_MSG(t->width == w, "texture width");
    ASSERT_MSG(t->height == h, "texture height");
    ASSERT_MSG(t->data != 0, "texture data");
    print_result("gl_texture_image2d upload", 1);

    float rgba[4];
    gl_texel_fetch(t, 0, 0, rgba);
    ASSERT_NEAR(rgba[0], 1.0f, 0.01f, "texel R");
    ASSERT_NEAR(rgba[1], 0.0f, 0.01f, "texel G");
    ASSERT_NEAR(rgba[2], 0.0f, 0.01f, "texel B");
    ASSERT_NEAR(rgba[3], 1.0f, 0.01f, "texel A");
    print_result("gl_texel_fetch readback", 1);

    gl_texture_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_texture_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    ASSERT_MSG(t->min_filter == GL_NEAREST, "nearest min filter");
    ASSERT_MSG(t->mag_filter == GL_NEAREST, "nearest mag filter");
    print_result("GL_NEAREST filtering set", 1);

    gl_texture_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_texture_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ASSERT_MSG(t->min_filter == GL_LINEAR, "linear min filter");
    ASSERT_MSG(t->mag_filter == GL_LINEAR, "linear mag filter");
    print_result("GL_LINEAR filtering set", 1);

    gl_texture_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl_texture_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    ASSERT_MSG(t->wrap_s == GL_REPEAT, "wrap_s repeat");
    ASSERT_MSG(t->wrap_t == GL_REPEAT, "wrap_t repeat");
    print_result("GL_REPEAT wrap mode", 1);

    gl_texture_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl_texture_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ASSERT_MSG(t->wrap_s == GL_CLAMP_TO_EDGE, "wrap_s clamp");
    ASSERT_MSG(t->wrap_t == GL_CLAMP_TO_EDGE, "wrap_t clamp");
    print_result("GL_CLAMP_TO_EDGE wrap mode", 1);

    gl_texture_delete(tex);
    ASSERT_MSG(gl_texture_get(tex) == 0, "texture deleted");
    print_result("gl_texture_delete", 1);

    return pass;
}

/* ============================================================
 * RASTERIZER TESTS
 * ============================================================ */

int gl_test_rasterizer(void) {
    int pass = 1;
    int fb_w = 32, fb_h = 32;

    gl_framebuffer_t *fb = create_test_fb(fb_w, fb_h);
    ASSERT_MSG(fb != 0, "create test fb");
    gl_rasterizer_set_framebuffer(fb);

    gl_clear_color(0.0f, 0.0f, 0.0f, 1.0f);
    gl_clear_depth(1.0);
    gl_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    gl_state_init();
    gl_disable(GL_CULL_FACE);
    gl_disable(GL_DEPTH_TEST);

    gl_vertex_t v0, v1, v2;
    memset(&v0, 0, sizeof(gl_vertex_t));
    memset(&v1, 0, sizeof(gl_vertex_t));
    memset(&v2, 0, sizeof(gl_vertex_t));

    v0.clip_pos = (vec4_t){-0.5f, -0.5f, 0.0f, 1.0f};
    v0.color    = (vec4_t){1.0f, 0.0f, 0.0f, 1.0f};
    v0.eye_z    = 0.5f;

    v1.clip_pos = (vec4_t){ 0.5f, -0.5f, 0.0f, 1.0f};
    v1.color    = (vec4_t){0.0f, 1.0f, 0.0f, 1.0f};
    v1.eye_z    = 0.5f;

    v2.clip_pos = (vec4_t){ 0.0f,  0.5f, 0.0f, 1.0f};
    v2.color    = (vec4_t){0.0f, 0.0f, 1.0f, 1.0f};
    v2.eye_z    = 0.5f;

    gl_triangle_t tri = {v0, v1, v2};
    gl_rasterize_triangle(&tri);

    int filled = 0;
    int center_x = fb_w / 2;
    int center_y = fb_h / 2;
    unsigned int center_pixel = fb->color_buffer[center_y * fb->stride + center_x];
    unsigned char cr = (center_pixel >> 0) & 0xFF;
    unsigned char cg = (center_pixel >> 8) & 0xFF;
    unsigned char cb = (center_pixel >> 16) & 0xFF;

    filled = (cr > 0 || cg > 0 || cb > 0);
    print_result("triangle rasterized center pixel", filled);
    if (!filled) pass = 0;

    int corner_pixel = fb->color_buffer[0];
    unsigned char ccr = (corner_pixel >> 0) & 0xFF;
    unsigned char ccg = (corner_pixel >> 8) & 0xFF;
    unsigned char ccb = (corner_pixel >> 16) & 0xFF;
    int corner_empty = (ccr == 0 && ccg == 0 && ccb == 0);
    print_result("corner pixel outside triangle", corner_empty);
    if (!corner_empty) pass = 0;

    gl_rasterizer_set_framebuffer(0);
    destroy_test_fb(fb);
    return pass;
}

/* ============================================================
 * FRAMEBUFFER (FBO) TESTS
 * ============================================================ */

int gl_test_framebuffer(void) {
    int pass = 1;

    gl_framebuffer_init();

    GLuint fbo_name = gl_framebuffer_create();
    ASSERT_MSG(fbo_name != 0, "fbo create");
    print_result("gl_framebuffer_create", 1);

    gl_framebuffer_bind(fbo_name);

    GLuint tex = gl_texture_create();
    gl_texture_bind(GL_TEXTURE_2D, tex);

    int w = 16, h = 16;
    unsigned char clear_pixels[16 * 16 * 4];
    memset(clear_pixels, 0, sizeof(clear_pixels));
    gl_texture_image2d(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, clear_pixels);

    gl_framebuffer_texture2d(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, tex, 0);

    gl_fbo_t *fbo = gl_framebuffer_get_current();
    ASSERT_MSG(fbo != 0, "fbo get current");
    ASSERT_MSG(fbo->color.texture == tex, "fbo color attachment");
    print_result("gl_framebuffer_texture2d attach", 1);

    gl_framebuffer_bind(0);
    print_result("gl_framebuffer_bind unbind", 1);

    gl_texture_delete(tex);
    gl_framebuffer_delete(fbo_name);
    print_result("fbo cleanup", 1);

    return pass;
}

/* ============================================================
 * VERTEX TESTS
 * ============================================================ */

int gl_test_vertex(void) {
    int pass = 1;

    gl_vertex_t v;
    memset(&v, 0, sizeof(gl_vertex_t));
    v.clip_pos = (vec4_t){0.5f, 0.5f, 0.0f, 1.0f};
    v.color    = (vec4_t){1.0f, 1.0f, 1.0f, 1.0f};
    v.texcoord = (vec2_t){0.25f, 0.75f};
    v.normal   = (vec3_t){0.0f, 0.0f, 1.0f};
    v.eye_z    = -1.0f;

    ASSERT_NEAR(v.clip_pos.x, 0.5f, 0.0001f, "vertex clip_x");
    ASSERT_NEAR(v.texcoord.x, 0.25f, 0.0001f, "vertex tex_u");
    ASSERT_NEAR(v.normal.z, 1.0f, 0.0001f, "vertex normal_z");
    print_result("gl_vertex_t fields", 1);

    gl_triangle_t tri;
    tri.v[0] = v;
    tri.v[1] = v;
    tri.v[2] = v;
    ASSERT_NEAR(tri.v[0].clip_pos.x, 0.5f, 0.0001f, "triangle vertex");
    print_result("gl_triangle_t assembly", 1);

    return pass;
}

/* ============================================================
 * MATRIX STACK TESTS
 * ============================================================ */

int gl_test_matrix(void) {
    int pass = 1;

    gl_state_init();
    gl_immediate_init();

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    ASSERT_NEAR(g_gl_state.modelview_matrix.m[0], 1.0f, 0.0001f, "mv identity [0]");
    ASSERT_NEAR(g_gl_state.modelview_matrix.m[5], 1.0f, 0.0001f, "mv identity [5]");
    print_result("glMatrixMode MODELVIEW + glLoadIdentity", 1);

    glPushMatrix();
    glTranslatef(1.0f, 2.0f, 3.0f);
    ASSERT_NEAR(g_gl_state.modelview_matrix.m[12], 1.0f, 0.0001f, "mv translate x");
    ASSERT_NEAR(g_gl_state.modelview_matrix.m[13], 2.0f, 0.0001f, "mv translate y");
    ASSERT_NEAR(g_gl_state.modelview_matrix.m[14], 3.0f, 0.0001f, "mv translate z");
    print_result("glPushMatrix + glTranslatef", 1);

    glPopMatrix();
    ASSERT_NEAR(g_gl_state.modelview_matrix.m[12], 0.0f, 0.0001f, "mv pop x=0");
    ASSERT_NEAR(g_gl_state.modelview_matrix.m[13], 0.0f, 0.0001f, "mv pop y=0");
    ASSERT_NEAR(g_gl_state.modelview_matrix.m[14], 0.0f, 0.0001f, "mv pop z=0");
    print_result("glPopMatrix restores", 1);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    ASSERT_NEAR(g_gl_state.projection_matrix.m[0], 1.0f, 0.0001f, "proj identity");
    print_result("glMatrixMode PROJECTION + glLoadIdentity", 1);

    glPushMatrix();
    glTranslatef(5.0f, 6.0f, 7.0f);
    ASSERT_NEAR(g_gl_state.projection_matrix.m[12], 5.0f, 0.0001f, "proj translate x");
    print_result("glPushMatrix PROJECTION", 1);
    glPopMatrix();
    ASSERT_NEAR(g_gl_state.projection_matrix.m[12], 0.0f, 0.0001f, "proj pop x=0");
    print_result("glPopMatrix PROJECTION restores", 1);

    return pass;
}

/* ============================================================
 * IMMEDIATE MODE TESTS
 * ============================================================ */

int gl_test_immediate(void) {
    int pass = 1;
    int fb_w = 64, fb_h = 64;

    gl_state_init();
    gl_immediate_init();

    gl_framebuffer_t *fb = create_test_fb(fb_w, fb_h);
    ASSERT_MSG(fb != 0, "immediate test fb");
    gl_rasterizer_set_framebuffer(fb);

    gl_clear_color(0.0f, 0.0f, 0.0f, 1.0f);
    gl_clear_depth(1.0);
    gl_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    gl_viewport(0, 0, fb_w, fb_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    gl_disable(GL_CULL_FACE);
    gl_disable(GL_DEPTH_TEST);

    glBegin(GL_TRIANGLES);
        glColor3f(1.0f, 0.0f, 0.0f);
        glVertex3f(-0.8f, -0.8f, 0.0f);
        glColor3f(0.0f, 1.0f, 0.0f);
        glVertex3f( 0.8f, -0.8f, 0.0f);
        glColor3f(0.0f, 0.0f, 1.0f);
        glVertex3f( 0.0f,  0.8f, 0.0f);
    glEnd();

    int cx = fb_w / 2;
    int cy = fb_h / 2;
    unsigned int pixel = fb->color_buffer[cy * fb->stride + cx];
    unsigned char pr = (pixel >> 0) & 0xFF;
    unsigned char pg = (pixel >> 8) & 0xFF;
    unsigned char pb = (pixel >> 16) & 0xFF;
    int has_color = (pr > 0 || pg > 0 || pb > 0);
    print_result("glBegin/glEnd triangle rasterized", has_color);
    if (!has_color) pass = 0;

    unsigned int corner = fb->color_buffer[0];
    unsigned char ccr = (corner >> 0) & 0xFF;
    unsigned char ccg = (corner >> 8) & 0xFF;
    unsigned char ccb = (corner >> 16) & 0xFF;
    int corner_black = (ccr < 10 && ccg < 10 && ccb < 10);
    print_result("glBegin/glEnd corner outside triangle", corner_black);
    if (!corner_black) pass = 0;

    gl_rasterizer_set_framebuffer(0);
    destroy_test_fb(fb);
    return pass;
}

/* ============================================================
 * LIGHTING TESTS
 * ============================================================ */

int gl_test_lighting(void) {
    int pass = 1;

    gl_state_init();

    g_gl_state.global_ambient[0] = 0.2f;
    g_gl_state.global_ambient[1] = 0.2f;
    g_gl_state.global_ambient[2] = 0.2f;
    g_gl_state.global_ambient[3] = 1.0f;

    g_gl_state.material_ambient[0] = 0.3f;
    g_gl_state.material_ambient[1] = 0.3f;
    g_gl_state.material_ambient[2] = 0.3f;
    g_gl_state.material_ambient[3] = 1.0f;

    g_gl_state.material_diffuse[0] = 0.8f;
    g_gl_state.material_diffuse[1] = 0.0f;
    g_gl_state.material_diffuse[2] = 0.0f;
    g_gl_state.material_diffuse[3] = 1.0f;

    g_gl_state.light_enabled[0] = 1;
    g_gl_state.light_position[0][0] = 1.0f;
    g_gl_state.light_position[0][1] = 1.0f;
    g_gl_state.light_position[0][2] = 1.0f;
    g_gl_state.light_position[0][3] = 0.0f;

    g_gl_state.light_diffuse[0][0] = 1.0f;
    g_gl_state.light_diffuse[0][1] = 1.0f;
    g_gl_state.light_diffuse[0][2] = 1.0f;
    g_gl_state.light_diffuse[0][3] = 1.0f;

    g_gl_state.light_ambient[0][0] = 0.1f;
    g_gl_state.light_ambient[0][1] = 0.1f;
    g_gl_state.light_ambient[0][2] = 0.1f;
    g_gl_state.light_ambient[0][3] = 1.0f;

    float ex = 0.0f, ey = 0.0f, ez = 1.0f;
    float r, g, b;

    gl_compute_lighting(0.0f, 0.0f, 1.0f, &ex, &ey, &ez, &r, &g, &b);

    ASSERT_MSG(r > 0.1f, "lighting red > 0.1");
    ASSERT_MSG(r <= 1.0f, "lighting red <= 1.0");
    print_result("gl_compute_lighting lit color reasonable", 1);

    gl_state_init();
    g_gl_state.light_enabled[0] = 0;

    gl_compute_lighting(0.0f, 0.0f, 1.0f, &ex, &ey, &ez, &r, &g, &b);
    float expected_r = g_gl_state.global_ambient[0] * g_gl_state.material_ambient[0];
    float expected_g = g_gl_state.global_ambient[1] * g_gl_state.material_ambient[1];
    float expected_b = g_gl_state.global_ambient[2] * g_gl_state.material_ambient[2];
    ASSERT_NEAR(r, expected_r, 0.01f, "unlit ambient r");
    ASSERT_NEAR(g, expected_g, 0.01f, "unlit ambient g");
    ASSERT_NEAR(b, expected_b, 0.01f, "unlit ambient b");
    print_result("gl_compute_lighting no lights = ambient", 1);

    return pass;
}

/* ============================================================
 * TEST RUNNER
 * ============================================================ */

int gl_test_all(void) {
    int pass = 1;
    int r;

    debuglog(DEBUG_INFO, "[GL_TEST] === GL Subsystem Test Suite ===\n");

    r = gl_test_math();
    debuglog(r ? DEBUG_INFO : DEBUG_ERROR,
             "[GL_TEST] math: %s\n", r ? "PASS" : "FAIL");
    if (!r) pass = 0;

    r = gl_test_state();
    debuglog(r ? DEBUG_INFO : DEBUG_ERROR,
             "[GL_TEST] state: %s\n", r ? "PASS" : "FAIL");
    if (!r) pass = 0;

    r = gl_test_texture();
    debuglog(r ? DEBUG_INFO : DEBUG_ERROR,
             "[GL_TEST] texture: %s\n", r ? "PASS" : "FAIL");
    if (!r) pass = 0;

    r = gl_test_rasterizer();
    debuglog(r ? DEBUG_INFO : DEBUG_ERROR,
             "[GL_TEST] rasterizer: %s\n", r ? "PASS" : "FAIL");
    if (!r) pass = 0;

    r = gl_test_framebuffer();
    debuglog(r ? DEBUG_INFO : DEBUG_ERROR,
             "[GL_TEST] framebuffer: %s\n", r ? "PASS" : "FAIL");
    if (!r) pass = 0;

    r = gl_test_vertex();
    debuglog(r ? DEBUG_INFO : DEBUG_ERROR,
             "[GL_TEST] vertex: %s\n", r ? "PASS" : "FAIL");
    if (!r) pass = 0;

    r = gl_test_matrix();
    debuglog(r ? DEBUG_INFO : DEBUG_ERROR,
             "[GL_TEST] matrix: %s\n", r ? "PASS" : "FAIL");
    if (!r) pass = 0;

    r = gl_test_immediate();
    debuglog(r ? DEBUG_INFO : DEBUG_ERROR,
             "[GL_TEST] immediate: %s\n", r ? "PASS" : "FAIL");
    if (!r) pass = 0;

    r = gl_test_lighting();
    debuglog(r ? DEBUG_INFO : DEBUG_ERROR,
             "[GL_TEST] lighting: %s\n", r ? "PASS" : "FAIL");
    if (!r) pass = 0;

    debuglog(DEBUG_INFO, "[GL_TEST] === All tests %s ===\n",
             pass ? "PASSED" : "SOME FAILED");

    return pass;
}
