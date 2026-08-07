#include <stddef.h>

#include "include/string.h"
#include "include/xdg.h"

#define XDG_DBUS_PREFIX "unix:path="
#define XDG_DBUS_SYSTEM_SOCKET "/run/dbus/system_bus_socket"

static const xdg_dirs_t g_xdg_defaults = {
    .home = "/home/root",
    .runtime_dir = "/run/user/0",
    .config_home = "/home/root/.config",
    .data_home = "/home/root/.local/share",
    .state_home = "/home/root/.local/state",
    .cache_home = "/home/root/.cache",
    .config_dirs = "/etc/xdg",
    .data_dirs = "/usr/local/share:/usr/share"
};

static int xdg_copy(char *dst, size_t dst_len, const char *src) {
    size_t len;

    if ((dst == NULL) || (src == NULL) || (dst_len == 0U)) {
        return -1;
    }

    len = strlen(src);
    if (len >= dst_len) {
        return -2;
    }

    memcpy(dst, src, len + 1U);
    return 0;
}

static int xdg_join_home_subdir(char *dst, size_t dst_len, const char *home, const char *suffix) {
    size_t home_len;
    size_t suffix_len;

    if ((dst == NULL) || (home == NULL) || (suffix == NULL)) {
        return -1;
    }

    home_len = strlen(home);
    suffix_len = strlen(suffix);

    if ((home_len + suffix_len) >= dst_len) {
        return -2;
    }

    memcpy(dst, home, home_len);
    memcpy(dst + home_len, suffix, suffix_len + 1U);

    return 0;
}

static const char *xdg_lookup_env(xdg_env_getter_fn getter, void *ctx, const char *name) {
    const char *value;

    if ((getter == NULL) || (name == NULL)) {
        return NULL;
    }

    value = getter(name, ctx);
    if ((value == NULL) || (value[0] == '\0')) {
        return NULL;
    }

    return value;
}

static int xdg_compose_prefixed_path(char *out,
                                     size_t out_len,
                                     const char *prefix,
                                     const char *path) {
    size_t prefix_len;
    size_t path_len;

    if ((out == NULL) || (prefix == NULL) || (path == NULL)) {
        return -1;
    }

    prefix_len = strlen(prefix);
    path_len = strlen(path);
    if ((prefix_len + path_len) >= out_len) {
        return -2;
    }

    memcpy(out, prefix, prefix_len);
    memcpy(out + prefix_len, path, path_len + 1U);
    return 0;
}

