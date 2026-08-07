#include "include/ata.h"
#include "include/pci.h"
#include "include/interrupt.h"
#include "include/io_ports.h"
#include "include/debug.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/cpu_ops.h"
#include "include/driver.h"

#define ATA_TIMEOUT_MS 5000
#define ATA_PIO_TIMEOUT_MS 30000
#define ATAPI_TIMEOUT_MS 1000
#define ATA_SECTOR_SIZE 512
#define ATAPI_SECTOR_SIZE 2048

static uint8_t ata_inb(uint16_t port) {
    return inb(port);
}

static void ata_outb(uint16_t port, uint8_t value) {
    outb(port, value);
}

static uint16_t ata_inw(uint16_t port) {
    return inportd(port);
}

static void ata_outw(uint16_t port, uint16_t value) {
    outportd(port, value);
}

static void ata_delay(ata_channel_t channel) {
    (void)channel;
    io_wait();
    io_wait();
}

static bool ata_wait_ready(ata_channel_t channel) {
    // Alternate Status is at ctrl_base+0, not +1 (that's the rarely-used
    // Drive Address register) - reading the wrong register here meant this
    // function operated on essentially unrelated bits.
    uint16_t ctrl_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_CTRL_BASE_PRIMARY : ATA_CTRL_BASE_SECONDARY;
    uint64_t timeout = ATA_TIMEOUT_MS * 1000;
    uint64_t start = read_tsc() / 2000;

    while ((read_tsc() / 2000) - start < timeout) {
        uint8_t status = ata_inb(ctrl_base);
        if ((status & (ATA_STATUS_BSY | ATA_STATUS_DRQ | ATA_STATUS_RDY)) == ATA_STATUS_RDY) {
            return true;
        }
        if (status & ATA_STATUS_ERR) {
            return false;
        }
    }
    return false;
}

static bool ata_wait_not_busy(ata_channel_t channel) {
    // See ata_wait_ready() above: Alternate Status is ctrl_base+0.
    uint16_t ctrl_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_CTRL_BASE_PRIMARY : ATA_CTRL_BASE_SECONDARY;
    uint64_t timeout = ATA_TIMEOUT_MS * 1000;
    uint64_t start = read_tsc() / 2000;

    while ((read_tsc() / 2000) - start < timeout) {
        uint8_t status = ata_inb(ctrl_base);
        if (!(status & ATA_STATUS_BSY)) {
            return true;
        }
    }
    return false;
}

__attribute__((unused)) static ata_error_t ata_get_error(ata_channel_t channel) {
    // The Error register lives in the command block at io_base+1, not the
    // control block - this previously read ctrl_base+1 (Drive Address),
    // an unrelated register.
    uint16_t io_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_IO_BASE_PRIMARY : ATA_IO_BASE_SECONDARY;
    uint8_t error = ata_inb(io_base + 1);

    if (error & 0x04) return ATA_ERROR_ID_NOT_FOUND;
    if (error & 0x02) return ATA_ERROR_COMMAND_ABORTED;
    if (error & 0x01) return ATA_ERROR_BAD_SECTOR;
    return ATA_ERROR_NONE;
}

static void ata_select_device(ata_channel_t channel, ata_device_select_t select) {
    uint16_t io_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_IO_BASE_PRIMARY : ATA_IO_BASE_SECONDARY;
    uint8_t dev = (select == ATA_DEV_MASTER) ? 0xA0 : 0xB0;
    ata_outb(io_base + 6, dev);
    ata_delay(channel);
}

static bool ata_send_command(ata_channel_t channel, ata_registers_t *cmd) {
    uint16_t io_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_IO_BASE_PRIMARY : ATA_IO_BASE_SECONDARY;

    if (!ata_wait_not_busy(channel)) {
        return false;
    }

    ata_outb(io_base + 1, cmd->features);
    ata_outb(io_base + 2, cmd->sector_count);
    ata_outb(io_base + 3, cmd->lba_low);
    ata_outb(io_base + 4, cmd->lba_mid);
    ata_outb(io_base + 5, cmd->lba_high);
    ata_outb(io_base + 6, cmd->device);
    ata_outb(io_base + 7, cmd->command);

    return true;
}

