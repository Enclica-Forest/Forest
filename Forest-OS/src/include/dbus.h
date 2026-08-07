#ifndef DBUS_H
#define DBUS_H

#include <stddef.h>

#include "stdint.h"

#define DBUS_MAX_HEADER_FIELDS 16U
#define DBUS_MAX_SIGNATURE_LEN 64U

#define DBUS_PROTOCOL_VERSION 1U
#define DBUS_ENDIAN_LITTLE ((uint8_t)'l')
#define DBUS_ENDIAN_BIG    ((uint8_t)'B')

typedef enum {
    DBUS_MESSAGE_INVALID = 0,
    DBUS_MESSAGE_METHOD_CALL = 1,
    DBUS_MESSAGE_METHOD_RETURN = 2,
    DBUS_MESSAGE_ERROR = 3,
    DBUS_MESSAGE_SIGNAL = 4
} dbus_message_type_t;

typedef enum {
    DBUS_HEADER_PATH = 1,
    DBUS_HEADER_INTERFACE = 2,
    DBUS_HEADER_MEMBER = 3,
    DBUS_HEADER_ERROR_NAME = 4,
    DBUS_HEADER_REPLY_SERIAL = 5,
    DBUS_HEADER_DESTINATION = 6,
    DBUS_HEADER_SENDER = 7,
    DBUS_HEADER_SIGNATURE = 8,
    DBUS_HEADER_UNIX_FDS = 9
} dbus_header_field_code_t;

typedef struct {
    uint8_t code;
    uint8_t reserved[3];
    uint32_t value_length;
    const uint8_t *value;
} dbus_header_field_t;

typedef struct {
    uint8_t endianness;
    uint8_t message_type;
    uint8_t flags;
    uint8_t protocol_version;
    uint32_t body_length;
    uint32_t serial;
    uint32_t header_fields_length;
} dbus_wire_header_t;

typedef struct {
    dbus_wire_header_t header;
    dbus_header_field_t fields[DBUS_MAX_HEADER_FIELDS];
    uint32_t fields_count;
    const uint8_t *body;
} dbus_message_t;

int dbus_message_init(dbus_message_t *msg,
                      dbus_message_type_t message_type,
                      uint8_t flags,
                      uint32_t serial,
                      const uint8_t *body,
                      uint32_t body_len);

int dbus_message_add_field(dbus_message_t *msg,
                           dbus_header_field_code_t code,
                           const uint8_t *value,
                           uint32_t value_len);

int dbus_message_validate(const dbus_message_t *msg);

#endif
