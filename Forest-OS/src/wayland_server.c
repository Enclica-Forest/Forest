#include "include/wayland_server.h"
#include "include/debuglog.h"
#include "include/memory.h"
#include "include/util.h"
#include "include/string.h"

#ifndef memory_set
#define memory_set memset
#endif

#define WAYLAND_GLOBAL_NAME_COMPOSITOR  1U
#define WAYLAND_GLOBAL_NAME_SHELL       2U
#define WAYLAND_GLOBAL_NAME_XDG_WM_BASE 3U
#define WAYLAND_GLOBAL_NAME_SEAT        4U
#define WAYLAND_GLOBAL_NAME_DMABUF      5U

#define WL_DISPLAY_GET_REGISTRY_OPCODE      1U
#define WL_REGISTRY_BIND_OPCODE             0U
#define WL_COMPOSITOR_CREATE_SURFACE_OPCODE 0U
#define WL_SHELL_GET_SHELL_SURFACE_OPCODE   0U
#define XDG_WM_BASE_GET_XDG_SURFACE_OPCODE  2U
#define WL_SEAT_GET_POINTER_OPCODE          0U
#define WL_SEAT_GET_KEYBOARD_OPCODE         1U
#define WL_SEAT_GET_TOUCH_OPCODE            2U
#define ZWP_DMABUF_CREATE_IMMED_OPCODE      3U

static wayland_server_state_t g_wayland_server;

static uint32 wayland_read_u32_le(const uint8* p) {
    return (uint32)((uint32)p[0] |
                    ((uint32)p[1] << 8) |
                    ((uint32)p[2] << 16) |
                    ((uint32)p[3] << 24));
}

static uint32 wayland_align4(uint32 v) {
    return (v + 3U) & ~3U;
}

static wayland_client_state_t* wayland_find_client_locked(int client_fd) {
    int i;

    for (i = 0; i < WAYLAND_MAX_CLIENTS; i++) {
        if (g_wayland_server.clients[i].used && (g_wayland_server.clients[i].fd == client_fd)) {
            return &g_wayland_server.clients[i];
        }
    }

    return NULL;
}

static const wayland_global_state_t* wayland_find_global_by_name_locked(uint32 name) {
    int i;

    for (i = 0; i < WAYLAND_MAX_GLOBALS; i++) {
        if (g_wayland_server.globals[i].used && (g_wayland_server.globals[i].name == name)) {
            return &g_wayland_server.globals[i];
        }
    }

    return NULL;
}

static wayland_surface_state_t* wayland_find_surface_locked(int client_fd, uint32 object_id) {
    int i;

    for (i = 0; i < WAYLAND_MAX_SURFACES; i++) {
        if (g_wayland_server.surfaces[i].used &&
            (g_wayland_server.surfaces[i].owner_fd == client_fd) &&
            (g_wayland_server.surfaces[i].object_id == object_id)) {
            return &g_wayland_server.surfaces[i];
        }
    }

    return NULL;
}

static int wayland_register_globals_locked(void) {
    int idx = 0;

    g_wayland_server.globals[idx++] =
        (wayland_global_state_t){ true, WAYLAND_GLOBAL_NAME_COMPOSITOR, WAYLAND_IFACE_WL_COMPOSITOR, 6U };
    g_wayland_server.globals[idx++] =
        (wayland_global_state_t){ true, WAYLAND_GLOBAL_NAME_SHELL, WAYLAND_IFACE_WL_SHELL, 1U };
    g_wayland_server.globals[idx++] =
        (wayland_global_state_t){ true, WAYLAND_GLOBAL_NAME_XDG_WM_BASE, WAYLAND_IFACE_XDG_WM_BASE, 6U };
    g_wayland_server.globals[idx++] =
        (wayland_global_state_t){ true, WAYLAND_GLOBAL_NAME_SEAT, WAYLAND_IFACE_WL_SEAT, 9U };
    g_wayland_server.globals[idx++] =
        (wayland_global_state_t){ true, WAYLAND_GLOBAL_NAME_DMABUF, WAYLAND_IFACE_ZWP_LINUX_DMABUF_V1, 4U };

    return idx;
}

static int wayland_bind_object_locked(wayland_client_state_t* client,
                                      uint32 interface_token,
                                      uint32 new_id,
                                      uint32 version) {
    if ((client == NULL) || (new_id == 0U)) {
        return -1;
    }

    if (interface_token == WAYLAND_IFACE_WL_COMPOSITOR) {
        client->compositor_object_id = new_id;
    } else if (interface_token == WAYLAND_IFACE_WL_SHELL) {
        client->shell_object_id = new_id;
    } else if (interface_token == WAYLAND_IFACE_XDG_WM_BASE) {
        client->xdg_wm_base_object_id = new_id;
    } else if (interface_token == WAYLAND_IFACE_WL_SEAT) {
        client->seat_object_id = new_id;
        g_wayland_server.input.seat_version = version;
    } else if (interface_token == WAYLAND_IFACE_ZWP_LINUX_DMABUF_V1) {
        client->dmabuf_object_id = new_id;
        g_wayland_server.dmabuf.enabled = true;
        g_wayland_server.dmabuf.version = version;
    } else {
        return -1;
    }

    if (new_id >= client->next_object_id) {
        client->next_object_id = new_id + 1U;
    }

    return 0;
}