int xdg_resolve_dirs(xdg_env_getter_fn getter, void *ctx, xdg_dirs_t *out) {
    const char *home;
    const char *runtime_dir;
    const char *config_home;
    const char *data_home;
    const char *state_home;
    const char *cache_home;
    const char *config_dirs;
    const char *data_dirs;

    if (out == NULL) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    home = xdg_lookup_env(getter, ctx, "HOME");
    if (home == NULL) {
        home = g_xdg_defaults.home;
    }
    if (xdg_copy(out->home, sizeof(out->home), home) != 0) {
        return -2;
    }

    runtime_dir = xdg_lookup_env(getter, ctx, "XDG_RUNTIME_DIR");
    if (runtime_dir != NULL) {
        if (xdg_copy(out->runtime_dir, sizeof(out->runtime_dir), runtime_dir) != 0) {
            return -3;
        }
    } else {
        /*
         * TODO(worker6-phase2): derive runtime dir from actual uid/gid/session,
         * and ensure the target directory exists with 0700 permissions.
         */
        if (xdg_copy(out->runtime_dir, sizeof(out->runtime_dir), g_xdg_defaults.runtime_dir) != 0) {
            return -4;
        }
    }

    config_home = xdg_lookup_env(getter, ctx, "XDG_CONFIG_HOME");
    if (config_home != NULL) {
        if (xdg_copy(out->config_home, sizeof(out->config_home), config_home) != 0) {
            return -5;
        }
    } else {
        if (xdg_join_home_subdir(out->config_home, sizeof(out->config_home), out->home, "/.config") != 0) {
            return -6;
        }
    }

    data_home = xdg_lookup_env(getter, ctx, "XDG_DATA_HOME");
    if (data_home != NULL) {
        if (xdg_copy(out->data_home, sizeof(out->data_home), data_home) != 0) {
            return -7;
        }
    } else {
        if (xdg_join_home_subdir(out->data_home, sizeof(out->data_home), out->home, "/.local/share") != 0) {
            return -8;
        }
    }

    state_home = xdg_lookup_env(getter, ctx, "XDG_STATE_HOME");
    if (state_home != NULL) {
        if (xdg_copy(out->state_home, sizeof(out->state_home), state_home) != 0) {
            return -9;
        }
    } else {
        if (xdg_join_home_subdir(out->state_home, sizeof(out->state_home), out->home, "/.local/state") != 0) {
            return -10;
        }
    }

    cache_home = xdg_lookup_env(getter, ctx, "XDG_CACHE_HOME");
    if (cache_home != NULL) {
        if (xdg_copy(out->cache_home, sizeof(out->cache_home), cache_home) != 0) {
            return -11;
        }
    } else {
        if (xdg_join_home_subdir(out->cache_home, sizeof(out->cache_home), out->home, "/.cache") != 0) {
            return -12;
        }
    }

    config_dirs = xdg_lookup_env(getter, ctx, "XDG_CONFIG_DIRS");
    if (config_dirs == NULL) {
        config_dirs = g_xdg_defaults.config_dirs;
    }
    if (xdg_copy(out->config_dirs, sizeof(out->config_dirs), config_dirs) != 0) {
        return -13;
    }

    data_dirs = xdg_lookup_env(getter, ctx, "XDG_DATA_DIRS");
    if (data_dirs == NULL) {
        data_dirs = g_xdg_defaults.data_dirs;
    }
    if (xdg_copy(out->data_dirs, sizeof(out->data_dirs), data_dirs) != 0) {
        return -14;
    }

    return 0;
}

int xdg_runtime_dir_resolve(char *out, size_t out_size) {
    xdg_dirs_t dirs;
    int rc;

    rc = xdg_resolve_dirs(NULL, NULL, &dirs);
    if (rc != 0) {
        return rc;
    }

    return xdg_copy(out, out_size, dirs.runtime_dir);
}

int xdg_dbus_session_bus_address_resolve(xdg_env_getter_fn getter,
                                         void *ctx,
                                         char *out,
                                         size_t out_size) {
    const char *bus_addr;
    xdg_dirs_t dirs;
    size_t runtime_len;
    int rc;

    if ((out == NULL) || (out_size == 0U)) {
        return -1;
    }

    bus_addr = xdg_lookup_env(getter, ctx, "DBUS_SESSION_BUS_ADDRESS");
    if (bus_addr != NULL) {
        return xdg_copy(out, out_size, bus_addr);
    }

    rc = xdg_resolve_dirs(getter, ctx, &dirs);
    if (rc != 0) {
        return rc;
    }

    runtime_len = strlen(dirs.runtime_dir);
    if ((runtime_len + strlen("/bus")) >= sizeof(dirs.runtime_dir)) {
        return -2;
    }

    memcpy(dirs.runtime_dir + runtime_len, "/bus", sizeof("/bus"));
    return xdg_compose_prefixed_path(out, out_size, XDG_DBUS_PREFIX, dirs.runtime_dir);
}

int xdg_dbus_system_bus_address_resolve(xdg_env_getter_fn getter,
                                        void *ctx,
                                        char *out,
                                        size_t out_size) {
    const char *bus_addr;

    if ((out == NULL) || (out_size == 0U)) {
        return -1;
    }

    bus_addr = xdg_lookup_env(getter, ctx, "DBUS_SYSTEM_BUS_ADDRESS");
    if (bus_addr != NULL) {
        return xdg_copy(out, out_size, bus_addr);
    }

    return xdg_compose_prefixed_path(out, out_size, XDG_DBUS_PREFIX, XDG_DBUS_SYSTEM_SOCKET);
}