static void ata_pio_read(ata_channel_t channel, void *buffer, uint32_t words) {
    uint16_t io_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_IO_BASE_PRIMARY : ATA_IO_BASE_SECONDARY;
    uint16_t *buf = (uint16_t *)buffer;

    for (uint32_t i = 0; i < words; i++) {
        buf[i] = ata_inw(io_base);
    }
}

static void ata_pio_write(ata_channel_t channel, const void *buffer, uint32_t words) {
    uint16_t io_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_IO_BASE_PRIMARY : ATA_IO_BASE_SECONDARY;
    const uint16_t *buf = (const uint16_t *)buffer;

    for (uint32_t i = 0; i < words; i++) {
        ata_outw(io_base, buf[i]);
    }
}

static bool ata_identify_device(ata_channel_t channel, ata_device_select_t select, ata_device_info_t *info) {
    uint16_t io_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_IO_BASE_PRIMARY : ATA_IO_BASE_SECONDARY;
    uint16_t buffer[256];
    ata_registers_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.command = ATA_CMD_IDENTIFY;
    cmd.device = (select == ATA_DEV_MASTER) ? 0xA0 : 0xB0;

    ata_select_device(channel, select);

    if (!ata_send_command(channel, &cmd)) {
        return false;
    }

    uint8_t status = ata_inb(io_base + 7);
    if (status == 0) {
        return false;
    }

    if (!ata_wait_not_busy(channel)) {
        return false;
    }

    if (status & ATA_STATUS_ERR) {
        return false;
    }

    ata_pio_read(channel, buffer, 256);

    info->sector_size = 512;
    info->supports_lba48 = (buffer[83] & 0x0400) ? true : false;
    info->dma_capable = (buffer[49] & 0x0100) ? true : false;
    info->exists = true;

    if (info->supports_lba48) {
        info->sectors = ((uint64_t)buffer[103] << 48) | ((uint64_t)buffer[102] << 32) |
                        ((uint64_t)buffer[101] << 16) | buffer[100];
    } else {
        info->sectors = ((uint32_t)buffer[61] << 16) | buffer[60];
    }

    memcpy(info->model, buffer + 27, 40);
    for (int i = 0; i < 40; i += 2) {
        uint8_t tmp = info->model[i];
        info->model[i] = info->model[i + 1];
        info->model[i + 1] = tmp;
    }
    info->model[40] = '\0';

    return true;
}

static bool atapi_identify_device(ata_channel_t channel, ata_device_select_t select, ata_device_info_t *info) {
    uint16_t io_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_IO_BASE_PRIMARY : ATA_IO_BASE_SECONDARY;
    uint16_t buffer[256];
    ata_registers_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.command = ATA_CMD_ATAPI_IDENTIFY;
    cmd.device = (select == ATA_DEV_MASTER) ? 0xA0 : 0xB0;

    ata_select_device(channel, select);

    if (!ata_send_command(channel, &cmd)) {
        return false;
    }

    uint8_t status = ata_inb(io_base + 7);
    if (status == 0) {
        return false;
    }

    ata_pio_read(channel, buffer, 256);

    info->atapi_removable = (buffer[0] & 0x0080) ? true : false;
    info->sector_size = 2048;
    info->exists = true;
    info->sectors = 0;
    info->dma_capable = false;
    info->supports_lba48 = false;

    memcpy(info->model, buffer + 27, 40);
    for (int i = 0; i < 40; i += 2) {
        uint8_t tmp = info->model[i];
        info->model[i] = info->model[i + 1];
        info->model[i + 1] = tmp;
    }
    info->model[40] = '\0';

    return true;
}

