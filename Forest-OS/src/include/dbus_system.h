#ifndef DBUS_SYSTEM_H
#define DBUS_SYSTEM_H

#include <stddef.h>

#include "dbus_bus.h"

int dbus_system_endpoint_init(dbus_bus_endpoint_t *endpoint);
int dbus_system_bus_address(char *out, size_t out_size);

#endif
