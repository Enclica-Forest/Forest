#include "include/wayland_protocol.h"
#include <stddef.h>
#include <stddef.h>

static const wayland_interface_desc_t g_wayland_interfaces[] = {
    { WAYLAND_IFACE_WL_REGISTRY, "wl_registry", 1U, 1U },
    { WAYLAND_IFACE_WL_COMPOSITOR, "wl_compositor", 6U, 5U },
    { WAYLAND_IFACE_WL_SHELL, "wl_shell", 1U, 0U },
    { WAYLAND_IFACE_XDG_WM_BASE, "xdg_wm_base", 6U, 3U },
    { WAYLAND_IFACE_WL_SEAT, "wl_seat", 9U, 4U },
    { WAYLAND_IFACE_ZWP_LINUX_DMABUF_V1, "zwp_linux_dmabuf_v1", 4U, 4U }
};

static uint32 wayland_read_u32_le(const uint8* p) {
    return (uint32)((uint32)p[0] |
                    ((uint32)p[1] << 8) |
                    ((uint32)p[2] << 16) |
                    ((uint32)p[3] << 24));
}

bool wayland_protocol_parse_header(const uint8* data,
                                   uint32 length,
                                   wayland_message_header_t* out_header) {
    uint32 word;
    uint16 size;

    if ((data == NULL) || (out_header == NULL) || (length < 8U)) {
        return false;
    }

    out_header->object_id = wayland_read_u32_le(data);
    word = wayland_read_u32_le(data + 4);

    out_header->opcode = (uint16)(word & 0xFFFFU);
    size = (uint16)(word >> 16);
    out_header->size = size;

    if ((size < 8U) || ((uint32)size > length)) {
        return false;
    }

    return true;
}

const wayland_interface_desc_t* wayland_protocol_lookup(uint32 token) {
    uint32 i;

    for (i = 0; i < (uint32)(sizeof(g_wayland_interfaces) / sizeof(g_wayland_interfaces[0])); i++) {
        if (g_wayland_interfaces[i].token == token) {
            return &g_wayland_interfaces[i];
        }
    }

    return NULL;
}

bool wayland_protocol_opcode_supported(uint32 token, uint16 opcode) {
    const wayland_interface_desc_t* iface;

    iface = wayland_protocol_lookup(token);
    if (iface == NULL) {
        return false;
    }

    return opcode <= iface->max_opcode;
}
