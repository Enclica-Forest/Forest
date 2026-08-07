#ifndef DRIVERCTL_H
#define DRIVERCTL_H

#include "types.h"

/*
 * /dev/driverctl interface.
 *
 * Implemented by src/driverctl_dev.c and surfaced through devfs. The
 * device accepts ioctl(2) calls with the commands below and returns
 * structured records to userspace so a future `driverctl` tool can list
 * drivers/devices, query information and force bind/unbind.
 */
#define DRIVERCTL_MAGIC 'D'

#define DRIVERCTL_LIST_DRIVERS    0x4401
#define DRIVERCTL_LIST_DEVICES    0x4402
#define DRIVERCTL_GET_DRIVER_INFO 0x4403
#define DRIVERCTL_GET_DEVICE_INFO 0x4404
#define DRIVERCTL_BIND            0x4405
#define DRIVERCTL_UNBIND          0x4406
#define DRIVERCTL_RESCAN          0x4407

#define DRIVERCTL_NAME_MAX 64

typedef struct {
    char     name[DRIVERCTL_NAME_MAX];
    char     version[32];
    uint32_t bus;
    uint32_t flags;
    uint32_t id;       /* registry slot id */
    uint32_t pad;
} driverctl_driver_info_t;

typedef struct {
    char     name[DRIVERCTL_NAME_MAX];
    uint32_t bus;
    uint32_t id;
    uint16_t vendor;
    uint16_t device_id;
    uint16_t subvendor;
    uint16_t subdevice;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  state;
    char     bound_driver[DRIVERCTL_NAME_MAX]; /* "" if unbound */
    uint32_t pad;
} driverctl_device_info_t;

typedef struct {
    char name[DRIVERCTL_NAME_MAX];   /* device name for BIND/UNBIND */
    uint32_t pad;
} driverctl_name_arg_t;

/* /dev/driverctl lifecycle. */
#include <stdbool.h>
bool driverctl_dev_init(void);
void driverctl_dev_shutdown(void);

#endif /* DRIVERCTL_H */