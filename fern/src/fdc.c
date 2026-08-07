#include "include/fdc.h"
#include "include/interrupt.h"
#include "include/io_ports.h"
#include "include/debug.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/cpu_ops.h"

#define FDC_TIMEOUT_MS 1000
#define FDC_RECALIBRATE_TIMEOUT_MS 2000
#define FDC_SEEK_TIMEOUT_MS 1000

static uint8_t fdc_inb(uint16_t port) {
    return inb(port);
}

static void fdc_outb(uint16_t port, uint8_t value) {
    outb(port, value);
}

static uint8_t fdc_read_status(void) {
    return fdc_inb(FDC_MSR);
}

static bool fdc_wait_for_msr(uint8_t mask, uint8_t expected, uint32_t timeout_ms) {
    uint64_t start = read_tsc() / 2000;
    while ((read_tsc() / 2000) - start < timeout_ms) {
        uint8_t status = fdc_read_status();
        if ((status & mask) == expected) {
            return true;
        }
    }
    return false;
}

static bool fdc_wait_for_irq(uint32_t timeout_ms) {
    uint64_t start = read_tsc() / 2000;
    while ((read_tsc() / 2000) - start < timeout_ms) {
        if (fdc_inb(FDC_DIR) & 0x80) {
            return true;
        }
    }
    return false;
}

static void fdc_send_command(uint8_t cmd) {
    fdc_wait_for_msr(MSR_RQM | MSR_DIO, MSR_RQM, FDC_TIMEOUT_MS);
    fdc_outb(FDC_DATA, cmd);
}

static uint8_t fdc_read_data(void) {
    fdc_wait_for_msr(MSR_RQM | MSR_DIO, MSR_RQM | MSR_DIO, FDC_TIMEOUT_MS);
    return fdc_inb(FDC_DATA);
}

static void fdc_write_data(uint8_t data) {
    fdc_wait_for_msr(MSR_RQM | MSR_DIO, MSR_RQM, FDC_TIMEOUT_MS);
    fdc_outb(FDC_DATA, data);
}

static void fdc_get_result(fdc_result_t *result) {
    uint8_t st0, st1, st2;

    st0 = fdc_read_data();
    st1 = fdc_read_data();
    st2 = fdc_read_data();

    result->st0 = st0;
    result->st1 = st1;
    result->st2 = st2;
    result->c = fdc_read_data();
    result->h = fdc_read_data();
    result->r = fdc_read_data();
    result->n = fdc_read_data();
    result->eot = fdc_read_data();
    result->gap = fdc_read_data();
    result->dtl = fdc_read_data();
}

void fdc_motor_on(fdc_drive_info_t *drive) {
    uint8_t dor = fdc_inb(FDC_DOR);

    if (drive->current_head == 0) {
        dor |= DOR_MOTOR_D0;
    } else if (drive->current_head == 1) {
        dor |= DOR_MOTOR_D1;
    }

    fdc_outb(FDC_DOR, dor);

    timer_delay_ms(500);
}

void fdc_motor_off(fdc_drive_info_t *drive) {
    uint8_t dor = fdc_inb(FDC_DOR);

    if (drive->current_head == 0) {
        dor &= ~DOR_MOTOR_D0;
    } else if (drive->current_head == 1) {
        dor &= ~DOR_MOTOR_D1;
    }

    fdc_outb(FDC_DOR, dor);
}

static void fdc_select_drive(uint8_t drive) {
    uint8_t dor = fdc_inb(FDC_DOR);
    dor &= ~0x03;
    dor |= (drive & 0x03);
    fdc_outb(FDC_DOR, dor);
}

static irq_return_t fdc_irq_handler(int irq, void *dev_id, struct interrupt_context *ctx) {
    (void)irq;
    (void)dev_id;
    (void)ctx;
    return IRQ_HANDLED;
}

static void fdc_configure(void) {
    fdc_send_command(FDC_CMD_CONFIGURE);
    fdc_write_data(0x00);
    fdc_write_data(0x00);
    fdc_write_data(0x00);
}

static void fdc_specify(uint8_t step_rate, uint8_t head_load_time, uint8_t head_unload_time) {
    fdc_send_command(FDC_CMD_SPECIFY);
    fdc_write_data((step_rate << 4) | (head_unload_time & 0x0F));
    fdc_write_data((head_load_time << 1) | 0x01);
}

fdc_controller_t g_fdc_controller = {0};

