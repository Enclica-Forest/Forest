#include "state.h"
#include <string.h>

gl_state_t g_gl_state;

static void mat4_set_identity(mat4_t *m)
{
    memset(m, 0, sizeof(mat4_t));
    m->m[0]  = 1.0f;
    m->m[5]  = 1.0f;
    m->m[10] = 1.0f;
    m->m[15] = 1.0f;
}

void gl_state_init(void)
{
    memset(&g_gl_state, 0, sizeof(gl_state_t));

    g_gl_state.depth_func = GL_LESS;
    g_gl_state.blend_src = GL_SRC_ALPHA;
    g_gl_state.blend_dst = GL_ONE_MINUS_SRC_ALPHA;
    g_gl_state.cull_face_mode = GL_BACK;
    g_gl_state.front_face = GL_CCW;
    g_gl_state.active_texture = GL_TEXTURE0;

    g_gl_state.fog_mode = GL_EXP;
    g_gl_state.fog_color[0] = 0.0f;
    g_gl_state.fog_color[1] = 0.0f;
    g_gl_state.fog_color[2] = 0.0f;
    g_gl_state.fog_color[3] = 0.0f;
    g_gl_state.fog_start = 0.0f;
    g_gl_state.fog_end = 1.0f;
    g_gl_state.fog_density = 1.0f;

    g_gl_state.alpha_func = GL_ALWAYS;
    g_gl_state.alpha_ref = 0.0f;

    g_gl_state.logic_op = GL_COPY;

    for (int i = 0; i < 8; i++)
        g_gl_state.tex_env_mode[i] = GL_MODULATE;

    g_gl_state.clear_color[0] = 0.0f;
    g_gl_state.clear_color[1] = 0.0f;
    g_gl_state.clear_color[2] = 0.0f;
    g_gl_state.clear_color[3] = 1.0f;
    g_gl_state.clear_depth = 1.0;
    g_gl_state.clear_stencil = 0;

    g_gl_state.stencil_func = GL_ALWAYS;
    g_gl_state.stencil_ref = 0;
    g_gl_state.stencil_val_mask = 0xFF;
    g_gl_state.stencil_write_mask = 0xFF;
    g_gl_state.stencil_sfail = GL_STENCIL_KEEP;
    g_gl_state.stencil_dpfail = GL_STENCIL_KEEP;
    g_gl_state.stencil_dppass = GL_STENCIL_KEEP;

    g_gl_state.polygon_offset_factor = 0.0f;
    g_gl_state.polygon_offset_units = 0.0f;

    mat4_set_identity(&g_gl_state.modelview_matrix);
    mat4_set_identity(&g_gl_state.projection_matrix);
    for (int i = 0; i < 8; i++)
        mat4_set_identity(&g_gl_state.texture_matrix[i]);
    g_gl_state.matrix_mode_ptr = &g_gl_state.modelview_matrix;

    g_gl_state.current_color[0] = 1.0f;
    g_gl_state.current_color[1] = 1.0f;
    g_gl_state.current_color[2] = 1.0f;
    g_gl_state.current_color[3] = 1.0f;

    g_gl_state.current_normal[0] = 0.0f;
    g_gl_state.current_normal[1] = 0.0f;
    g_gl_state.current_normal[2] = 1.0f;

    g_gl_state.current_texcoord[0] = 0.0f;
    g_gl_state.current_texcoord[1] = 0.0f;

    g_gl_state.global_ambient[0] = 0.2f;
    g_gl_state.global_ambient[1] = 0.2f;
    g_gl_state.global_ambient[2] = 0.2f;
    g_gl_state.global_ambient[3] = 1.0f;

    g_gl_state.material_shininess = 0.0f;
    for (int i = 0; i < 4; i++) {
        g_gl_state.material_ambient[i] = 0.2f;
        g_gl_state.material_diffuse[i] = 0.8f;
        g_gl_state.material_specular[i] = 0.0f;
    }

    g_gl_state.normal_matrix_dirty = GL_TRUE;

    g_gl_state.modelview_stack_top = 0;
    g_gl_state.projection_stack_top = 0;
    for (int i = 0; i < 8; i++)
        g_gl_state.texture_stack_top[i] = 0;

    g_gl_state.immediate.count = 0;

    g_gl_state.dl_recording = GL_FALSE;
    g_gl_state.dl_current_list = 0;
}
