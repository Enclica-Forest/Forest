#ifndef WAYLAND_SERVER_H
#define WAYLAND_SERVER_H

#include "types.h"
#include "spinlock.h"
#include "wayland_protocol.h"

#define WAYLAND_MAX_CLIENTS 16
#define WAYLAND_MAX_GLOBALS 8
#define WAYLAND_MAX_SURFACES 128

typedef enum {
    WAYLAND_SURFACE_ROLE_NONE = 0,
    WAYLAND_SURFACE_ROLE_WL_SHELL = 1,
    WAYLAND_SURFACE_ROLE_XDG_TOPLEVEL = 2
} wayland_surface_role_t;

typedef struct {
    bool used;
    uint32 name;
    uint32 interface_token;
    uint32 version;
} wayland_global_state_t;

typedef struct {
    bool used;
    int fd;
    uint32 registry_object_id;
    uint32 compositor_object_id;
    uint32 shell_object_id;
    uint32 xdg_wm_base_object_id;
    uint32 seat_object_id;
    uint32 dmabuf_object_id;
    uint32 next_object_id;
} wayland_client_state_t;

typedef struct {
    bool used;
    int owner_fd;
    uint32 object_id;
    wayland_surface_role_t role;
    int32 x;
    int32 y;
    uint32 width;
    uint32 height;
    bool configured;
} wayland_surface_state_t;

typedef struct {
    uint32 seat_version;
    bool pointer_bound;
    bool keyboard_bound;
    bool touch_bound;
    int32 pointer_x;
    int32 pointer_y;
    uint32 button_mask;
} wayland_input_state_t;

typedef struct {
    bool enabled;
    uint32 version;
    uint32 last_buffer_id;
    uint32 last_width;
    uint32 last_height;
    uint32 last_format;
    uint64 last_modifier;
} wayland_dmabuf_state_t;

typedef struct {
    bool initialized;
    uint32 serial;
    spinlock_t lock;
    wayland_global_state_t globals[WAYLAND_MAX_GLOBALS];
    wayland_client_state_t clients[WAYLAND_MAX_CLIENTS];
    wayland_surface_state_t surfaces[WAYLAND_MAX_SURFACES];
    wayland_input_state_t input;
    wayland_dmabuf_state_t dmabuf;
} wayland_server_state_t;

void wayland_server_init(void);
void wayland_server_shutdown(void);
void wayland_server_pump(uint32 max_events);

int wayland_handle_connection(int client_fd);
int wayland_handle_request(int client_fd, const uint8* request, uint32 length);

int wayland_registry_bind(int client_fd,
                          uint32 global_name,
                          uint32 interface_token,
                          uint32 version,
                          uint32 new_id);
int wayland_create_surface(int client_fd, uint32 object_id);
int wayland_assign_shell_role(int client_fd, uint32 surface_id, uint32 role_token);
int wayland_dmabuf_import(int client_fd,
                          uint32 buffer_id,
                          uint32 width,
                          uint32 height,
                          uint32 format,
                          uint64 modifier);

const wayland_server_state_t* wayland_server_get_state(void);

#endif
