/*
 * Unified driver model: registry, bus matching, bind/unbind, hotplug,
 * power-management and iteration. Lives in /home/bluet/Forest-OS/src.
 */

#include "include/driver.h"
#include "include/driver_config.h"
#include "include/spinlock.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/screen.h"
#include "include/debuglog.h"

#if ENABLE_DRIVER_MODEL

static spinlock_t g_drv_lock = SPINLOCK_INIT("drv_registry");
static drv_driver_t* g_drivers_head = NULL;
static drv_device_t* g_devices_head = NULL;
static uint32_t g_next_device_id = 1;
static uint32_t g_driver_count = 0;
static uint32_t g_device_count = 0;
static bool g_drv_ready = false;

bool drv_core_init(void) {
    spinlock_acquire(&g_drv_lock);
    g_drivers_head = NULL;
    g_devices_head = NULL;
    g_next_device_id = 1;
    g_driver_count = 0;
    g_device_count = 0;
    g_drv_ready = true;
    spinlock_release(&g_drv_lock);
    return true;
}

void drv_core_shutdown(void) {
    spinlock_acquire(&g_drv_lock);

    drv_device_t* dev = g_devices_head;
    while (dev) {
        drv_device_t* next = dev->next;
        if (dev->bound && dev->bound->remove) {
            dev->bound->remove(dev);
        }
        dev->state = DEVICE_STATE_REMOVED;
        dev = next;
    }

    spinlock_release(&g_drv_lock);

    /* Tear down after a second pass to be safe without holding the lock. */
    spinlock_acquire(&g_drv_lock);
    dev = g_devices_head;
    while (dev) {
        drv_device_t* next = dev->next;
        kfree(dev);
        dev = next;
    }
    g_devices_head = NULL;
    g_drivers_head = NULL;
    g_device_count = 0;
    g_driver_count = 0;
    g_drv_ready = false;
    spinlock_release(&g_drv_lock);
}

static int drv_match_id(const drv_driver_t* drv, const drv_device_t* dev,
                        const drv_id_t** out_id) {
    if (!drv->id_table) {
        return 0;
    }
    for (const drv_id_t* id = drv->id_table;
         !(id->flags == 0 && id->bus == DRV_BUS_UNKNOWN);
         id++) {
        if (id->bus != DRV_BUS_UNKNOWN && id->bus != dev->bus) {
            continue;
        }
        if (id->flags & DRV_MATCH_ANY) {
            if (out_id) *out_id = id;
            return 1;
        }
        if (id->flags & DRV_MATCH_VENDOR) {
            if (id->vendor != DRV_ID_ANY && id->vendor != dev->vendor) continue;
            if (id->device  != DRV_ID_ANY && id->device  != dev->device_id) continue;
            if (id->flags  & DRV_MATCH_SUBSYS) {
                if (id->subvendor != DRV_ID_ANY && id->subvendor != dev->subvendor) continue;
                if (id->subdevice != DRV_ID_ANY && id->subdevice != dev->subdevice) continue;
            }
        }
        if (id->flags & DRV_MATCH_CLASS) {
            if (id->class_code != 0xFF && id->class_code != dev->class_code) continue;
            if (id->subclass   != 0xFF && id->subclass   != dev->subclass) continue;
            if (id->prog_if    != 0xFF && id->prog_if    != dev->prog_if)    continue;
        }
        if (out_id) *out_id = id;
        return 1;
    }
    return 0;
}

