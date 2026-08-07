#include <stddef.h>

#include "include/dbus_bus.h"
#include "include/string.h"
#include "include/xdg.h"

int dbus_bus_endpoint_init(dbus_bus_endpoint_t *endpoint, dbus_bus_kind_t kind) {
    if (endpoint == NULL) {
        return -1;
    }

    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->kind = kind;
    endpoint->connected = false;
    endpoint->last_error = 0;

    if (kind == DBUS_BUS_KIND_SYSTEM) {
        if (xdg_dbus_system_bus_address_resolve(NULL,
                                                NULL,
                                                endpoint->address,
                                                sizeof(endpoint->address)) != 0) {
            endpoint->last_error = -2;
            return -2;
        }
        return 0;
    }

    if (xdg_dbus_session_bus_address_resolve(NULL,
                                             NULL,
                                             endpoint->address,
                                             sizeof(endpoint->address)) != 0) {
        endpoint->last_error = -2;
        return -2;
    }
    return 0;
}

int dbus_bus_endpoint_connect(dbus_bus_endpoint_t *endpoint) {
    if (endpoint == NULL) {
        return -1;
    }

    /*
     * TODO(worker6-phase4):
     * - parse address transport (unix:path / unix:abstract)
     * - create socket and connect
     * - integrate authentication handshake (EXTERNAL/ANONYMOUS)
     */
    endpoint->connected = false;
    endpoint->last_error = -100;
    return -100;
}

int dbus_bus_endpoint_disconnect(dbus_bus_endpoint_t *endpoint) {
    if (endpoint == NULL) {
        return -1;
    }

    /* TODO(worker6-phase4): close socket/fd once transport is implemented. */
    endpoint->connected = false;
    endpoint->last_error = 0;
    return 0;
}