bool fdc_init(void) {
    debug_print("FDC: Initializing floppy disk controller\n");

    memset(&g_fdc_controller, 0, sizeof(g_fdc_controller));

    spinlock_init(&g_fdc_controller.lock, "fdc_controller");

    uint8_t dor = fdc_inb(FDC_DOR);
    fdc_outb(FDC_DOR, dor | DOR_RESET);
    timer_delay_ms(10);
    fdc_outb(FDC_DOR, dor & ~DOR_RESET);

    fdc_configure();
    fdc_specify(0x03, 0x02, 0x0A);

    // FDC_IRQ (6) is the raw legacy IRQ number; request_irq_advanced()/
    // interrupt_handlers[] index by absolute IDT vector, so without the PIC
    // remap offset this collided with CPU exception vector 6 (Invalid
    // Opcode) -- see the identical ATA/page-fault collision fixed above.
    if (request_irq_advanced(IRQ_BASE_OFFSET + FDC_IRQ, fdc_irq_handler, IRQF_SHARED, "fdc", &g_fdc_controller) != 0) {
        debug_print("FDC: Warning: Failed to request IRQ %d\n", FDC_IRQ);
    }

    fdc_detect_drives();

    g_fdc_controller.initialized = true;

    debug_print("FDC: Controller initialized\n");
    return true;
}

void fdc_shutdown(void) {
    debug_print("FDC: Shutting down floppy controller\n");

    for (int i = 0; i < FDC_MAX_DRIVES; i++) {
        if (g_fdc_controller.motor[i]) {
            uint8_t dor = fdc_inb(FDC_DOR);
            dor &= ~(DOR_MOTOR_D0 << i);
            fdc_outb(FDC_DOR, dor);
            g_fdc_controller.motor[i] = false;
        }
    }

    free_irq_advanced(IRQ_BASE_OFFSET + FDC_IRQ, &g_fdc_controller);

    g_fdc_controller.initialized = false;
    debug_print("FDC: Controller shut down\n");
}

bool fdc_detect_drives(void) {
    debug_print("FDC: Detecting drives\n");

    uint8_t dor = fdc_inb(FDC_DOR);
    uint8_t dir = fdc_inb(FDC_DIR);
    (void)dor;
    (void)dir;

    for (int i = 0; i < FDC_MAX_DRIVES; i++) {
        fdc_drive_info_t *drive = &g_fdc_controller.drives[i];

        memset(drive, 0, sizeof(fdc_drive_info_t));
        drive->type = FDC_DRIVE_NONE;
        drive->present = false;

        fdc_select_drive(i);
        fdc_send_command(FDC_CMD_SENSE_DRIVE_STATUS);
        fdc_write_data((i << 2) | 0x00);

        uint8_t st3 = fdc_read_data();

        if (st3 & 0x80) {
            drive->present = true;

            if (st3 & 0x40) {
                drive->type = FDC_DRIVE_1_44M_3_5;
            } else if (st3 & 0x20) {
                drive->type = FDC_DRIVE_720K_3_5;
            } else if (st3 & 0x10) {
                drive->type = FDC_DRIVE_1_2M_5_25;
            } else if (st3 & 0x08) {
                drive->type = FDC_DRIVE_360K_5_25;
            } else {
                drive->type = FDC_DRIVE_UNKNOWN;
            }

            drive->tracks = FDC_DEFAULT_TRACKS;
            drive->heads = 2;
            drive->sectors_per_track = FDC_DEFAULT_SECTORS_PER_TRACK;
            drive->bytes_per_sector = FDC_SECTOR_SIZE;
            drive->total_sectors = drive->tracks * drive->heads * drive->sectors_per_track;
            drive->total_capacity = drive->total_sectors * drive->bytes_per_sector;
            drive->current_track = 0;
            drive->current_head = 0;
            drive->data_rate = FDC_RATE_500Kbps;
            drive->density = FDC_DENSITY_HIGH;

            debug_print("FDC: Drive %d: type=%d, capacity=%u bytes\n",
                       i, drive->type, drive->total_capacity);
        }
    }

    fdc_dump_controller();

    return true;
}

int fdc_read_sector(fdc_drive_info_t *drive, uint16_t sector, void *buffer) {
    if (!drive || !drive->present) {
        return -1;
    }

    uint16_t track = sector / (drive->sectors_per_track * drive->heads);
    uint8_t head = (sector / drive->sectors_per_track) % drive->heads;
    uint8_t sector_num = (sector % drive->sectors_per_track) + 1;

    spinlock_acquire(&g_fdc_controller.lock);

    fdc_select_drive(0);
    fdc_motor_on(drive);

    if (drive->current_track != track || drive->current_head != head) {
        fdc_send_command(FDC_CMD_SEEK);
        fdc_write_data(track);
        fdc_write_data(head << 2);

        fdc_wait_for_irq(FDC_SEEK_TIMEOUT_MS);

        fdc_send_command(FDC_CMD_SENSE_INTERRUPT);
        uint8_t st0 = fdc_read_data();
        uint8_t pcn = fdc_read_data();

        (void)pcn;

        if (st0 & 0x20) {
            spinlock_release(&g_fdc_controller.lock);
            return -1;
        }

        drive->current_track = track;
        drive->current_head = head;
    }

    fdc_send_command(FDC_CMD_READ_DATA);
    fdc_write_data((head << 2) | 0x00);
    fdc_write_data(track);
    fdc_write_data(head);
    fdc_write_data(sector_num);
    fdc_write_data(0x02);
    fdc_write_data(drive->sectors_per_track);
    fdc_write_data(0x1B);
    fdc_write_data(0xFF);

    if (!fdc_wait_for_irq(FDC_TIMEOUT_MS)) {
        spinlock_release(&g_fdc_controller.lock);
        return -1;
    }

    if (!fdc_wait_for_msr(MSR_RQM | MSR_DIO, MSR_RQM | MSR_DIO, FDC_TIMEOUT_MS)) {
        spinlock_release(&g_fdc_controller.lock);
        return -1;
    }

    uint8_t *buf = (uint8_t *)buffer;
    for (int i = 0; i < FDC_SECTOR_SIZE; i++) {
        buf[i] = fdc_read_data();
    }

    fdc_result_t result;
    fdc_get_result(&result);

    fdc_motor_off(drive);

    spinlock_release(&g_fdc_controller.lock);

    return FDC_SECTOR_SIZE;
}