static bool atapi_send_packet(ata_channel_t channel, const uint8_t *packet, uint16_t packet_size, void *data, uint32_t *data_size) {
    uint16_t io_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_IO_BASE_PRIMARY : ATA_IO_BASE_SECONDARY;
    uint16_t ctrl_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_CTRL_BASE_PRIMARY : ATA_CTRL_BASE_SECONDARY;
    ata_registers_t cmd;
    uint32_t transfer_words;
    uint8_t status;

    if (!ata_wait_not_busy(channel)) {
        return false;
    }

    ata_outb(ctrl_base, 0x00);

    cmd.command = ATA_CMD_PACKET;
    cmd.device = 0x00;
    cmd.sector_count = (uint8_t)(packet_size & 0xFF);

    if (!ata_send_command(channel, &cmd)) {
        return false;
    }

    status = ata_inb(io_base + 7);
    if (status & ATA_STATUS_ERR) {
        return false;
    }

    if (!(status & ATA_STATUS_DRQ)) {
        return false;
    }

    ata_pio_write(channel, packet, packet_size / 2);

    if (!ata_wait_not_busy(channel)) {
        return false;
    }

    status = ata_inb(io_base + 7);
    if (status & ATA_STATUS_ERR) {
        return false;
    }

    if (status & ATA_STATUS_DRQ) {
        uint16_t byte_count = ata_inb(io_base + 4) | (ata_inb(io_base + 5) << 8);
        transfer_words = byte_count / 2;

        if (data && data_size && *data_size > 0) {
            uint32_t words_to_read = (*data_size / 2) < transfer_words ? (*data_size / 2) : transfer_words;
            ata_pio_read(channel, data, words_to_read);
            *data_size = words_to_read * 2;
        } else {
            uint16_t discard[256];
            ata_pio_read(channel, discard, transfer_words > 256 ? 256 : transfer_words);
        }
    }

    return true;
}

static irq_return_t ata_irq_handler(int irq, void *dev_id, struct interrupt_context *ctx) {
    (void)irq;
    (void)dev_id;
    (void)ctx;
    return IRQ_HANDLED;
}

ata_controller_t g_ata_controller = {0};

static int ata_drv_probe(drv_device_t* dev, const drv_id_t* id) {
    (void)dev; (void)id;
    return 0;
}

static void ata_drv_remove(drv_device_t* dev) {
    (void)dev;
}

static const drv_id_t g_ata_drv_ids[] = {
    { DRV_ID_ANY, DRV_ID_ANY, DRV_ID_ANY, DRV_ID_ANY, DRV_BUS_PCI,
      0x01, 0x01, 0xFF, DRV_MATCH_CLASS, 0 },
    DRV_ID_TABLE_END
};

static drv_driver_t g_ata_drv = {
    .name = "ata",
    .version = "1.0",
    .bus = DRV_BUS_PCI,
    .id_table = g_ata_drv_ids,
    .probe = ata_drv_probe,
    .remove = ata_drv_remove,
    .flags = DRV_FLAG_PM,
};

bool ata_init(void) {
    debug_print("ATA: Initializing ATA/ATAPI controller\n");

    memset(&g_ata_controller, 0, sizeof(g_ata_controller));

    g_ata_controller.io_base = ATA_IO_BASE_PRIMARY;
    g_ata_controller.ctrl_base = ATA_CTRL_BASE_PRIMARY;
    g_ata_controller.bm_base = 0;
    g_ata_controller.irq = ATA_IRQ_PRIMARY;

    spinlock_init(&g_ata_controller.lock, "ata_controller");

    outb(g_ata_controller.ctrl_base, 0x02);

    ata_detect_devices();

    // g_ata_controller.irq stores the raw legacy IRQ number (14) for EOI/debug
    // purposes, but request_irq()/interrupt_handlers[] are indexed by absolute
    // IDT vector. Without the PIC remap offset this collided with CPU exception
    // vector 14 (page fault) -- ata_irq_handler silently replaced the page-fault
    // handler system-wide the moment ATA initialized, since nothing else here
    // applies the offset for us.
    if (request_irq(IRQ_BASE_OFFSET + g_ata_controller.irq, ata_irq_handler, IRQF_SHARED, "ata", &g_ata_controller) != 0) {
        debug_print("ATA: Warning: Failed to request IRQ %d\n", g_ata_controller.irq);
    }

    g_ata_controller.initialized = true;

    debug_print("ATA: Controller initialized\n");

    drv_register(&g_ata_drv);

    return true;
}

