/*
 * Serial Device Drivers for Fern
 * Implements standard serial port devices: /dev/ttyS*, /dev/ttyUSB*, /dev/ttyACM*, /dev/ttyAMA*
 */

#include "include/device_fs.h"
#include "include/memory_safe.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/io_ports.h"

/* Serial port definitions */
#define SERIAL_COM1_BASE 0x3F8
#define SERIAL_COM2_BASE 0x2F8
#define SERIAL_COM3_BASE 0x3E8
#define SERIAL_COM4_BASE 0x2E8

/* Serial port structure */
typedef struct serial_port {
    uint16_t base_port;
    uint16_t irq;
    bool initialized;
    uint8_t line_status;
    /* Buffers for input/output */
    char input_buffer[1024];
    uint32_t input_head;
    uint32_t input_tail;
    char output_buffer[1024];
    uint32_t output_head;
    uint32_t output_tail;
} serial_port_t;

/* Global serial ports */
static serial_port_t g_serial_ports[4] = {
    {SERIAL_COM1_BASE, 4, false, 0, {0}, 0, 0, {0}, 0, 0}, /* COM1 */
    {SERIAL_COM2_BASE, 3, false, 0, {0}, 0, 0, {0}, 0, 0}, /* COM2 */
    {SERIAL_COM3_BASE, 4, false, 0, {0}, 0, 0, {0}, 0, 0}, /* COM3 */
    {SERIAL_COM4_BASE, 3, false, 0, {0}, 0, 0, {0}, 0, 0}  /* COM4 */
};

/* Forward declarations for device operations */
static device_operations_t ttyS_ops;

/* Serial port functions */
static void serial_init_port(serial_port_t *port) {
    if (port->initialized) return;

    uint16_t base = port->base_port;

    /* Disable interrupts */
    outportb(base + 1, 0x00);

    /* Enable DLAB */
    outportb(base + 3, 0x80);

    /* Set divisor to 3 (38400 baud) */
    outportb(base + 0, 0x03);
    outportb(base + 1, 0x00);

    /* 8 bits, no parity, one stop bit */
    outportb(base + 3, 0x03);

    /* Enable FIFO, clear them, 14-byte threshold */
    outportb(base + 2, 0xC7);

    /* IRQs enabled, RTS/DSR set */
    outportb(base + 4, 0x0B);

    port->initialized = true;
    port->input_head = 0;
    port->input_tail = 0;
    port->output_head = 0;
    port->output_tail = 0;
}

static bool serial_can_read(serial_port_t *port) {
    uint16_t base = port->base_port;
    return (inportb(base + 5) & 0x01) != 0;
}

static bool serial_can_write(serial_port_t *port) {
    uint16_t base = port->base_port;
    return (inportb(base + 5) & 0x20) != 0;
}

static char serial_read_char(serial_port_t *port) {
    uint16_t base = port->base_port;
    while (!serial_can_read(port));
    return inportb(base);
}

static void serial_write_char(serial_port_t *port, char c) {
    uint16_t base = port->base_port;
    while (!serial_can_write(port));
    outportb(base, (uint8_t)c);
}

/* Device operations for /dev/ttyS* */

static ssize_t ttyS_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset) {
    (void)offset;

    if (!dev || !buffer || count == 0) return 0;

    /* Get port index from minor number */
    int port_index = dev->minor - 64; /* ttyS0 = 64, ttyS1 = 65, etc. */
    if (port_index < 0 || port_index >= 4) return -DEVICE_ERROR_INVALID_PARAM;

    serial_port_t *port = &g_serial_ports[port_index];
    if (!port->initialized) serial_init_port(port);

    char *buf = (char*)buffer;
    size_t read = 0;

    /* Read from input buffer */
    while (read < count && port->input_head != port->input_tail) {
        buf[read++] = port->input_buffer[port->input_tail];
        port->input_tail = (port->input_tail + 1) % sizeof(port->input_buffer);
    }

    /* If buffer empty, try direct read */
    if (read == 0 && serial_can_read(port)) {
        buf[read++] = serial_read_char(port);
    }

    return read > 0 ? read : 0;
}

static ssize_t ttyS_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset) {
    (void)offset;

    if (!dev || !buffer || count == 0) return 0;

    /* Get port index from minor number */
    int port_index = dev->minor - 64; /* ttyS0 = 64, ttyS1 = 65, etc. */
    if (port_index < 0 || port_index >= 4) return -DEVICE_ERROR_INVALID_PARAM;

    serial_port_t *port = &g_serial_ports[port_index];
    if (!port->initialized) serial_init_port(port);

    const char *buf = (const char*)buffer;
    size_t written = 0;

    /* Write characters */
    for (size_t i = 0; i < count; i++) {
        serial_write_char(port, buf[i]);
        written++;
    }

    return written;
}

static int ttyS_ioctl(struct device_node *dev, uint32_t request, void *arg) {
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

/* Initialize serial devices */
int serial_devices_init(void) {
    debuglog_printf("SERIAL: Initializing serial devices\n");

    /* Initialize device operations */
    ttyS_ops.open = NULL;
    ttyS_ops.close = NULL;
    ttyS_ops.read = ttyS_read;
    ttyS_ops.write = ttyS_write;
    ttyS_ops.ioctl = ttyS_ioctl;
    ttyS_ops.mmap = NULL;
    ttyS_ops.poll = NULL;
    ttyS_ops.flush = NULL;
    ttyS_ops.suspend = NULL;
    ttyS_ops.resume = NULL;
    ttyS_ops.get_info = NULL;
    ttyS_ops.set_config = NULL;

    /* Register standard serial ports */
    const char *serial_names[] = {"ttyS0", "ttyS1", "ttyS2", "ttyS3"};
    uint16_t serial_majors[] = {4, 4, 4, 4}; /* All use TTY_MAJOR */
    uint16_t serial_minors[] = {64, 65, 66, 67}; /* ttyS0=64, ttyS1=65, etc. */

    for (int i = 0; i < 4; i++) {
        device_params_t params = {
            .name = serial_names[i],
            .major = serial_majors[i],
            .minor = serial_minors[i],
            .type = DT_CHR,
            .mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH, /* rw-rw-rw- */
            .uid = 0,
            .gid = 0,
            .ops = &ttyS_ops,
            .private_data = &g_serial_ports[i]
        };
        device_register(&params);
    }

    debuglog_printf("SERIAL: Serial devices initialized\n");
    return 0;
}

/* Cleanup serial devices */
void serial_devices_cleanup(void) {
    debuglog_printf("SERIAL: Cleaning up serial devices\n");
    /* Devices are automatically cleaned up by device_fs_cleanup() */
}