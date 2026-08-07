#ifndef DRIVER_CONFIG_H
#define DRIVER_CONFIG_H

/*
 * Build-time feature flags for the unified driver model in driver.h /
 * driver_registry.c / driver_bus.c / driverctl_dev.c.
 *
 * All flags default to ON. Define any of them to 0 in build-config.mk or
 * on the compiler command line to disable the corresponding subsystem.
 * When disabled, the public API in driver.h resolves to no-op stubs so
 * the kernel links and existing callers compile untouched.
 */

#ifndef ENABLE_DRIVER_MODEL
#define ENABLE_DRIVER_MODEL 1
#endif

#ifndef ENABLE_DRIVER_HOTPLUG
#define ENABLE_DRIVER_HOTPLUG 1
#endif

#ifndef ENABLE_DRIVER_PM
#define ENABLE_DRIVER_PM 1
#endif

#ifndef ENABLE_DRIVER_UEVENT
#define ENABLE_DRIVER_UEVENT 1
#endif

#ifndef DRIVER_MAX_DEVICES
#define DRIVER_MAX_DEVICES 256
#endif

#ifndef DRIVER_MAX_DRIVERS
#define DRIVER_MAX_DRIVERS 128
#endif

#endif /* DRIVER_CONFIG_H */