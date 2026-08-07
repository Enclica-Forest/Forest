/*
 * driverctl_dev.c — /dev/driverctl device node.
 *
 * Surfaces the unified driver/device registry to userspace through ioctl
 * on a devfs character device. A future `driverctl` tool can list drivers
 * and devices (a combined lspci/lsusb-style view), query per-entry info
 * and force bind/unbind.
 */

#include "include/devfs.h"
#include "include/driver.h"
#include "include/driver_config.h"
#include "include/driverctl.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/spinlock.h"
#include "include/pci.h"
#include "include/util.h"

#include <stdint.h>

#define DRIVERCTL_MAJOR 240   /* experimental major, picked out-of-band */
#define DRIVERCTL_MINOR 0

#if ENABLE_DRIVER_MODEL

typedef struct {
    uint32_t max_count;       /* in: capacity of entries */
    uint32_t actual_count;    /* out: number written */
    void*    entries;          /* in: user buffer */
} driverctl_list_arg_t;

/* ---- Driver list ----------------------------------------------------- */
typedef struct {
    const driverctl_driver_info_t* out;
    uint32_t max;
    uint32_t wrote;
    void* dst;
} list_drv_ctx_t;

static int list_drv_cb(const drv_driver_t* d, void* user) {
    list_drv_ctx_t* c = (list_drv_ctx_t*)user;
    if (c->wrote >= c->max) return 1;
    driverctl_driver_info_t* dst = (driverctl_driver_info_t*)c->dst + c->wrote;
    memory_set((uint8*)dst, 0, sizeof(*dst));
    if (d->name) {
        uint32_t i = 0;
        for (; i + 1 < DRIVERCTL_NAME_MAX && d->name[i]; i++) dst->name[i] = d->name[i];
        dst->name[i] = 0;
    }
    if (d->version) {
        uint32_t i = 0;
        for (; i + 1 < 32 && d->version[i]; i++) dst->version[i] = d->version[i];
        dst->version[i] = 0;
    }
    dst->bus    = (uint32_t)d->bus;
    dst->flags  = d->flags;
    dst->id     = c->wrote + 1;
    c->wrote++;
    return 0;
}

/* ---- Device list ----------------------------------------------------- */
typedef struct {
    uint32_t max;
    uint32_t wrote;
    void* dst;
} list_dev_ctx_t;

static int list_dev_cb(const drv_device_t* d, void* user) {
    list_dev_ctx_t* c = (list_dev_ctx_t*)user;
    if (c->wrote >= c->max) return 1;
    driverctl_device_info_t* dst = (driverctl_device_info_t*)c->dst + c->wrote;
    memory_set((uint8*)dst, 0, sizeof(*dst));
    memory_copy((char*)d->name, dst->name, DRV_DEVICE_NAME_MAX);
    dst->bus          = (uint32_t)d->bus;
    dst->id           = d->id;
    dst->vendor       = d->vendor;
    dst->device_id    = d->device_id;
    dst->subvendor    = d->subvendor;
    dst->subdevice    = d->subdevice;
    dst->class_code   = d->class_code;
    dst->subclass     = d->subclass;
    dst->prog_if      = d->prog_if;
    dst->state        = (uint8_t)d->state;
    if (d->bound && d->bound->name) {
        uint32_t i = 0;
        for (; i + 1 < DRIVERCTL_NAME_MAX && d->bound->name[i]; i++)
            dst->bound_driver[i] = d->bound->name[i];
        dst->bound_driver[i] = 0;
    }
    c->wrote++;
    return 0;
}

typedef struct {
    const char* want;
    drv_driver_t* hit;
} find_drv_ctx_t;

static int find_drv_cb(const drv_driver_t* d, void* user) {
    find_drv_ctx_t* c = (find_drv_ctx_t*)user;
    if (d->name && c->want && strcmp(d->name, c->want) == 0) {
        c->hit = (drv_driver_t*)d;
        return 1;
    }
    return 0;
}

