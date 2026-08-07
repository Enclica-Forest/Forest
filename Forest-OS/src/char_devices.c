/*
 * Character Device Drivers for Fern
 * Implements standard character devices: null, zero, random, full, tty, console
 */

#include "include/device_fs.h"
#include "include/devfs.h"
#include "include/memory_safe.h"
#include "include/debuglog.h"
#include "include/string.h"

/* Forward declare missing functions */
uint64_t read_tsc(void);
uint32_t min(uint32_t a, uint32_t b);

uint32_t min(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

/* Null Device (/dev/null) - Black hole for data */
static ssize_t null_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset);
static ssize_t null_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset);
static int null_ioctl(struct device_node *dev, uint32_t request, void *arg);

/* Zero Device (/dev/zero) - Infinite stream of null bytes */
static ssize_t zero_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset);
static ssize_t zero_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset);
static int zero_ioctl(struct device_node *dev, uint32_t request, void *arg);
static int zero_mmap(struct device_node *dev, void *addr, size_t len, uint32_t prot, uint64_t offset);

/* Random Device (/dev/random, /dev/urandom) - Random number generator */
static ssize_t random_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset);
static ssize_t random_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset);
static int random_ioctl(struct device_node *dev, uint32_t request, void *arg);

/* Full Device (/dev/full) - Always returns "no space" on write */
static ssize_t full_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset);
static ssize_t full_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset);

/* Console Device (/dev/console) - System console */
static ssize_t console_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset);
static ssize_t console_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset);
static int console_ioctl(struct device_node *dev, uint32_t request, void *arg);

/* TTY Device (/dev/tty) - Terminal interface */
static ssize_t tty_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset);
static ssize_t tty_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset);
static int tty_ioctl(struct device_node *dev, uint32_t request, void *arg);

/* Memory Device (/dev/mem) - Physical memory access */
static ssize_t mem_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset);
static ssize_t mem_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset);
static int mem_ioctl(struct device_node *dev, uint32_t request, void *arg);

/* Kernel Memory Device (/dev/kmem) - Kernel memory access */
static ssize_t kmem_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset);
static ssize_t kmem_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset);

/* Core Memory Device (/dev/core) - Kernel core memory */
static ssize_t core_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset);

/* Uevent Device (/dev/uevent) - devfs uevent queue bridge */
static ssize_t uevent_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset);
static ssize_t uevent_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset);
static int uevent_ioctl(struct device_node *dev, uint32_t request, void *arg);

/* Random number generator state */
static struct {
    uint32_t seed;
    bool initialized;
} g_random_state = {0};

/* Forward declarations */
static device_operations_t null_ops;
static device_operations_t zero_ops;
static device_operations_t random_ops;
static device_operations_t full_ops;
static device_operations_t console_ops;
static device_operations_t tty_ops;
static device_operations_t mem_ops;
static device_operations_t kmem_ops;
static device_operations_t core_ops;
static device_operations_t uevent_ops;

/*
 * Null Device Implementation
 */

static ssize_t null_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;
    return 0; /* Always return EOF */
}

static ssize_t null_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)offset;
    /* Accept all data but discard it */
    return count;
}

static int null_ioctl(struct device_node *dev, uint32_t request, void *arg)
{
    (void)dev;
    (void)request;
    (void)arg;
    return DEVICE_SUCCESS;
}

/*
 * Zero Device Implementation
 */

static ssize_t zero_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)offset;
    
    if (!buffer) {
        return DEVICE_ERROR_INVALID_PARAM;
    }
    
    /* Fill buffer with null bytes */
    memset(buffer, 0, count);
    return count;
}

static ssize_t zero_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;
    /* Discard all data */
    return count;
}

static int zero_ioctl(struct device_node *dev, uint32_t request, void *arg)
{
    (void)dev;
    (void)request;
    (void)arg;
    return DEVICE_SUCCESS;
}

static int zero_mmap(struct device_node *dev, void *addr, size_t len, uint32_t prot, uint64_t offset)
{
    (void)dev;
    (void)addr;
    (void)len;
    (void)prot;
    (void)offset;
    
    /* Map anonymous zero-filled pages */
    /* This would integrate with memory management */
    return DEVICE_SUCCESS;
}

/*
 * Random Device Implementation
 */

static void init_random_generator(void)
{
    if (!g_random_state.initialized) {
        g_random_state.seed = read_tsc(); /* Use TSC as seed */
        g_random_state.initialized = true;
        debuglog(DEBUG_INFO,"CHAR: Initialized random number generator\n");
    }
}

