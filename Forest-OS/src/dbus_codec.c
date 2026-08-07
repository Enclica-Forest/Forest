#include <stddef.h>

#include "include/dbus_codec.h"
#include "include/string.h"

static void dbus_write_u32(uint8_t *out, uint32_t value, uint8_t endianness) {
    if (endianness == DBUS_ENDIAN_BIG) {
        out[0] = (uint8_t)((value >> 24) & 0xFFU);
        out[1] = (uint8_t)((value >> 16) & 0xFFU);
        out[2] = (uint8_t)((value >> 8) & 0xFFU);
        out[3] = (uint8_t)(value & 0xFFU);
    } else {
        out[0] = (uint8_t)(value & 0xFFU);
        out[1] = (uint8_t)((value >> 8) & 0xFFU);
        out[2] = (uint8_t)((value >> 16) & 0xFFU);
        out[3] = (uint8_t)((value >> 24) & 0xFFU);
    }
}

static uint32_t dbus_read_u32(const uint8_t *in, uint8_t endianness) {
    if (endianness == DBUS_ENDIAN_BIG) {
        return ((uint32_t)in[0] << 24) |
               ((uint32_t)in[1] << 16) |
               ((uint32_t)in[2] << 8) |
               (uint32_t)in[3];
    }

    return ((uint32_t)in[0]) |
           ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

int dbus_encode_wire_header(const dbus_wire_header_t *header,
                            uint8_t *out,
                            size_t out_size) {
    if ((header == NULL) || (out == NULL)) {
        return -1;
    }
    if (out_size < DBUS_WIRE_HEADER_SIZE) {
        return -2;
    }
    if ((header->endianness != DBUS_ENDIAN_LITTLE) &&
        (header->endianness != DBUS_ENDIAN_BIG)) {
        return -3;
    }
    if (header->protocol_version != DBUS_PROTOCOL_VERSION) {
        return -4;
    }

    out[0] = header->endianness;
    out[1] = header->message_type;
    out[2] = header->flags;
    out[3] = header->protocol_version;
    dbus_write_u32(&out[4], header->body_length, header->endianness);
    dbus_write_u32(&out[8], header->serial, header->endianness);
    dbus_write_u32(&out[12], header->header_fields_length, header->endianness);

    return (int)DBUS_WIRE_HEADER_SIZE;
}

int dbus_decode_wire_header(const uint8_t *frame,
                            size_t frame_len,
                            dbus_wire_header_t *out_header) {
    uint8_t endian;

    if ((frame == NULL) || (out_header == NULL)) {
        return -1;
    }
    if (frame_len < DBUS_WIRE_HEADER_SIZE) {
        return -2;
    }

    endian = frame[0];
    if ((endian != DBUS_ENDIAN_LITTLE) && (endian != DBUS_ENDIAN_BIG)) {
        return -3;
    }

    out_header->endianness = endian;
    out_header->message_type = frame[1];
    out_header->flags = frame[2];
    out_header->protocol_version = frame[3];
    out_header->body_length = dbus_read_u32(&frame[4], endian);
    out_header->serial = dbus_read_u32(&frame[8], endian);
    out_header->header_fields_length = dbus_read_u32(&frame[12], endian);

    if (out_header->protocol_version != DBUS_PROTOCOL_VERSION) {
        return -4;
    }

    return 0;
}

int dbus_header_total_bytes(const dbus_wire_header_t *header, size_t *total_bytes_out) {
    uint64_t total;

    if ((header == NULL) || (total_bytes_out == NULL)) {
        return -1;
    }

    total = (uint64_t)DBUS_WIRE_HEADER_SIZE +
            (uint64_t)header->header_fields_length +
            (uint64_t)header->body_length;

    if (total > (uint64_t)((size_t)-1)) {
        return -2;
    }

    *total_bytes_out = (size_t)total;
    return 0;
}