void ata_shutdown(void) {
    debug_print("ATA: Shutting down ATA controller\n");

    outb(g_ata_controller.ctrl_base, 0x02);

    if (g_ata_controller.irq) {
        free_irq_advanced(IRQ_BASE_OFFSET + g_ata_controller.irq, &g_ata_controller);
    }

    g_ata_controller.initialized = false;
    debug_print("ATA: Controller shut down\n");
}

bool ata_detect_devices(void) {
    debug_print("ATA: Detecting devices on channels\n");

    ata_channel_t channels[] = {ATA_CHANNEL_PRIMARY, ATA_CHANNEL_SECONDARY};

    for (int c = 0; c < 2; c++) {
        ata_channel_t channel = channels[c];
        uint16_t io_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_IO_BASE_PRIMARY : ATA_IO_BASE_SECONDARY;
        uint16_t ctrl_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_CTRL_BASE_PRIMARY : ATA_CTRL_BASE_SECONDARY;
        uint8_t irq = (channel == ATA_CHANNEL_PRIMARY) ? ATA_IRQ_PRIMARY : ATA_IRQ_SECONDARY;

        g_ata_controller.channel.io_base = io_base;
        g_ata_controller.channel.ctrl_base = ctrl_base;
        g_ata_controller.channel.irq = irq;

        debug_print("ATA: Checking channel %d (IO=0x%X, IRQ=%d)\n", channel, io_base, irq);

        for (int d = 0; d < 2; d++) {
            ata_device_select_t select = (ata_device_select_t)d;
            ata_device_t *device = &g_ata_controller.devices[c][d];
            ata_device_info_t *info = &device->info;

            memset(info, 0, sizeof(ata_device_info_t));
            info->type = ATA_DEV_TYPE_NONE;

            ata_select_device(channel, select);

            io_wait();
            io_wait();

            outb(io_base + 6, (select == ATA_DEV_MASTER) ? 0xA0 : 0xB0);
            outb(io_base + 7, 0xEC);

            uint8_t status = ata_inb(io_base + 7);
            io_wait();

            if (status == 0) {
                debug_print("ATA:  Channel %d %s: No device\n", channel, (select == ATA_DEV_MASTER) ? "master" : "slave");
                continue;
            }

            if (!ata_wait_not_busy(channel)) {
                debug_print("ATA:  Channel %d %s: Device not ready\n", channel, (select == ATA_DEV_MASTER) ? "master" : "slave");
                continue;
            }

            status = ata_inb(io_base + 7);

            if (status & ATA_STATUS_ERR) {
                io_wait();

                ata_outb(io_base + 6, (select == ATA_DEV_MASTER) ? 0xA0 : 0xB0);
                ata_outb(io_base + 7, 0xA1);

                io_wait();

                if (!ata_wait_not_busy(channel)) {
                    debug_print("ATA:  Channel %d %s: ATAPI device not responding\n", channel, (select == ATA_DEV_MASTER) ? "master" : "slave");
                    continue;
                }

                if (atapi_identify_device(channel, select, info)) {
                    info->type = ATA_DEV_TYPE_ATAPI;
                    debug_print("ATA:  Channel %d %s: ATAPI device found - %s\n", channel, (select == ATA_DEV_MASTER) ? "master" : "slave", info->model);
                }
            } else {
                if (ata_identify_device(channel, select, info)) {
                    info->type = ATA_DEV_TYPE_ATA;
                    debug_print("ATA:  Channel %d %s: ATA device found - %s (%llu sectors)\n", channel, (select == ATA_DEV_MASTER) ? "master" : "slave", info->model, (unsigned long long)info->sectors);
                }
            }

            device->channel = channel;
            device->select = select;
        }
    }

    ata_dump_devices();

    return true;
}

