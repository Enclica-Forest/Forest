#include <stddef.h>

#include "include/dbus_bus.h"
#include "include/dbus_system.h"
#include "include/xdg.h"

int dbus_system_endpoint_init(dbus_bus_endpoint_t *endpoint) {
    return dbus_bus_endpoint_init(endpoint, DBUS_BUS_KIND_SYSTEM);
}

int dbus_system_bus_address(char *out, size_t out_size) {
    return xdg_dbus_system_bus_address_resolve(NULL, NULL, out, out_size);
}
