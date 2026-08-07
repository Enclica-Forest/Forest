#ifndef WAYLAND_PROTOCOL_H
#define WAYLAND_PROTOCOL_H

#include "types.h"

typedef enum {
    WAYLAND_IFACE_WL_REGISTRY = 1,
    WAYLAND_IFACE_WL_COMPOSITOR = 2,
    WAYLAND_IFACE_WL_SHELL = 3,
    WAYLAND_IFACE_XDG_WM_BASE = 4,
    WAYLAND_IFACE_WL_SEAT = 5,
    WAYLAND_IFACE_ZWP_LINUX_DMABUF_V1 = 6
} wayland_interface_token_t;

typedef struct {
    uint32 token;
    const char* name;
    uint32 advertised_version;
    uint16 max_opcode;
} wayland_interface_desc_t;

typedef struct {
    uint32 object_id;
    uint16 opcode;
    uint16 size;
} wayland_message_header_t;

bool wayland_protocol_parse_header(const uint8* data,
                                   uint32 length,
                                   wayland_message_header_t* out_header);
const wayland_interface_desc_t* wayland_protocol_lookup(uint32 token);
bool wayland_protocol_opcode_supported(uint32 token, uint16 opcode);

#endif