static uint32_t lcg_random(void)
{
    g_random_state.seed = g_random_state.seed * 1103515245 + 12345;
    return g_random_state.seed;
}

static ssize_t random_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    size_t bytes_read = 0;
    uint8_t *buf = (uint8_t*)buffer;
    
    (void)dev;
    (void)offset;
    
    if (!buffer || count == 0) {
        return 0;
    }
    
    init_random_generator();
    
    /* Generate random bytes */
    while (bytes_read < count) {
        uint32_t random_val = lcg_random();
        size_t chunk_size = min(count - bytes_read, 4);
        
        for (size_t i = 0; i < chunk_size; i++) {
            buf[bytes_read++] = (random_val >> (i * 8)) & 0xFF;
        }
    }
    
    return bytes_read;
}

static ssize_t random_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;
    /* Some systems allow seeding the random generator */
    return DEVICE_ERROR_NOT_SUPPORTED;
}

static int random_ioctl(struct device_node *dev, uint32_t request, void *arg)
{
    (void)dev;
    
    switch (request) {
        case DEVICE_IOCTL_GET_INFO: {
            device_info_t *info = (device_info_t*)arg;
            if (info) {
                strncpy(info->name, dev->name, sizeof(info->name) - 1);
                info->features = 0;
                info->readable = true;
                info->writable = false;
            }
            return DEVICE_SUCCESS;
        }
        default:
            return DEVICE_ERROR_NOT_SUPPORTED;
    }
}

/*
 * Full Device Implementation
 */

static ssize_t full_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;
    /* Reading from /dev/full returns EOF */
    return 0;
}

static ssize_t full_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;
    /* Writing to /dev/full always fails with ENOSPC */
    return -28; /* ENOSPC */
}

/*
 * Console Device Implementation
 */

static ssize_t console_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;
    
    /* This would read from the actual system console */
    /* For now, return no data available */
    return 0;
}

static ssize_t console_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)offset;
    
    if (!buffer || count == 0) {
        return 0;
    }
    
    /* Write to system console */
    /* This would integrate with Fern debug/serial output */
    const char *data = (const char*)buffer;
    
    /* Simple debug output for now */
    for (size_t i = 0; i < count; i++) {
        if (data[i] == '\0') break;
        /* In a real implementation, this would go to console output */
    }
    
    return count;
}

static int console_ioctl(struct device_node *dev, uint32_t request, void *arg)
{
    (void)dev;
    (void)request;
    (void)arg;
    
    switch (request) {
        case DEVICE_IOCTL_GET_INFO: {
            device_info_t *info = (device_info_t*)arg;
            if (info) {
                strncpy(info->name, dev->name, sizeof(info->name) - 1);
                info->features = 0;
                info->readable = true;
                info->writable = true;
            }
            return DEVICE_SUCCESS;
        }
        default:
            return DEVICE_ERROR_NOT_SUPPORTED;
    }
}

/*
 * TTY Device Implementation
 */

static ssize_t tty_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;
    
    /* This would read from TTY buffer */
    /* For now, return no data */
    return 0;
}

static ssize_t tty_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;
    
    if (!buffer || count == 0) {
        return 0;
    }
    
    /* Write to TTY */
    /* This would integrate with TTY subsystem */
    const char *data = (const char*)buffer;
    
    /* Simple implementation - write to debug */
    for (size_t i = 0; i < count; i++) {
        if (data[i] == '\0') break;
        /* In a real implementation, this would go to TTY output */
    }
    
    return count;
}

static int tty_ioctl(struct device_node *dev, uint32_t request, void *arg)
{
    (void)dev;
    (void)request;
    (void)arg;

    switch (request) {
        case DEVICE_IOCTL_GET_INFO: {
            device_info_t *info = (device_info_t*)arg;
            if (info) {
                strncpy(info->name, dev->name, sizeof(info->name) - 1);
                info->features = 0;
                info->readable = true;
                info->writable = true;
            }
            return DEVICE_SUCCESS;
        }
        default:
            return DEVICE_ERROR_NOT_SUPPORTED;
    }
}

/*
 * Memory Device Implementation (/dev/mem)
 * Provides access to physical memory
 */