void wayland_server_init(void) {
    memory_set((uint8*)&g_wayland_server, 0, sizeof(g_wayland_server));
    spinlock_init(&g_wayland_server.lock, "wayland_server");

    spinlock_acquire(&g_wayland_server.lock);
    g_wayland_server.initialized = true;
    g_wayland_server.serial = 1U;
    (void)wayland_register_globals_locked();
    g_wayland_server.input.seat_version = 1U;
    g_wayland_server.dmabuf.enabled = true;
    g_wayland_server.dmabuf.version = 4U;
    spinlock_release(&g_wayland_server.lock);

    debuglog(DEBUG_INFO, "[Wayland] server initialized (registry/compositor/shell/xdg/input/dmabuf stubs)\n");
}

void wayland_server_shutdown(void) {
    spinlock_acquire(&g_wayland_server.lock);
    memory_set((uint8*)g_wayland_server.clients, 0, sizeof(g_wayland_server.clients));
    memory_set((uint8*)g_wayland_server.surfaces, 0, sizeof(g_wayland_server.surfaces));
    memory_set((uint8*)g_wayland_server.globals, 0, sizeof(g_wayland_server.globals));
    g_wayland_server.initialized = false;
    spinlock_release(&g_wayland_server.lock);

    debuglog(DEBUG_INFO, "[Wayland] server shutdown complete\n");
}

void wayland_server_pump(uint32 max_events) {
    (void)max_events;
}

int wayland_handle_connection(int client_fd) {
    int i;

    if (!g_wayland_server.initialized || (client_fd < 0)) {
        return -1;
    }

    spinlock_acquire(&g_wayland_server.lock);
    for (i = 0; i < WAYLAND_MAX_CLIENTS; i++) {
        if (!g_wayland_server.clients[i].used) {
            wayland_client_state_t* client = &g_wayland_server.clients[i];

            memory_set((uint8*)client, 0, sizeof(*client));
            client->used = true;
            client->fd = client_fd;
            client->next_object_id = 2U;

            spinlock_release(&g_wayland_server.lock);
            return 0;
        }
    }
    spinlock_release(&g_wayland_server.lock);

    return -1;
}

int wayland_registry_bind(int client_fd,
                          uint32 global_name,
                          uint32 interface_token,
                          uint32 version,
                          uint32 new_id) {
    wayland_client_state_t* client;
    const wayland_global_state_t* global;

    spinlock_acquire(&g_wayland_server.lock);

    client = wayland_find_client_locked(client_fd);
    if (client == NULL) {
        spinlock_release(&g_wayland_server.lock);
        return -1;
    }

    global = wayland_find_global_by_name_locked(global_name);
    if ((global == NULL) || (global->interface_token != interface_token)) {
        spinlock_release(&g_wayland_server.lock);
        return -1;
    }

    if (version > global->version) {
        version = global->version;
    }

    if (wayland_bind_object_locked(client, interface_token, new_id, version) != 0) {
        spinlock_release(&g_wayland_server.lock);
        return -1;
    }

    spinlock_release(&g_wayland_server.lock);
    return 0;
}

int wayland_create_surface(int client_fd, uint32 object_id) {
    wayland_client_state_t* client;
    int i;

    spinlock_acquire(&g_wayland_server.lock);

    client = wayland_find_client_locked(client_fd);
    if ((client == NULL) || (client->compositor_object_id == 0U) || (object_id == 0U)) {
        spinlock_release(&g_wayland_server.lock);
        return -1;
    }

    if (wayland_find_surface_locked(client_fd, object_id) != NULL) {
        spinlock_release(&g_wayland_server.lock);
        return -1;
    }

    for (i = 0; i < WAYLAND_MAX_SURFACES; i++) {
        if (!g_wayland_server.surfaces[i].used) {
            wayland_surface_state_t* surface = &g_wayland_server.surfaces[i];

            memory_set((uint8*)surface, 0, sizeof(*surface));
            surface->used = true;
            surface->owner_fd = client_fd;
            surface->object_id = object_id;
            surface->role = WAYLAND_SURFACE_ROLE_NONE;

            spinlock_release(&g_wayland_server.lock);
            return 0;
        }
    }

    spinlock_release(&g_wayland_server.lock);
    return -1;
}