ata_device_t *ata_get_device(ata_channel_t channel, ata_device_select_t select) {
    if (channel >= ATA_MAX_CHANNELS || select >= ATA_MAX_DEVICES_PER_CHANNEL) {
        return NULL;
    }
    return &g_ata_controller.devices[channel][select];
}

int ata_read_sectors(ata_device_t *device, uint64_t lba, uint32_t count, void *buffer) {
    if (!device || !device->info.exists || device->info.type != ATA_DEV_TYPE_ATA) {
        return -1;
    }

    ata_channel_t channel = device->channel;
    uint16_t io_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_IO_BASE_PRIMARY : ATA_IO_BASE_SECONDARY;
    uint16_t ctrl_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_CTRL_BASE_PRIMARY : ATA_CTRL_BASE_SECONDARY;
    (void)ctrl_base;
    uint32_t sectors_per_op = 256;
    uint8_t *buf = (uint8_t *)buffer;
    uint32_t total_read = 0;

    spinlock_acquire(&g_ata_controller.lock);

    ata_select_device(channel, device->select);

    while (count > 0) {
        uint32_t sectors_this_op = (count < sectors_per_op) ? count : sectors_per_op;
        ata_registers_t cmd;

        memset(&cmd, 0, sizeof(cmd));

        if (device->info.supports_lba48 && lba + sectors_this_op > 0x10000000ULL) {
            cmd.command = ATA_CMD_READ_PIO_EXT;
            cmd.sector_count = (sectors_this_op >> 0) & 0xFF;
            cmd.lba_low = (lba >> 0) & 0xFF;
            cmd.lba_mid = (lba >> 8) & 0xFF;
            cmd.lba_high = (lba >> 16) & 0xFF;
            cmd.device = 0x40;
        } else {
            cmd.command = ATA_CMD_READ_PIO;
            cmd.sector_count = (sectors_this_op >> 0) & 0xFF;
            cmd.lba_low = (lba >> 0) & 0xFF;
            cmd.lba_mid = (lba >> 8) & 0xFF;
            cmd.lba_high = (lba >> 16) & 0xFF;
            cmd.device = 0xE0 | ((lba >> 24) & 0x0F);
        }

        if (!ata_send_command(channel, &cmd)) {
            spinlock_release(&g_ata_controller.lock);
            return total_read;
        }

        uint8_t status = ata_inb(io_base + 7);
        if (status & ATA_STATUS_ERR) {
            spinlock_release(&g_ata_controller.lock);
            return total_read;
        }

        for (uint32_t s = 0; s < sectors_this_op; s++) {
            if (!ata_wait_ready(channel)) {
                spinlock_release(&g_ata_controller.lock);
                return total_read;
            }

            status = ata_inb(io_base + 7);
            if (status & ATA_STATUS_ERR) {
                spinlock_release(&g_ata_controller.lock);
                return total_read;
            }

            ata_pio_read(channel, buf + total_read, 256);
            total_read += 512;
        }

        lba += sectors_this_op;
        count -= sectors_this_op;
    }

    spinlock_release(&g_ata_controller.lock);

    return total_read;
}

