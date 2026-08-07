#include "gl.h"
#include "demo.h"
#include "../include/debuglog.h"

#define CHECKER_SIZE 64
#define TEX_SIZE     64

static GLuint checker_tex = 0;
static float  tex_rotation = 0.0f;

void gl_demo_texured_init(void)
{
    unsigned char pixels[TEX_SIZE * TEX_SIZE * 4];

    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int idx = (y * TEX_SIZE + x) * 4;
            int check = ((x / CHECKER_SIZE) + (y / CHECKER_SIZE)) & 1;
            if (check) {
                pixels[idx + 0] = 255;
                pixels[idx + 1] = 255;
                pixels[idx + 2] = 255;
            } else {
                pixels[idx + 0] = 60;
                pixels[idx + 1] = 60;
                pixels[idx + 2] = 200;
            }
            pixels[idx + 3] = 255;
        }
    }

    gl_gen_textures(1, &checker_tex);
    gl_bind_texture(GL_TEXTURE_2D, checker_tex);
    gl_tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA8,
                    TEX_SIZE, TEX_SIZE, 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    gl_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    gl_enable(GL_TEXTURE_2D);

    debuglog(DEBUG_INFO, "[GL_DEMO] Textured quad demo initialized (texture=%u)\n",
             checker_tex);
}

void gl_demo_texured_render(void)
{
    gl_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(50.0, 1.333, 0.1, 100.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0, 0, 3,
              0, 0, 0,
              0, 1, 0);

    glRotatef(tex_rotation, 0, 0, 1);
    tex_rotation += 0.5f;
    if (tex_rotation >= 360.0f) tex_rotation -= 360.0f;

    gl_bind_texture(GL_TEXTURE_2D, checker_tex);
    gl_enable(GL_TEXTURE_2D);

    glBegin(GL_TRIANGLES);

    glTexCoord2f(0.0f, 0.0f);
    glColor3f(1, 1, 1);
    glVertex3f(-1.5f, -1.0f, 0.0f);

    glTexCoord2f(2.0f, 0.0f);
    glColor3f(1, 1, 1);
    glVertex3f( 1.5f, -1.0f, 0.0f);

    glTexCoord2f(2.0f, 2.0f);
    glColor3f(1, 1, 1);
    glVertex3f( 1.5f,  1.0f, 0.0f);

    glTexCoord2f(0.0f, 0.0f);
    glColor3f(1, 1, 1);
    glVertex3f(-1.5f, -1.0f, 0.0f);

    glTexCoord2f(2.0f, 2.0f);
    glColor3f(1, 1, 1);
    glVertex3f( 1.5f,  1.0f, 0.0f);

    glTexCoord2f(0.0f, 2.0f);
    glColor3f(1, 1, 1);
    glVertex3f(-1.5f,  1.0f, 0.0f);

    glEnd();

    gl_present();
}

void gl_demo_texured_cleanup(void)
{
    if (checker_tex) {
        gl_delete_textures(1, &checker_tex);
        checker_tex = 0;
    }
    debuglog(DEBUG_INFO, "[GL_DEMO] Textured quad demo cleaned up\n");
}