int wayland_assign_shell_role(int client_fd, uint32 surface_id, uint32 role_token) {
    wayland_client_state_t* client;
    wayland_surface_state_t* surface;
    wayland_surface_role_t target_role;

    spinlock_acquire(&g_wayland_server.lock);

    client = wayland_find_client_locked(client_fd);
    if (client == NULL) {
        spinlock_release(&g_wayland_server.lock);
        return -1;
    }

    surface = wayland_find_surface_locked(client_fd, surface_id);
    if (surface == NULL) {
        spinlock_release(&g_wayland_server.lock);
        return -1;
    }

    if (role_token == WAYLAND_IFACE_WL_SHELL) {
        if (client->shell_object_id == 0U) {
            spinlock_release(&g_wayland_server.lock);
            return -1;
        }
        target_role = WAYLAND_SURFACE_ROLE_WL_SHELL;
    } else if (role_token == WAYLAND_IFACE_XDG_WM_BASE) {
        if (client->xdg_wm_base_object_id == 0U) {
            spinlock_release(&g_wayland_server.lock);
            return -1;
        }
        target_role = WAYLAND_SURFACE_ROLE_XDG_TOPLEVEL;
    } else {
        spinlock_release(&g_wayland_server.lock);
        return -1;
    }

    if ((surface->role != WAYLAND_SURFACE_ROLE_NONE) && (surface->role != target_role)) {
        spinlock_release(&g_wayland_server.lock);
        return -1;
    }

    surface->role = target_role;
    surface->configured = true;

    spinlock_release(&g_wayland_server.lock);
    return 0;
}

int wayland_dmabuf_import(int client_fd,
                          uint32 buffer_id,
                          uint32 width,
                          uint32 height,
                          uint32 format,
                          uint64 modifier) {
    wayland_client_state_t* client;

    spinlock_acquire(&g_wayland_server.lock);

    client = wayland_find_client_locked(client_fd);
    if ((client == NULL) || (client->dmabuf_object_id == 0U) || (buffer_id == 0U) ||
        (width == 0U) || (height == 0U)) {
        spinlock_release(&g_wayland_server.lock);
        return -1;
    }

    g_wayland_server.dmabuf.last_buffer_id = buffer_id;
    g_wayland_server.dmabuf.last_width = width;
    g_wayland_server.dmabuf.last_height = height;
    g_wayland_server.dmabuf.last_format = format;
    g_wayland_server.dmabuf.last_modifier = modifier;

    spinlock_release(&g_wayland_server.lock);
    return 0;
}

const wayland_server_state_t* wayland_server_get_state(void) {
    return &g_wayland_server;
}

