#ifndef XDG_H
#define XDG_H

#include <stddef.h>

#define XDG_PATH_MAX 256U
#define XDG_LIST_MAX 384U

typedef const char *(*xdg_env_getter_fn)(const char *name, void *ctx);

typedef struct {
    char home[XDG_PATH_MAX];
    char runtime_dir[XDG_PATH_MAX];
    char config_home[XDG_PATH_MAX];
    char data_home[XDG_PATH_MAX];
    char state_home[XDG_PATH_MAX];
    char cache_home[XDG_PATH_MAX];
    char config_dirs[XDG_LIST_MAX];
    char data_dirs[XDG_LIST_MAX];
} xdg_dirs_t;

int xdg_resolve_dirs(xdg_env_getter_fn getter, void *ctx, xdg_dirs_t *out);
int xdg_runtime_dir_resolve(char *out, size_t out_size);

int xdg_dbus_session_bus_address_resolve(xdg_env_getter_fn getter,
                                         void *ctx,
                                         char *out,
                                         size_t out_size);
int xdg_dbus_system_bus_address_resolve(xdg_env_getter_fn getter,
                                        void *ctx,
                                        char *out,
                                        size_t out_size);

#endif