int ata_write_sectors(ata_device_t *device, uint64_t lba, uint32_t count, const void *buffer) {
    if (!device || !device->info.exists || device->info.type != ATA_DEV_TYPE_ATA) {
        return -1;
    }

    ata_channel_t channel = device->channel;
    uint16_t io_base = (channel == ATA_CHANNEL_PRIMARY) ? ATA_IO_BASE_PRIMARY : ATA_IO_BASE_SECONDARY;
    const uint8_t *buf = (const uint8_t *)buffer;
    (void)io_base;
    uint32_t total_written = 0;

    spinlock_acquire(&g_ata_controller.lock);

    ata_select_device(channel, device->select);

    while (count > 0) {
        uint32_t sectors_this_op = (count < 256) ? count : 256;
        ata_registers_t cmd;

        memset(&cmd, 0, sizeof(cmd));

        if (device->info.supports_lba48) {
            cmd.command = ATA_CMD_WRITE_PIO_EXT;
            cmd.sector_count = (sectors_this_op >> 0) & 0xFF;
            cmd.lba_low = (lba >> 0) & 0xFF;
            cmd.lba_mid = (lba >> 8) & 0xFF;
            cmd.lba_high = (lba >> 16) & 0xFF;
            cmd.device = 0x40;
        } else {
            cmd.command = ATA_CMD_WRITE_PIO;
            cmd.sector_count = (sectors_this_op >> 0) & 0xFF;
            cmd.lba_low = (lba >> 0) & 0xFF;
            cmd.lba_mid = (lba >> 8) & 0xFF;
            cmd.lba_high = (lba >> 16) & 0xFF;
            cmd.device = 0xE0 | ((lba >> 24) & 0x0F);
        }

        if (!ata_send_command(channel, &cmd)) {
            spinlock_release(&g_ata_controller.lock);
            return total_written;
        }

        for (uint32_t s = 0; s < sectors_this_op; s++) {
            if (!ata_wait_ready(channel)) {
                spinlock_release(&g_ata_controller.lock);
                return total_written;
            }

            ata_pio_write(channel, buf + total_written, 256);
            total_written += 512;
        }

        if (!ata_wait_not_busy(channel)) {
            spinlock_release(&g_ata_controller.lock);
            return total_written;
        }

        cmd.command = ATA_CMD_CACHE_FLUSH;
        if (device->info.supports_lba48) {
            cmd.command = ATA_CMD_CACHE_FLUSH_EXT;
        }

        ata_send_command(channel, &cmd);
        ata_wait_not_busy(channel);

        lba += sectors_this_op;
        count -= sectors_this_op;
    }

    spinlock_release(&g_ata_controller.lock);

    return total_written;
}

int ata_read_sectors_dma(ata_device_t *device, uint64_t lba, uint32_t count, void *buffer) {
    if (!device || !device->info.exists || !device->info.dma_capable) {
        return ata_read_sectors(device, lba, count, buffer);
    }

    return ata_read_sectors(device, lba, count, buffer);
}

int ata_write_sectors_dma(ata_device_t *device, uint64_t lba, uint32_t count, const void *buffer) {
    if (!device || !device->info.exists || !device->info.dma_capable) {
        return ata_write_sectors(device, lba, count, buffer);
    }

    return ata_write_sectors(device, lba, count, buffer);
}

bool ata_atapi_packet(ata_device_t *device, const uint8_t *packet, uint16_t packet_size, void *data, uint32_t *data_transferred) {
    if (!device || device->info.type != ATA_DEV_TYPE_ATAPI) {
        return false;
    }

    spinlock_acquire(&g_ata_controller.lock);

    bool result = atapi_send_packet(device->channel, packet, packet_size, data, data_transferred);

    spinlock_release(&g_ata_controller.lock);

    return result;
}

void ata_dump_devices(void) {
    debug_print("ATA: Device dump:\n");

    for (int c = 0; c < ATA_MAX_CHANNELS; c++) {
        for (int d = 0; d < ATA_MAX_DEVICES_PER_CHANNEL; d++) {
            ata_device_t *device = &g_ata_controller.devices[c][d];
            ata_device_info_t *info = &device->info;

            if (!info->exists) {
                continue;
            }

            debug_print("  Channel %d %s: type=%s, sectors=%llu, model=%s\n",
                       device->channel,
                       (device->select == ATA_DEV_MASTER) ? "master" : "slave",
                       (info->type == ATA_DEV_TYPE_ATA) ? "ATA" : "ATAPI",
                       (unsigned long long)info->sectors,
                       info->model);
        }
    }
}