static int driverctl_ioctl(vfs_node_t* node, uint32_t request, void* arg) {
    (void)node;
    if (!arg) return -1;

    switch (request) {
    case DRIVERCTL_LIST_DRIVERS: {
        driverctl_list_arg_t* a = (driverctl_list_arg_t*)arg;
        list_drv_ctx_t c = { NULL, a->max_count, 0, a->entries };
        drv_iter_drivers(list_drv_cb, &c);
        a->actual_count = c.wrote;
        return 0;
    }
    case DRIVERCTL_LIST_DEVICES: {
        driverctl_list_arg_t* a = (driverctl_list_arg_t*)arg;
        list_dev_ctx_t c = { a->max_count, 0, a->entries };
        drv_iter_devices(list_dev_cb, &c);
        a->actual_count = c.wrote;
        return 0;
    }
    case DRIVERCTL_GET_DRIVER_INFO: {
        driverctl_driver_info_t* info = (driverctl_driver_info_t*)arg;
        find_drv_ctx_t fc = { info->name, NULL };
        drv_iter_drivers(find_drv_cb, &fc);
        if (!fc.hit) return -1;
        memory_set((uint8*)info, 0, sizeof(*info));
        memory_copy((char*)fc.hit->name, info->name, DRIVERCTL_NAME_MAX);
        if (fc.hit->version) {
            uint32_t i = 0;
            for (; i + 1 < 32 && fc.hit->version[i]; i++) info->version[i] = fc.hit->version[i];
        }
        info->bus   = (uint32_t)fc.hit->bus;
        info->flags = fc.hit->flags;
        return 0;
    }
    case DRIVERCTL_GET_DEVICE_INFO: {
        driverctl_device_info_t* info = (driverctl_device_info_t*)arg;
        drv_device_t* dev = drv_device_get_by_name(info->name);
        if (!dev) return -1;
        memory_copy((char*)dev->name, info->name, DRV_DEVICE_NAME_MAX);
        info->bus        = (uint32_t)dev->bus;
        info->id         = dev->id;
        info->vendor     = dev->vendor;
        info->device_id  = dev->device_id;
        info->subvendor  = dev->subvendor;
        info->subdevice  = dev->subdevice;
        info->class_code = dev->class_code;
        info->subclass   = dev->subclass;
        info->prog_if    = dev->prog_if;
        info->state      = (uint8_t)dev->state;
        if (dev->bound && dev->bound->name) {
            uint32_t i = 0;
            for (; i + 1 < DRIVERCTL_NAME_MAX && dev->bound->name[i]; i++)
                info->bound_driver[i] = dev->bound->name[i];
            info->bound_driver[i] = 0;
        } else {
            info->bound_driver[0] = 0;
        }
        drv_device_put(dev);
        return 0;
    }
    case DRIVERCTL_BIND: {
        driverctl_name_arg_t* a = (driverctl_name_arg_t*)arg;
        drv_device_t* dev = drv_device_get_by_name(a->name);
        if (!dev) return -1;
        int rc = drv_bind(dev);
        drv_device_put(dev);
        return rc;
    }
    case DRIVERCTL_UNBIND: {
        driverctl_name_arg_t* a = (driverctl_name_arg_t*)arg;
        drv_device_t* dev = drv_device_get_by_name(a->name);
        if (!dev) return -1;
        drv_unbind(dev);
        drv_device_put(dev);
        return 0;
    }
    case DRIVERCTL_RESCAN:
        return drv_rescan();
    default:
        return -1;
    }
}

static uint32 driverctl_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0;
}

static uint32 driverctl_write(vfs_node_t* node, uint32 offset, uint32 size, const uint8* buffer) {
    (void)node; (void)offset; (void)buffer;
    return size;
}

static dev_ops_t g_driverctl_ops = {
    .read  = driverctl_read,
    .write = (uint32 (*)(vfs_node_t *, uint32, uint32, uint8 *))driverctl_write,
    .ioctl = driverctl_ioctl,
};

bool driverctl_dev_init(void) {
    if (!devfs_register_device("driverctl", DEV_TYPE_CHAR,
                               DRIVERCTL_MAJOR, DRIVERCTL_MINOR,
                               &g_driverctl_ops, NULL)) {
        debuglog(DEBUG_WARN, "[DRV] failed to register /dev/driverctl\n");
        return false;
    }
    debuglog(DEBUG_INFO, "[DRV] /dev/driverctl registered\n");
    return true;
}

void driverctl_dev_shutdown(void) {
    devfs_unregister_device("driverctl");
}

#else /* !ENABLE_DRIVER_MODEL */

bool driverctl_dev_init(void) { return true; }
void driverctl_dev_shutdown(void) {}

#endif /* ENABLE_DRIVER_MODEL */