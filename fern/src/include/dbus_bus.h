#ifndef DBUS_BUS_H
#define DBUS_BUS_H

#include <stdbool.h>
#include <stddef.h>

#include "stdint.h"

#define DBUS_ENDPOINT_ADDR_MAX 256U

typedef enum {
    DBUS_BUS_KIND_SESSION = 0,
    DBUS_BUS_KIND_SYSTEM = 1
} dbus_bus_kind_t;

typedef struct {
    dbus_bus_kind_t kind;
    bool connected;
    char address[DBUS_ENDPOINT_ADDR_MAX];
    int last_error;
} dbus_bus_endpoint_t;

int dbus_bus_endpoint_init(dbus_bus_endpoint_t *endpoint, dbus_bus_kind_t kind);
int dbus_bus_endpoint_connect(dbus_bus_endpoint_t *endpoint);
int dbus_bus_endpoint_disconnect(dbus_bus_endpoint_t *endpoint);

#endif