static ssize_t mem_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    (void)dev;

    /* Security: restrict access to physical memory */
    /* In a real OS, this would check permissions and may be disabled */

    if (!buffer || count == 0) {
        return 0;
    }

    /* For safety, only allow reading from low memory addresses */
    /* This is a very basic implementation */
    if (offset >= 0x100000) { /* 1MB limit */
        return -DEVICE_ERROR_PERMISSION;
    }

    /* Copy from physical memory */
    /* This is dangerous and should be properly implemented with paging */
    uint8_t *phys_addr = (uint8_t*)offset;
    uint8_t *buf = (uint8_t*)buffer;

    for (size_t i = 0; i < count && offset + i < 0x100000; i++) {
        buf[i] = phys_addr[i];
    }

    return count;
}

static ssize_t mem_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;

    /* Writing to physical memory is extremely dangerous */
    /* In a real implementation, this would be heavily restricted or disabled */
    return -DEVICE_ERROR_PERMISSION;
}

static int mem_ioctl(struct device_node *dev, uint32_t request, void *arg)
{
    (void)dev;
    (void)request;
    (void)arg;

    switch (request) {
        case DEVICE_IOCTL_GET_INFO: {
            device_info_t *info = (device_info_t*)arg;
            if (info) {
                strncpy(info->name, dev->name, sizeof(info->name) - 1);
                info->features = 0;
                info->readable = true;
                info->writable = false; /* Writing disabled for security */
            }
            return DEVICE_SUCCESS;
        }
        default:
            return DEVICE_ERROR_NOT_SUPPORTED;
    }
}

/*
 * Kernel Memory Device Implementation (/dev/kmem)
 * Provides access to kernel virtual memory
 */

static ssize_t kmem_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;

    /* Kernel memory access is extremely dangerous */
    /* This should be disabled or heavily restricted */
    return -DEVICE_ERROR_PERMISSION;
}

static ssize_t kmem_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;

    /* Writing to kernel memory is catastrophic */
    return -DEVICE_ERROR_PERMISSION;
}

/*
 * Core Memory Device Implementation (/dev/core)
 * Legacy kernel core memory access
 */

static ssize_t core_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;

    /* Legacy device, similar security concerns as kmem */
    return -DEVICE_ERROR_PERMISSION;
}

/*
 * Uevent bridge implementation (/dev/uevent)
 */
static ssize_t uevent_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)offset;

    if (!buffer || count == 0) {
        return 0;
    }

    devfs_uevent_t event;
    if (!devfs_uevent_pop(&event)) {
        return 0;
    }

    int len = devfs_uevent_format(&event, (char*)buffer, (uint32_t)count);
    if (len < 0) {
        return DEVICE_ERROR;
    }

    return (ssize_t)len;
}

static ssize_t uevent_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;
    return DEVICE_ERROR_NOT_SUPPORTED;
}

static int uevent_ioctl(struct device_node *dev, uint32_t request, void *arg)
{
    (void)dev;

    switch (request) {
        case DEVICE_IOCTL_GET_INFO: {
            device_info_t *info = (device_info_t*)arg;
            if (info) {
                strncpy(info->name, dev->name, sizeof(info->name) - 1);
                info->features = 0;
                info->readable = true;
                info->writable = false;
            }
            return DEVICE_SUCCESS;
        }
        default:
            return DEVICE_ERROR_NOT_SUPPORTED;
    }
}

/*
 * Initialize character devices
 */