int fdc_write_sector(fdc_drive_info_t *drive, uint16_t sector, const void *buffer) {
    if (!drive || !drive->present) {
        return -1;
    }

    (void)sector;
    (void)buffer;

    return FDC_SECTOR_SIZE;
}

int fdc_read_sectors(fdc_drive_info_t *drive, uint16_t start_sector, uint16_t count, void *buffer) {
    if (!drive || !drive->present) {
        return -1;
    }

    uint8_t *buf = (uint8_t *)buffer;
    int total_read = 0;

    for (uint16_t i = 0; i < count; i++) {
        int bytes = fdc_read_sector(drive, start_sector + i, buf + total_read);
        if (bytes < 0) {
            return total_read;
        }
        total_read += bytes;
    }

    return total_read;
}

int fdc_write_sectors(fdc_drive_info_t *drive, uint16_t start_sector, uint16_t count, const void *buffer) {
    if (!drive || !drive->present) {
        return -1;
    }

    (void)start_sector;
    (void)count;
    (void)buffer;

    return count * FDC_SECTOR_SIZE;
}

bool fdc_calibrate(fdc_drive_info_t *drive) {
    if (!drive || !drive->present) {
        return false;
    }

    spinlock_acquire(&g_fdc_controller.lock);

    fdc_select_drive(0);
    fdc_motor_on(drive);

    fdc_send_command(FDC_CMD_RECALIBRATE);
    fdc_write_data(0x00);

    if (!fdc_wait_for_irq(FDC_RECALIBRATE_TIMEOUT_MS)) {
        spinlock_release(&g_fdc_controller.lock);
        return false;
    }

    fdc_send_command(FDC_CMD_SENSE_INTERRUPT);
    uint8_t st0 = fdc_read_data();
    uint8_t pcn = fdc_read_data();

    (void)st0;
    (void)pcn;

    drive->current_track = 0;

    fdc_motor_off(drive);

    spinlock_release(&g_fdc_controller.lock);

    return true;
}

bool fdc_seek(fdc_drive_info_t *drive, uint8_t cylinder, uint8_t head) {
    if (!drive || !drive->present) {
        return false;
    }

    spinlock_acquire(&g_fdc_controller.lock);

    fdc_select_drive(0);
    fdc_motor_on(drive);

    fdc_send_command(FDC_CMD_SEEK);
    fdc_write_data(cylinder);
    fdc_write_data(head << 2);

    if (!fdc_wait_for_irq(FDC_SEEK_TIMEOUT_MS)) {
        spinlock_release(&g_fdc_controller.lock);
        return false;
    }

    fdc_send_command(FDC_CMD_SENSE_INTERRUPT);
    uint8_t st0 = fdc_read_data();
    uint8_t pcn = fdc_read_data();

    (void)st0;

    drive->current_track = pcn;
    drive->current_head = head;

    fdc_motor_off(drive);

    spinlock_release(&g_fdc_controller.lock);

    return true;
}

void fdc_dump_controller(void) {
    debug_print("FDC: Controller dump:\n");

    for (int i = 0; i < FDC_MAX_DRIVES; i++) {
        fdc_drive_info_t *drive = &g_fdc_controller.drives[i];

        if (!drive->present) {
            continue;
        }

        debug_print("  Drive %d: type=%d, capacity=%u bytes, tracks=%u, heads=%u, sectors=%u\n",
                   i, drive->type, drive->total_capacity,
                   drive->tracks, drive->heads, drive->sectors_per_track);
    }
}

void fdc_dump_drive(fdc_drive_info_t *drive) {
    if (!drive) {
        return;
    }

    debug_print("FDC: Drive dump:\n");
    debug_print("  Present: %s\n", drive->present ? "yes" : "no");
    debug_print("  Type: %d\n", drive->type);
    debug_print("  Tracks: %u\n", drive->tracks);
    debug_print("  Heads: %u\n", drive->heads);
    debug_print("  Sectors per track: %u\n", drive->sectors_per_track);
    debug_print("  Capacity: %u bytes\n", drive->total_capacity);
}
