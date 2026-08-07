#ifndef DBUS_CODEC_H
#define DBUS_CODEC_H

#include <stddef.h>

#include "dbus.h"

#define DBUS_WIRE_HEADER_SIZE 16U

int dbus_encode_wire_header(const dbus_wire_header_t *header,
                            uint8_t *out,
                            size_t out_size);

int dbus_decode_wire_header(const uint8_t *frame,
                            size_t frame_len,
                            dbus_wire_header_t *out_header);

int dbus_header_total_bytes(const dbus_wire_header_t *header,
                            size_t *total_bytes_out);

#endif