static drv_driver_t* drv_find_locked(const char* name) {
    for (drv_driver_t* d = g_drivers_head; d; d = d->next) {
        if (d->name && name && strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

int drv_register(const drv_driver_t* drv) {
    if (!drv || !drv->name) {
        return -1;
    }
    if (!g_drv_ready) {
        drv_core_init();
    }
    spinlock_acquire(&g_drv_lock);
    if (drv_find_locked(drv->name)) {
        spinlock_release(&g_drv_lock);
        return -1;
    }
    if (g_driver_count >= DRIVER_MAX_DRIVERS) {
        spinlock_release(&g_drv_lock);
        return -1;
    }
    /* Cast away const for the registry link field; semantics are otherwise const. */
    drv_driver_t* entry = (drv_driver_t*)drv;
    entry->next = g_drivers_head;
    g_drivers_head = entry;
    g_driver_count++;
    spinlock_release(&g_drv_lock);

    debuglog(DEBUG_INFO, "[DRV] registered driver '%s' (bus=%u)\n",
             drv->name, (unsigned)drv->bus);

    /* Try to bind to any existing unbound device of matching bus. */
    spinlock_acquire(&g_drv_lock);
    for (drv_device_t* dev = g_devices_head; dev; dev = dev->next) {
        if (dev->bound) continue;
        if (drv->bus != DRV_BUS_UNKNOWN && drv->bus != dev->bus) continue;
        const drv_id_t* id = NULL;
        if (drv_match_id(drv, dev, &id)) {
            dev->refcount++;
            spinlock_release(&g_drv_lock);
            drv_bind(dev);
            drv_device_put(dev);
            spinlock_acquire(&g_drv_lock);
        }
    }
    spinlock_release(&g_drv_lock);
    return 0;
}

void drv_unregister(const drv_driver_t* drv) {
    if (!drv) return;
    spinlock_acquire(&g_drv_lock);
    drv_driver_t* prev = NULL;
    for (drv_driver_t* d = g_drivers_head; d; prev = d, d = d->next) {
        if (d == drv) {
            if (prev) prev->next = d->next;
            else       g_drivers_head = d->next;
            g_driver_count--;
            break;
        }
    }
    /* Unbind every device still bound to this driver. */
    for (drv_device_t* dev = g_devices_head; dev; dev = dev->next) {
        if (dev->bound == drv) {
            dev->refcount++;
            spinlock_release(&g_drv_lock);
            drv_unbind(dev);
            drv_device_put(dev);
            spinlock_acquire(&g_drv_lock);
        }
    }
    spinlock_release(&g_drv_lock);
}

int drv_bind(drv_device_t* dev) {
    if (!dev) return -1;
    spinlock_acquire(&g_drv_lock);
    if (dev->bound) {
        spinlock_release(&g_drv_lock);
        return 0;
    }
    for (drv_driver_t* d = g_drivers_head; d; d = d->next) {
        if (d->bus != DRV_BUS_UNKNOWN && d->bus != dev->bus) continue;
        const drv_id_t* id = NULL;
        if (!drv_match_id(d, dev, &id)) continue;
        dev->bound = d;
        dev->state = DEVICE_STATE_BOUND;
        spinlock_release(&g_drv_lock);

        int ret = 0;
        if (d->probe) {
            ret = d->probe(dev, id);
        }
        if (ret == 0) {
            dev->state = DEVICE_STATE_RUNNING;
            debuglog(DEBUG_INFO, "[DRV] bound '%s' to device '%s'\n",
                     d->name, dev->name);
        } else {
            spinlock_acquire(&g_drv_lock);
            dev->bound = NULL;
            dev->state = DEVICE_STATE_INIT;
            spinlock_release(&g_drv_lock);
            debuglog(DEBUG_WARN, "[DRV] probe for '%s' on '%s' failed (%d)\n",
                     d->name, dev->name, ret);
        }
        return ret;
    }
    spinlock_release(&g_drv_lock);
    return -1;
}

void drv_unbind(drv_device_t* dev) {
    if (!dev) return;
    drv_driver_t* bound;
    spinlock_acquire(&g_drv_lock);
    bound = dev->bound;
    if (!bound) {
        spinlock_release(&g_drv_lock);
        return;
    }
    dev->state = DEVICE_STATE_SUSPENDED;
    spinlock_release(&g_drv_lock);

    if (bound->remove) {
        bound->remove(dev);
    }
    spinlock_acquire(&g_drv_lock);
    dev->bound = NULL;
    dev->state = DEVICE_STATE_INIT;
    spinlock_release(&g_drv_lock);
    debuglog(DEBUG_INFO, "[DRV] unbound '%s' from '%s'\n",
             bound->name, dev->name);
}

int drv_suspend(drv_device_t* dev, int level) {
    if (!dev) return -1;
#if ENABLE_DRIVER_PM
    spinlock_acquire(&g_drv_lock);
    drv_driver_t* bound = dev->bound;
    spinlock_release(&g_drv_lock);
    if (!bound || !bound->suspend) return 0;
    int ret = bound->suspend(dev, level);
    if (ret == 0) dev->state = DEVICE_STATE_SUSPENDED;
    return ret;
#else
    (void)level;
    return 0;
#endif
}

int drv_resume(drv_device_t* dev, int level) {
    if (!dev) return -1;
#if ENABLE_DRIVER_PM
    spinlock_acquire(&g_drv_lock);
    drv_driver_t* bound = dev->bound;
    spinlock_release(&g_drv_lock);
    if (!bound || !bound->resume) return 0;
    int ret = bound->resume(dev, level);
    if (ret == 0) dev->state = DEVICE_STATE_RUNNING;
    return ret;
#else
    (void)level;
    return 0;
#endif
}

drv_device_t* drv_device_get(drv_device_t* dev) {
    if (!dev) return NULL;
    spinlock_acquire(&g_drv_lock);
    dev->refcount++;
    spinlock_release(&g_drv_lock);
    return dev;
}

void drv_device_put(drv_device_t* dev) {
    if (!dev) return;
    spinlock_acquire(&g_drv_lock);
    if (dev->refcount > 0) dev->refcount--;
    spinlock_release(&g_drv_lock);
}

static void drv_fill_device(drv_device_t* dev) {
    dev->next = NULL;
    dev->bound = NULL;
    dev->state = DEVICE_STATE_INIT;
    dev->refcount = 1;
    dev->id = g_next_device_id++;
}

static drv_device_t* drv_device_find_locked(const char* name) {
    for (drv_device_t* d = g_devices_head; d; d = d->next) {
        if (strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

int drv_bus_register(drv_device_t* dev) {
    if (!dev || !dev->name[0]) return -1;
    if (!g_drv_ready) drv_core_init();
    spinlock_acquire(&g_drv_lock);
    if (g_device_count >= DRIVER_MAX_DEVICES) {
        spinlock_release(&g_drv_lock);
        return -1;
    }
    if (drv_device_find_locked(dev->name)) {
        spinlock_release(&g_drv_lock);
        return -1;
    }
    drv_fill_device(dev);
    dev->next = g_devices_head;
    g_devices_head = dev;
    g_device_count++;
    dev->refcount++;
    spinlock_release(&g_drv_lock);

    debuglog(DEBUG_INFO, "[DRV] bus added device '%s' bus=%u vid=0x%x did=0x%x\n",
             dev->name, (unsigned)dev->bus, dev->vendor, dev->device_id);

    drv_bind(dev);
    drv_device_put(dev);
    return 0;
}

int drv_bus_unregister(drv_device_t* dev) {
    if (!dev) return -1;
    spinlock_acquire(&g_drv_lock);
    drv_device_t* prev = NULL;
    drv_device_t* cur = g_devices_head;
    while (cur) {
        if (cur == dev) break;
        prev = cur;
        cur = cur->next;
    }
    if (!cur) {
        spinlock_release(&g_drv_lock);
        return -1;
    }
    dev->state = DEVICE_STATE_REMOVED;
    dev->refcount++;
    spinlock_release(&g_drv_lock);

    drv_unbind(dev);
    drv_device_put(dev);

    spinlock_acquire(&g_drv_lock);
    if (prev) prev->next = dev->next;
    else       g_devices_head = dev->next;
    g_device_count--;
    spinlock_release(&g_drv_lock);
    debuglog(DEBUG_INFO, "[DRV] bus removed device '%s'\n", dev->name);
    return 0;
}

#if ENABLE_DRIVER_HOTPLUG
int drv_hotplug_add(drv_device_t* dev) {
    return drv_bus_register(dev);
}

int drv_hotplug_remove(drv_device_t* dev) {
    return drv_bus_unregister(dev);
}
#else
int drv_hotplug_add(drv_device_t* dev) { (void)dev; return -1; }
int drv_hotplug_remove(drv_device_t* dev) { (void)dev; return -1; }
#endif

int drv_rescan(void) {
    int n = 0;
    spinlock_acquire(&g_drv_lock);
    for (drv_device_t* dev = g_devices_head; dev; dev = dev->next) {
        if (dev->bound) continue;
        dev->refcount++;
        spinlock_release(&g_drv_lock);
        if (drv_bind(dev) == 0) n++;
        drv_device_put(dev);
        spinlock_acquire(&g_drv_lock);
    }
    spinlock_release(&g_drv_lock);
    return n;
}

drv_device_t* drv_device_get_by_name(const char* name) {
    if (!name) return NULL;
    spinlock_acquire(&g_drv_lock);
    drv_device_t* dev = drv_device_find_locked(name);
    if (dev) dev->refcount++;
    spinlock_release(&g_drv_lock);
    return dev;
}

const drv_driver_t* drv_iter_drivers(int (*cb)(const drv_driver_t*, void*), void* user) {
    if (!cb) return NULL;
    spinlock_acquire(&g_drv_lock);
    for (drv_driver_t* d = g_drivers_head; d; d = d->next) {
        int r = cb(d, user);
        if (r) {
            spinlock_release(&g_drv_lock);
            return d;
        }
    }
    spinlock_release(&g_drv_lock);
    return NULL;
}

const drv_device_t* drv_iter_devices(int (*cb)(const drv_device_t*, void*), void* user) {
    if (!cb) return NULL;
    spinlock_acquire(&g_drv_lock);
    for (drv_device_t* d = g_devices_head; d; d = d->next) {
        int r = cb(d, user);
        if (r) {
            spinlock_release(&g_drv_lock);
            return d;
        }
    }
    spinlock_release(&g_drv_lock);
    return NULL;
}

#else /* !ENABLE_DRIVER_MODEL — stubs so the kernel links either way. */

bool drv_core_init(void) { return true; }
void drv_core_shutdown(void) {}
int  drv_register(const drv_driver_t* drv) { (void)drv; return 0; }
void drv_unregister(const drv_driver_t* drv) { (void)drv; }
int  drv_bus_register(drv_device_t* dev) { (void)dev; return 0; }
int  drv_bus_unregister(drv_device_t* dev) { (void)dev; return 0; }
int  drv_hotplug_add(drv_device_t* dev) { (void)dev; return 0; }
int  drv_hotplug_remove(drv_device_t* dev) { (void)dev; return 0; }
int  drv_bind(drv_device_t* dev) { (void)dev; return 0; }
void drv_unbind(drv_device_t* dev) { (void)dev; }
int  drv_suspend(drv_device_t* dev, int level) { (void)dev; (void)level; return 0; }
int  drv_resume(drv_device_t* dev, int level) { (void)dev; (void)level; return 0; }
drv_device_t* drv_device_get(drv_device_t* dev) { return dev; }
void          drv_device_put(drv_device_t* dev) { (void)dev; }
drv_device_t* drv_device_get_by_name(const char* name) { (void)name; return NULL; }
const drv_driver_t* drv_iter_drivers(int (*cb)(const drv_driver_t*, void*), void* user) {
    (void)cb; (void)user; return NULL;
}
const drv_device_t* drv_iter_devices(int (*cb)(const drv_device_t*, void*), void* user) {
    (void)cb; (void)user; return NULL;
}
int drv_rescan(void) { return 0; }
int drv_bus_enumerate_pci(void) { return 0; }
int drv_bus_enumerate_usb(void) { return 0; }

#endif /* ENABLE_DRIVER_MODEL */