int wayland_handle_request(int client_fd, const uint8* request, uint32 length) {
    wayland_message_header_t header;

    if (!g_wayland_server.initialized || (request == NULL)) {
        return -1;
    }

    if (!wayland_protocol_parse_header(request, length, &header)) {
        return -1;
    }

    if (header.object_id == 1U) {
        wayland_client_state_t* client;

        if (header.opcode != WL_DISPLAY_GET_REGISTRY_OPCODE) {
            return 0;
        }
        if (header.size < 12U) {
            return -1;
        }

        spinlock_acquire(&g_wayland_server.lock);
        client = wayland_find_client_locked(client_fd);
        if (client != NULL) {
            client->registry_object_id = wayland_read_u32_le(request + 8);
            if (client->registry_object_id >= client->next_object_id) {
                client->next_object_id = client->registry_object_id + 1U;
            }
        }
        spinlock_release(&g_wayland_server.lock);

        return (client != NULL) ? 0 : -1;
    }

    spinlock_acquire(&g_wayland_server.lock);

    {
        wayland_client_state_t* client = wayland_find_client_locked(client_fd);
        if (client == NULL) {
            spinlock_release(&g_wayland_server.lock);
            return -1;
        }

        if ((header.object_id == client->registry_object_id) &&
            (header.opcode == WL_REGISTRY_BIND_OPCODE) &&
            (header.size >= 24U)) {
            uint32 global_name = wayland_read_u32_le(request + 8);
            uint32 string_len = wayland_read_u32_le(request + 12);
            uint32 off = 16U + wayland_align4(string_len);
            uint32 version;
            uint32 new_id;
            const wayland_global_state_t* global;

            if ((off + 8U) > header.size) {
                spinlock_release(&g_wayland_server.lock);
                return -1;
            }

            version = wayland_read_u32_le(request + off);
            new_id = wayland_read_u32_le(request + off + 4U);
            global = wayland_find_global_by_name_locked(global_name);
            if (global == NULL) {
                spinlock_release(&g_wayland_server.lock);
                return -1;
            }

            if (version > global->version) {
                version = global->version;
            }

            if (wayland_bind_object_locked(client, global->interface_token, new_id, version) != 0) {
                spinlock_release(&g_wayland_server.lock);
                return -1;
            }

            spinlock_release(&g_wayland_server.lock);
            return 0;
        }

        if ((header.object_id == client->compositor_object_id) &&
            (header.opcode == WL_COMPOSITOR_CREATE_SURFACE_OPCODE) &&
            (header.size >= 12U)) {
            uint32 new_surface_id = wayland_read_u32_le(request + 8);
            int i;

            for (i = 0; i < WAYLAND_MAX_SURFACES; i++) {
                if (!g_wayland_server.surfaces[i].used) {
                    wayland_surface_state_t* surface = &g_wayland_server.surfaces[i];
                    memory_set((uint8*)surface, 0, sizeof(*surface));
                    surface->used = true;
                    surface->owner_fd = client_fd;
                    surface->object_id = new_surface_id;
                    spinlock_release(&g_wayland_server.lock);
                    return 0;
                }
            }

            spinlock_release(&g_wayland_server.lock);
            return -1;
        }

        if ((header.object_id == client->shell_object_id) &&
            (header.opcode == WL_SHELL_GET_SHELL_SURFACE_OPCODE) &&
            (header.size >= 16U)) {
            uint32 surface_id = wayland_read_u32_le(request + 12);
            wayland_surface_state_t* surface = wayland_find_surface_locked(client_fd, surface_id);

            if (surface == NULL) {
                spinlock_release(&g_wayland_server.lock);
                return -1;
            }

            if ((surface->role != WAYLAND_SURFACE_ROLE_NONE) &&
                (surface->role != WAYLAND_SURFACE_ROLE_WL_SHELL)) {
                spinlock_release(&g_wayland_server.lock);
                return -1;
            }

            surface->role = WAYLAND_SURFACE_ROLE_WL_SHELL;
            spinlock_release(&g_wayland_server.lock);
            return 0;
        }

        if ((header.object_id == client->xdg_wm_base_object_id) &&
            (header.opcode == XDG_WM_BASE_GET_XDG_SURFACE_OPCODE) &&
            (header.size >= 16U)) {
            uint32 surface_id = wayland_read_u32_le(request + 12);
            wayland_surface_state_t* surface = wayland_find_surface_locked(client_fd, surface_id);

            if (surface == NULL) {
                spinlock_release(&g_wayland_server.lock);
                return -1;
            }

            if ((surface->role != WAYLAND_SURFACE_ROLE_NONE) &&
                (surface->role != WAYLAND_SURFACE_ROLE_XDG_TOPLEVEL)) {
                spinlock_release(&g_wayland_server.lock);
                return -1;
            }

            surface->role = WAYLAND_SURFACE_ROLE_XDG_TOPLEVEL;
            surface->configured = true;
            spinlock_release(&g_wayland_server.lock);
            return 0;
        }

        if (header.object_id == client->seat_object_id) {
            if ((header.opcode == WL_SEAT_GET_POINTER_OPCODE) && (header.size >= 12U)) {
                g_wayland_server.input.pointer_bound = true;
                spinlock_release(&g_wayland_server.lock);
                return 0;
            }
            if ((header.opcode == WL_SEAT_GET_KEYBOARD_OPCODE) && (header.size >= 12U)) {
                g_wayland_server.input.keyboard_bound = true;
                spinlock_release(&g_wayland_server.lock);
                return 0;
            }
            if ((header.opcode == WL_SEAT_GET_TOUCH_OPCODE) && (header.size >= 12U)) {
                g_wayland_server.input.touch_bound = true;
                spinlock_release(&g_wayland_server.lock);
                return 0;
            }
        }

        if ((header.object_id == client->dmabuf_object_id) &&
            (header.opcode == ZWP_DMABUF_CREATE_IMMED_OPCODE) &&
            (header.size >= 36U)) {
            uint32 buffer_id = wayland_read_u32_le(request + 8);
            uint32 width = wayland_read_u32_le(request + 16);
            uint32 height = wayland_read_u32_le(request + 20);
            uint32 format = wayland_read_u32_le(request + 24);
            uint64 modifier_hi = (uint64)wayland_read_u32_le(request + 28);
            uint64 modifier_lo = (uint64)wayland_read_u32_le(request + 32);

            g_wayland_server.dmabuf.last_buffer_id = buffer_id;
            g_wayland_server.dmabuf.last_width = width;
            g_wayland_server.dmabuf.last_height = height;
            g_wayland_server.dmabuf.last_format = format;
            g_wayland_server.dmabuf.last_modifier = (modifier_hi << 32) | modifier_lo;
            spinlock_release(&g_wayland_server.lock);
            return 0;
        }
    }

    spinlock_release(&g_wayland_server.lock);
    return 0;
}
