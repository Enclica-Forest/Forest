#include "gl.h"
#include "demo.h"
#include "../include/debuglog.h"

static float rotation = 0.0f;

void gl_demo_init(void)
{
    debuglog(DEBUG_INFO, "[GL_DEMO] Rotating cube demo initialized\n");
}

void gl_demo_render(void)
{
    gl_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, 1.333, 0.1, 100.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(3, 3, 3,
              0, 0, 0,
              0, 1, 0);

    glRotatef(rotation, 0, 1, 0);
    rotation += 1.0f;
    if (rotation >= 360.0f) rotation -= 360.0f;

    gl_enable(GL_DEPTH_TEST);

    glBegin(GL_TRIANGLES);

    /* Front face (red) */
    glColor3f(1, 0, 0);
    glVertex3f(-1, -1,  1);
    glVertex3f( 1, -1,  1);
    glVertex3f( 1,  1,  1);
    glVertex3f(-1, -1,  1);
    glVertex3f( 1,  1,  1);
    glVertex3f(-1,  1,  1);

    /* Back face (green) */
    glColor3f(0, 1, 0);
    glVertex3f(-1, -1, -1);
    glVertex3f(-1,  1, -1);
    glVertex3f( 1,  1, -1);
    glVertex3f(-1, -1, -1);
    glVertex3f( 1,  1, -1);
    glVertex3f( 1, -1, -1);

    /* Top face (blue) */
    glColor3f(0, 0, 1);
    glVertex3f(-1,  1, -1);
    glVertex3f(-1,  1,  1);
    glVertex3f( 1,  1,  1);
    glVertex3f(-1,  1, -1);
    glVertex3f( 1,  1,  1);
    glVertex3f( 1,  1, -1);

    /* Bottom face (yellow) */
    glColor3f(1, 1, 0);
    glVertex3f(-1, -1, -1);
    glVertex3f( 1, -1, -1);
    glVertex3f( 1, -1,  1);
    glVertex3f(-1, -1, -1);
    glVertex3f( 1, -1,  1);
    glVertex3f(-1, -1,  1);

    /* Right face (magenta) */
    glColor3f(1, 0, 1);
    glVertex3f( 1, -1, -1);
    glVertex3f( 1,  1, -1);
    glVertex3f( 1,  1,  1);
    glVertex3f( 1, -1, -1);
    glVertex3f( 1,  1,  1);
    glVertex3f( 1, -1,  1);

    /* Left face (cyan) */
    glColor3f(0, 1, 1);
    glVertex3f(-1, -1, -1);
    glVertex3f(-1, -1,  1);
    glVertex3f(-1,  1,  1);
    glVertex3f(-1, -1, -1);
    glVertex3f(-1,  1,  1);
    glVertex3f(-1,  1, -1);

    glEnd();

    gl_present();
}

void gl_demo_cleanup(void)
{
    debuglog(DEBUG_INFO, "[GL_DEMO] Cleanup complete\n");
}