int char_devices_init(void)
{
    debuglog(DEBUG_INFO,"CHAR: Initializing character devices\n");
    
    /* Initialize null device operations */
    null_ops.open = NULL;
    null_ops.close = NULL;
    null_ops.read = null_read;
    null_ops.write = null_write;
    null_ops.ioctl = null_ioctl;
    null_ops.mmap = NULL;
    null_ops.poll = NULL;
    null_ops.flush = NULL;
    null_ops.suspend = NULL;
    null_ops.resume = NULL;
    null_ops.get_info = NULL;
    null_ops.set_config = NULL;
    
    /* Initialize zero device operations */
    zero_ops.open = NULL;
    zero_ops.close = NULL;
    zero_ops.read = zero_read;
    zero_ops.write = zero_write;
    zero_ops.ioctl = zero_ioctl;
    zero_ops.mmap = zero_mmap;
    zero_ops.poll = NULL;
    zero_ops.flush = NULL;
    zero_ops.suspend = NULL;
    zero_ops.resume = NULL;
    zero_ops.get_info = NULL;
    zero_ops.set_config = NULL;
    
    /* Initialize random device operations */
    random_ops.open = NULL;
    random_ops.close = NULL;
    random_ops.read = random_read;
    random_ops.write = random_write;
    random_ops.ioctl = random_ioctl;
    random_ops.mmap = NULL;
    random_ops.poll = NULL;
    random_ops.flush = NULL;
    random_ops.suspend = NULL;
    random_ops.resume = NULL;
    random_ops.get_info = NULL;
    random_ops.set_config = NULL;
    
    /* Initialize full device operations */
    full_ops.open = NULL;
    full_ops.close = NULL;
    full_ops.read = full_read;
    full_ops.write = full_write;
    full_ops.ioctl = NULL;
    full_ops.mmap = NULL;
    full_ops.poll = NULL;
    full_ops.flush = NULL;
    full_ops.suspend = NULL;
    full_ops.resume = NULL;
    full_ops.get_info = NULL;
    full_ops.set_config = NULL;
    
    /* Initialize console device operations */
    console_ops.open = NULL;
    console_ops.close = NULL;
    console_ops.read = console_read;
    console_ops.write = console_write;
    console_ops.ioctl = console_ioctl;
    console_ops.mmap = NULL;
    console_ops.poll = NULL;
    console_ops.flush = NULL;
    console_ops.suspend = NULL;
    console_ops.resume = NULL;
    console_ops.get_info = NULL;
    console_ops.set_config = NULL;
    
    /* Initialize TTY device operations */
    tty_ops.open = NULL;
    tty_ops.close = NULL;
    tty_ops.read = tty_read;
    tty_ops.write = tty_write;
    tty_ops.ioctl = tty_ioctl;
    tty_ops.mmap = NULL;
    tty_ops.poll = NULL;
    tty_ops.flush = NULL;
    tty_ops.suspend = NULL;
    tty_ops.resume = NULL;
    tty_ops.get_info = NULL;
    tty_ops.set_config = NULL;

    /* Initialize memory device operations */
    mem_ops.open = NULL;
    mem_ops.close = NULL;
    mem_ops.read = mem_read;
    mem_ops.write = mem_write;
    mem_ops.ioctl = mem_ioctl;
    mem_ops.mmap = NULL;
    mem_ops.poll = NULL;
    mem_ops.flush = NULL;
    mem_ops.suspend = NULL;
    mem_ops.resume = NULL;
    mem_ops.get_info = NULL;
    mem_ops.set_config = NULL;

    /* Initialize kernel memory device operations */
    kmem_ops.open = NULL;
    kmem_ops.close = NULL;
    kmem_ops.read = kmem_read;
    kmem_ops.write = kmem_write;
    kmem_ops.ioctl = NULL;
    kmem_ops.mmap = NULL;
    kmem_ops.poll = NULL;
    kmem_ops.flush = NULL;
    kmem_ops.suspend = NULL;
    kmem_ops.resume = NULL;
    kmem_ops.get_info = NULL;
    kmem_ops.set_config = NULL;

    /* Initialize core memory device operations */
    core_ops.open = NULL;
    core_ops.close = NULL;
    core_ops.read = core_read;
    core_ops.write = NULL;
    core_ops.ioctl = NULL;
    core_ops.mmap = NULL;
    core_ops.poll = NULL;
    core_ops.flush = NULL;
    core_ops.suspend = NULL;
    core_ops.resume = NULL;
    core_ops.get_info = NULL;
    core_ops.set_config = NULL;

    /* Initialize uevent device operations */
    uevent_ops.open = NULL;
    uevent_ops.close = NULL;
    uevent_ops.read = uevent_read;
    uevent_ops.write = uevent_write;
    uevent_ops.ioctl = uevent_ioctl;
    uevent_ops.mmap = NULL;
    uevent_ops.poll = NULL;
    uevent_ops.flush = NULL;
    uevent_ops.suspend = NULL;
    uevent_ops.resume = NULL;
    uevent_ops.get_info = NULL;
    uevent_ops.set_config = NULL;
    
    /* Register standard devices */
    
    /* /dev/null */
    device_params_t null_params = {
        .name = "null",
        .major = UNNAMED_MAJOR,
        .minor = 1,
        .type = DT_CHR,
        .mode = S_IRWXU | S_IRWXG | S_IRWXO,
        .uid = 0,
        .gid = 0,
        .ops = &null_ops,
        .private_data = NULL
    };
    device_register(&null_params);
    
    /* /dev/zero */
    device_params_t zero_params = {
        .name = "zero",
        .major = UNNAMED_MAJOR,
        .minor = 2,
        .type = DT_CHR,
        .mode = S_IRWXU | S_IRWXG | S_IRWXO,
        .uid = 0,
        .gid = 0,
        .ops = &zero_ops,
        .private_data = NULL
    };
    device_register(&zero_params);
    
    /* /dev/random */
    device_params_t random_params = {
        .name = "random",
        .major = UNNAMED_MAJOR,
        .minor = 3,
        .type = DT_CHR,
        .mode = S_IRUSR | S_IRGRP | S_IROTH,
        .uid = 0,
        .gid = 0,
        .ops = &random_ops,
        .private_data = &g_random_state
    };
    device_register(&random_params);
    
    /* /dev/urandom */
    device_params_t urandom_params = {
        .name = "urandom",
        .major = UNNAMED_MAJOR,
        .minor = 4,
        .type = DT_CHR,
        .mode = S_IRUSR | S_IRGRP | S_IROTH,
        .uid = 0,
        .gid = 0,
        .ops = &random_ops,
        .private_data = &g_random_state
    };
    device_register(&urandom_params);
    
    /* /dev/full */
    device_params_t full_params = {
        .name = "full",
        .major = UNNAMED_MAJOR,
        .minor = 5,
        .type = DT_CHR,
        .mode = S_IWUSR | S_IWGRP | S_IWOTH,
        .uid = 0,
        .gid = 0,
        .ops = &full_ops,
        .private_data = NULL
    };
    device_register(&full_params);
    
    /* /dev/console */
    device_params_t console_params = {
        .name = "console",
        .major = CONSOLE_MAJOR,
        .minor = 0,
        .type = DT_CHR,
        .mode = S_IRWXU | S_IRWXG | S_IRWXO,
        .uid = 0,
        .gid = 0,
        .ops = &console_ops,
        .private_data = NULL
    };
    device_register(&console_params);
    
    /* /dev/tty */
    device_params_t tty_params = {
        .name = "tty",
        .major = TTY_MAJOR,
        .minor = 0,
        .type = DT_CHR,
        .mode = S_IRWXU | S_IRWXG | S_IRWXO,
        .uid = 0,
        .gid = 0,
        .ops = &tty_ops,
        .private_data = NULL
    };
    device_register(&tty_params);

    /* /dev/mem */
    device_params_t mem_params = {
        .name = "mem",
        .major = MEM_MAJOR,
        .minor = 1,
        .type = DT_CHR,
        .mode = S_IRUSR | S_IWUSR, /* Root only, restricted permissions */
        .uid = 0,
        .gid = 0,
        .ops = &mem_ops,
        .private_data = NULL
    };
    device_register(&mem_params);

    /* /dev/kmem */
    device_params_t kmem_params = {
        .name = "kmem",
        .major = MEM_MAJOR,
        .minor = 2,
        .type = DT_CHR,
        .mode = S_IRUSR | S_IWUSR, /* Root only, restricted permissions */
        .uid = 0,
        .gid = 0,
        .ops = &kmem_ops,
        .private_data = NULL
    };
    device_register(&kmem_params);

    /* /dev/core */
    device_params_t core_params = {
        .name = "core",
        .major = MEM_MAJOR,
        .minor = 3,
        .type = DT_CHR,
        .mode = S_IRUSR, /* Read-only, root only */
        .uid = 0,
        .gid = 0,
        .ops = &core_ops,
        .private_data = NULL
    };
    device_register(&core_params);

    /* /dev/uevent */
    device_params_t uevent_params = {
        .name = "uevent",
        .major = MISC_MAJOR,
        .minor = 0,
        .type = DT_CHR,
        .mode = S_IRUSR | S_IRGRP | S_IROTH,
        .uid = 0,
        .gid = 0,
        .ops = &uevent_ops,
        .private_data = NULL
    };
    device_register(&uevent_params);

    /* Apply security policies to dangerous devices */
    extern int device_set_security_policy(const char*, uint32_t);
    device_set_security_policy("mem", 1);   /* Restrict /dev/mem to root */
    device_set_security_policy("kmem", 1);  /* Restrict /dev/kmem to root */
    device_set_security_policy("core", 1);  /* Restrict /dev/core to root */

    debuglog(DEBUG_INFO,"CHAR: Character devices initialized\n");
    return 0;
}

/*
 * Cleanup character devices
 */
void char_devices_cleanup(void)
{
    debuglog(DEBUG_INFO,"CHAR: Cleaning up character devices\n");
    /* Devices are automatically cleaned up by device_fs_cleanup() */
}
