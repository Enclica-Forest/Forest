/*
 * Character Device Drivers for Fern (Simplified)
 * Implements standard character devices: null, zero, random, full
 */

#include "include/device_fs.h"
#include "include/memory_safe.h"
#include "include/debuglog.h"
#include "include/debug.h"
#include "include/string.h"

/* Simple LCG random generator */
static struct {
    uint32_t seed;
    bool initialized;
} simple_rng = {0};

static uint32_t simple_random(void)
{
    if (!simple_rng.initialized) {
        simple_rng.seed = 12345; /* Fixed seed */
        simple_rng.initialized = true;
    }
    simple_rng.seed = simple_rng.seed * 1103515245 + 12345;
    return simple_rng.seed;
}

/* Null device (/dev/null) */
static ssize_t null_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev; (void)buffer; (void)offset;
    return count; /* Accept and discard */
}

static int null_open(struct device_node *dev, uint32_t flags)
{
    (void)dev; (void)flags;
    return 0;
}

/* Zero device (/dev/zero) */
static ssize_t zero_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    (void)dev; (void)offset;
    if (buffer && count > 0) {
        memset(buffer, 0, count);
    }
    return count;
}

static ssize_t zero_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev; (void)buffer; (void)offset;
    return count; /* Accept and discard */
}

/* Random device (/dev/random) */
static ssize_t random_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    (void)dev; (void)offset;
    if (buffer && count > 0) {
        uint8_t *buf = (uint8_t*)buffer;
        for (size_t i = 0; i < count; i++) {
            buf[i] = (uint8_t)(simple_random() & 0xFF);
        }
    }
    return count;
}

/* Full device (/dev/full) */
static ssize_t full_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev; (void)buffer; (void)count; (void)offset;
    /* Always return no space error */
    return -28;
}

/* Device operations structures */
static struct device_operations null_ops = {
    .open = null_open,
    .close = NULL,
    .read = NULL,
    .write = null_write,
    .ioctl = NULL,
    .mmap = NULL,
    .poll = NULL,
    .flush = NULL,
    .suspend = NULL,
    .resume = NULL,
    .get_info = NULL,
    .set_config = NULL
};

static struct device_operations zero_ops = {
    .open = NULL,
    .close = NULL,
    .read = zero_read,
    .write = zero_write,
    .ioctl = NULL,
    .mmap = NULL,
    .poll = NULL,
    .flush = NULL,
    .suspend = NULL,
    .resume = NULL,
    .get_info = NULL,
    .set_config = NULL
};

static struct device_operations random_ops = {
    .open = NULL,
    .close = NULL,
    .read = random_read,
    .write = NULL,
    .ioctl = NULL,
    .mmap = NULL,
    .poll = NULL,
    .flush = NULL,
    .suspend = NULL,
    .resume = NULL,
    .get_info = NULL,
    .set_config = NULL
};

static struct device_operations full_ops = {
    .open = NULL,
    .close = NULL,
    .read = NULL,
    .write = full_write,
    .ioctl = NULL,
    .mmap = NULL,
    .poll = NULL,
    .flush = NULL,
    .suspend = NULL,
    .resume = NULL,
    .get_info = NULL,
    .set_config = NULL
};

/*
 * Initialize character devices
 */
int char_devices_init(void)
{
    debug_print("CHAR: Initializing character devices\n");
    
    /* Register standard Unix character devices */
    
    /* /dev/null - black hole */
    device_params_t null_params = {
        .name = "null",
        .major = 1, /* UNNAMED_MAJOR */
        .minor = 3,
        .type = DT_CHR,
        .mode = S_IRWXU | S_IRWXG | S_IRWXO,
        .uid = 0,
        .gid = 0,
        .ops = &null_ops,
        .private_data = NULL
    };
    device_register(&null_params);
    
    /* /dev/zero - source of null bytes */
    device_params_t zero_params = {
        .name = "zero",
        .major = 1,
        .minor = 5,
        .type = DT_CHR,
        .mode = S_IRUSR | S_IRGRP | S_IROTH,
        .uid = 0,
        .gid = 0,
        .ops = &zero_ops,
        .private_data = NULL
    };
    device_register(&zero_params);
    
    /* /dev/random - source of random bytes */
    device_params_t random_params = {
        .name = "random",
        .major = 1,
        .minor = 8,
        .type = DT_CHR,
        .mode = S_IRUSR | S_IRGRP | S_IROTH,
        .uid = 0,
        .gid = 0,
        .ops = &random_ops,
        .private_data = &simple_rng
    };
    device_register(&random_params);
    
    /* /dev/full - always full */
    device_params_t full_params = {
        .name = "full",
        .major = 1,
        .minor = 7,
        .type = DT_CHR,
        .mode = S_IWUSR | S_IWGRP | S_IWOTH,
        .uid = 0,
        .gid = 0,
        .ops = &full_ops,
        .private_data = NULL
    };
    device_register(&full_params);
    
    debug_print("CHAR: Character devices initialized\n");
    return 0;
}

/*
 * Cleanup character devices
 */
void char_devices_cleanup(void)
{
    debug_print("CHAR: Cleaning up character devices\n");
}