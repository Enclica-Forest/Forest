#include <stddef.h>

#include "include/dbus.h"
#include "include/string.h"

static bool dbus_is_valid_endianness(uint8_t byte_order) {
    return (byte_order == DBUS_ENDIAN_LITTLE) || (byte_order == DBUS_ENDIAN_BIG);
}

int dbus_message_init(dbus_message_t *msg,
                      dbus_message_type_t message_type,
                      uint8_t flags,
                      uint32_t serial,
                      const uint8_t *body,
                      uint32_t body_len) {
    if (msg == NULL) {
        return -1;
    }
    if ((message_type < DBUS_MESSAGE_METHOD_CALL) || (message_type > DBUS_MESSAGE_SIGNAL)) {
        return -2;
    }

    memset(msg, 0, sizeof(*msg));
    msg->header.endianness = DBUS_ENDIAN_LITTLE;
    msg->header.message_type = (uint8_t)message_type;
    msg->header.flags = flags;
    msg->header.protocol_version = DBUS_PROTOCOL_VERSION;
    msg->header.body_length = body_len;
    msg->header.serial = serial;
    msg->header.header_fields_length = 0;
    msg->fields_count = 0;
    msg->body = body;

    return 0;
}

int dbus_message_add_field(dbus_message_t *msg,
                           dbus_header_field_code_t code,
                           const uint8_t *value,
                           uint32_t value_len) {
    dbus_header_field_t *field;

    if (msg == NULL) {
        return -1;
    }
    if (msg->fields_count >= DBUS_MAX_HEADER_FIELDS) {
        return -2;
    }
    if ((value == NULL) && (value_len != 0U)) {
        return -3;
    }

    field = &msg->fields[msg->fields_count++];
    field->code = (uint8_t)code;
    field->reserved[0] = 0;
    field->reserved[1] = 0;
    field->reserved[2] = 0;
    field->value_length = value_len;
    field->value = value;

    /*
     * Placeholder length model:
     * Each field contributes 8 bytes of local metadata + payload bytes.
     * TODO(worker6-phase3): switch to exact D-Bus marshaled variant sizing/alignment.
     */
    msg->header.header_fields_length += 8U + value_len;

    return 0;
}

int dbus_message_validate(const dbus_message_t *msg) {
    if (msg == NULL) {
        return -1;
    }
    if (!dbus_is_valid_endianness(msg->header.endianness)) {
        return -2;
    }
    if (msg->header.protocol_version != DBUS_PROTOCOL_VERSION) {
        return -3;
    }
    if ((msg->header.message_type < DBUS_MESSAGE_METHOD_CALL) ||
        (msg->header.message_type > DBUS_MESSAGE_SIGNAL)) {
        return -4;
    }
    if (msg->fields_count > DBUS_MAX_HEADER_FIELDS) {
        return -5;
    }

    return 0;
}
