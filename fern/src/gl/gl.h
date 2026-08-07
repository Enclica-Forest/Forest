#ifndef GL_H
#define GL_H

#include "state.h"
#include "math.h"

// All GL API headers
#include "api_state.h"
#include "api_matrix.h"
#include "api_vertex.h"
#include "api_immediate.h"
#include "api_vertex_arrays.h"
#include "api_texture.h"
#include "api_buffer.h"
#include "api_draw.h"
#include "api_light.h"
#include "api_shader.h"
#include "api_query.h"
#include "api_hint.h"
#include "displaylist.h"

// Internal headers
#include "texture.h"
#include "buffer.h"
#include "rasterizer.h"
#include "fragment.h"
#include "framebuffer.h"
#include "present.h"
#include "lighting.h"
#include "error.h"
#include "context.h"
#include "surface.h"

// GL initialization
void gl_init(void);
void gl_init_with_framebuffer(void);
void gl_shutdown(void);
int  gl_is_initialized(void);

// Framebuffer access
gl_framebuffer_t* gl_get_framebuffer(void);
void gl_screenshot(void);

// GetString
const GLubyte* glGetString(GLenum name);

#endif
