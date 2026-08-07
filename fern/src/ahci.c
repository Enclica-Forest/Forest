#include "include/ahci.h"
#include "include/pci.h"
#include "include/interrupt.h"
#include "include/io_ports.h"
#include "include/debug.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/cpu_ops.h"
#include "include/driver.h"

#define PCI_CLASS_STORAGE 0x01
#define PCI_SUBCLASS_AHCI 0x06

#define AHCI_TIMEOUT_MS 5000
#define AHCI_FIS_SIZE 256
#define AHCI_CMD_TBL_SIZE 0x100
#define AHCI_CMD_LIST_SIZE (32 * sizeof(ahci_cmd_slot_t))
#define AHCI_PRDT_MAX_65535 65535

static uint8_t ahci_inb(uint32_t addr) {
    return inb(addr);
}

static void ahci_outb(uint32_t addr, uint8_t value) {
    outb(addr, value);
}

static uint32_t ahci_inl(uint32_t addr) {
    return inportd(addr);
}

static void ahci_outl(uint32_t addr, uint32_t value) {
    outportd(addr, value);
}

static bool ahci_wait_for(volatile uint32_t *reg, uint32_t mask, uint32_t expected, uint32_t timeout_ms) {
    uint64_t start = read_tsc() / 2000;
    while ((read_tsc() / 2000) - start < timeout_ms) {
        if ((ahci_inl((uint32_t)reg) & mask) == expected) {
            return true;
        }
    }
    return false;
}

static bool ahci_port_ready(ahci_port_t *port) {
    uint32_t status = ahci_inl((uint32_t)&port->regs->sstatus);
    return (status & AHCI_SSTS_DET) == AHCI_SSTS_DET_ACTIVE;
}

static bool ahci_start_cmd(ahci_port_t *port) {
    uint32_t cmd = ahci_inl((uint32_t)&port->regs->cmd);
    if (cmd & AHCI_CMD_ST) {
        return true;
    }

    cmd |= AHCI_CMD_ST | AHCI_CMD_FRE;
    ahci_outl((uint32_t)&port->regs->cmd, cmd);

    if (!ahci_wait_for(&port->regs->cmd, AHCI_CMD_CR | AHCI_CMD_FR, AHCI_CMD_CR | AHCI_CMD_FR, 100)) {
        return false;
    }

    return true;
}

static bool ahci_stop_cmd(ahci_port_t *port) {
    uint32_t cmd = ahci_inl((uint32_t)&port->regs->cmd);
    cmd &= ~(AHCI_CMD_ST | AHCI_CMD_FRE);
    ahci_outl((uint32_t)&port->regs->cmd, cmd);

    if (!ahci_wait_for(&port->regs->cmd, AHCI_CMD_CR | AHCI_CMD_FR, 0, 100)) {
        return false;
    }

    return true;
}

static void ahci_port_irq_handler(int irq, void *dev_id, struct interrupt_context *ctx) {
    ahci_port_t *port = (ahci_port_t *)dev_id;
    (void)irq;
    (void)ctx;

    uint32_t is = ahci_inl((uint32_t)&port->regs->is);

    if (is & AHCI_PxIS_TFES) {
    }

    if (is & AHCI_PxIS_DMPS) {
    }

    ahci_outl((uint32_t)&port->regs->is, is);
}

static bool ahci_identify_port(ahci_port_t *port) {
    ahci_port_info_t *info = &port->info;
    uint8_t *identify;
    uint32_t sstatus;

    memset(info, 0, sizeof(ahci_port_info_t));

    sstatus = ahci_inl((uint32_t)&port->regs->sstatus);

    if ((sstatus & AHCI_SSTS_DET) != AHCI_SSTS_DET_ACTIVE) {
        return false;
    }

    info->present = true;

    uint32_t sig_lo = ahci_inl((uint32_t)&port->regs->signature);
    uint32_t sig_hi = ahci_inl((uint32_t)&port->regs->signature);

    (void)sig_hi;

    if ((sig_lo & 0xFF) == 0x01 && ((sig_lo >> 8) & 0xFF) == 0xEB) {
        info->device_type = AHCI_DEV_TYPE_SATAPI;
        info->atapi = true;
    } else if ((sig_lo & 0xFF) == 0x14 && ((sig_lo >> 8) & 0xFF) == 0xEB) {
        info->device_type = AHCI_DEV_TYPE_SEMB;
    } else if ((sig_lo & 0xFF) == 0x01 && ((sig_lo >> 8) & 0xFF) == 0x00) {
        info->device_type = AHCI_DEV_TYPE_PM;
    } else {
        info->device_type = AHCI_DEV_TYPE_SATA;
    }

    if (info->device_type != AHCI_DEV_TYPE_SATA) {
        return true;
    }

    return true;
}

static void ahci_setup_cmd_slot(ahci_port_t *port, uint8_t slot, uint64_t lba, uint32_t count, bool is_write, bool use_ncq) {
    ahci_cmd_slot_t *cmd_slot = &port->cmd_list[slot];
    ahci_cmd_table_t *cmd_table = (ahci_cmd_table_t *)((uint8_t *)port->cmd_tables + slot * AHCI_CMD_TBL_SIZE);
    ahci_fis_reg_h2d_t *fis = &cmd_table->command_fis;
    ahci_sg_entry_t *prdt = (ahci_sg_entry_t *)cmd_table->reserved;

    uint32_t opts = (count - 1) & 0x1F;

    if (use_ncq) {
        opts |= (1U << 7);
    }

    if (count > 1) {
        opts |= (1U << 6);
    }

    if (is_write) {
        opts |= (1U << 5);
    }

    opts |= (1U << 0);

    memset(cmd_slot, 0, sizeof(ahci_cmd_slot_t));
    cmd_slot->opts = opts;
    cmd_slot->cmd_table_base = (uint32_t)((uint64_t)cmd_table & 0xFFFFFFFF);
    cmd_slot->cmd_table_base_upper = (uint32_t)((uint64_t)cmd_table >> 32);

    memset(fis, 0, sizeof(ahci_fis_reg_h2d_t));
    fis->fis_type = ATA_FIS_TYPE_REG_H2D;
    fis->command = is_write ? 0x35 : 0x25;
    fis->lba_low = (lba >> 0) & 0xFF;
    fis->lba_mid = (lba >> 8) & 0xFF;
    fis->lba_high = (lba >> 16) & 0xFF;
    fis->lba_exp = (lba >> 24) & 0xFF;
    fis->sector_count_low = (count >> 0) & 0xFF;
    fis->sector_count_exp = (count >> 8) & 0xFF;
    fis->device = 0x40;
}

static int ahci_read_write_port(ahci_port_t *port, uint64_t lba, uint32_t count, void *buffer, bool is_write) {
    if (!port || !port->initialized || !port->info.present || port->info.device_type != AHCI_DEV_TYPE_SATA) {
        return -1;
    }

    spinlock_acquire(&port->lock);

    uint32_t completed;
    uint32_t sactive = ahci_inl((uint32_t)&port->regs->sactive);
    uint8_t slot = 0;

    for (uint8_t i = 0; i < 32; i++) {
        if (!(sactive & (1U << i))) {
            slot = i;
            break;
        }
    }

    ahci_setup_cmd_slot(port, slot, lba, count, is_write, false);

    ahci_outl((uint32_t)&port->regs->ci, (1U << slot));

    uint64_t start = read_tsc() / 2000;
    while ((read_tsc() / 2000) - start < AHCI_TIMEOUT_MS) {
        completed = ahci_inl((uint32_t)&port->regs->ci);
        if (!(completed & (1U << slot))) {
            break;
        }
    }

    spinlock_release(&port->lock);

    if (completed & (1U << slot)) {
        return -1;
    }

    return count * AHCI_SECTOR_SIZE;
}

static bool ahci_pci_callback(const pci_device_t *device, void *context) {
    (void)context;

    if (device->class_code != PCI_CLASS_STORAGE || device->subclass != PCI_SUBCLASS_AHCI) {
        return true;
    }

    if ((device->prog_if & 0x01) == 0 && (device->prog_if & 0x02) == 0) {
        return true;
    }

    return true;
}

ahci_controller_t g_ahci_controller = {0};

static int ahci_drv_probe(drv_device_t* dev, const drv_id_t* id) {
    (void)dev; (void)id;
    return 0;
}

static void ahci_drv_remove(drv_device_t* dev) {
    (void)dev;
}

static const drv_id_t g_ahci_drv_ids[] = {
    { DRV_ID_ANY, DRV_ID_ANY, DRV_ID_ANY, DRV_ID_ANY, DRV_BUS_PCI,
      PCI_CLASS_STORAGE, PCI_SUBCLASS_AHCI, 0xFF, DRV_MATCH_CLASS, 0 },
    DRV_ID_TABLE_END
};

static drv_driver_t g_ahci_drv = {
    .name = "ahci",
    .version = "1.0",
    .bus = DRV_BUS_PCI,
    .id_table = g_ahci_drv_ids,
    .probe = ahci_drv_probe,
    .remove = ahci_drv_remove,
    .flags = DRV_FLAG_PM,
};

bool ahci_init(void) {
    debug_print("AHCI: Initializing AHCI controller\n");

    memset(&g_ahci_controller, 0, sizeof(g_ahci_controller));

    spinlock_init(&g_ahci_controller.lock, "ahci_controller");

    ahci_detect_controller();

    g_ahci_controller.initialized = true;

    debug_print("AHCI: Controller initialized\n");

    drv_register(&g_ahci_drv);

    return true;
}

void ahci_shutdown(void) {
    debug_print("AHCI: Shutting down AHCI controller\n");

    for (uint32_t i = 0; i < g_ahci_controller.port_count; i++) {
        if (g_ahci_controller.ports[i].initialized) {
            ahci_port_stop(&g_ahci_controller.ports[i]);
        }
    }

    g_ahci_controller.initialized = false;
    debug_print("AHCI: Controller shut down\n");
}

bool ahci_detect_controller(void) {
    debug_print("AHCI: Detecting AHCI controllers\n");

    pci_enumerate(ahci_pci_callback, NULL);

    for (uint32_t i = 0; i < g_ahci_controller.port_count; i++) {
        ahci_port_t *port = &g_ahci_controller.ports[i];

        spinlock_init(&port->lock, "ahci_port");

        if (!ahci_identify_port(port)) {
            continue;
        }

        debug_print("AHCI: Port %d: type=%d, present=%s\n",
                   port->port_number,
                   port->info.device_type,
                   port->info.present ? "yes" : "no");
    }

    return g_ahci_controller.port_count > 0;
}

bool ahci_detect_ports(void) {
    debug_print("AHCI: Detecting ports on controller\n");

    uint32_t pi = g_ahci_controller.ports_implemented;
    uint32_t port_num = 0;

    for (uint32_t i = 0; i < 32; i++) {
        if (pi & (1U << i)) {
            ahci_port_t *port = &g_ahci_controller.ports[port_num];

            port->port_number = i;
            spinlock_init(&port->lock, "ahci_port");

            if (ahci_identify_port(port)) {
                debug_print("AHCI: Port %d present: type=%d\n", i, port->info.device_type);
            }

            port_num++;
        }
    }

    g_ahci_controller.port_count = port_num;

    return port_num > 0;
}

ahci_port_t *ahci_get_port(uint8_t port_number) {
    for (uint32_t i = 0; i < g_ahci_controller.port_count; i++) {
        if (g_ahci_controller.ports[i].port_number == port_number) {
            return &g_ahci_controller.ports[i];
        }
    }
    return NULL;
}

int ahci_read_sectors(ahci_port_t *port, uint64_t lba, uint32_t count, void *buffer) {
    return ahci_read_write_port(port, lba, count, buffer, false);
}

int ahci_write_sectors(ahci_port_t *port, uint64_t lba, uint32_t count, const void *buffer) {
    return ahci_read_write_port(port, lba, count, (void *)buffer, true);
}

bool ahci_port_start(ahci_port_t *port) {
    if (!port) {
        return false;
    }

    if (port->initialized) {
        return true;
    }

    if (!ahci_start_cmd(port)) {
        debug_print("AHCI: Failed to start port %d\n", port->port_number);
        return false;
    }

    port->initialized = true;

    return true;
}

bool ahci_port_stop(ahci_port_t *port) {
    if (!port || !port->initialized) {
        return true;
    }

    ahci_stop_cmd(port);

    port->initialized = false;

    return true;
}

void ahci_dump_controller(void) {
    debug_print("AHCI: Controller dump:\n");
    debug_print("  Ports implemented: 0x%X\n", g_ahci_controller.ports_implemented);
    debug_print("  Port count: %u\n", g_ahci_controller.port_count);
    debug_print("  Version: 0x%X\n", g_ahci_controller.caps);
}

void ahci_dump_port(ahci_port_t *port) {
    if (!port) {
        return;
    }

    debug_print("AHCI: Port %d dump:\n", port->port_number);
    debug_print("  Present: %s\n", port->info.present ? "yes" : "no");
    debug_print("  Device type: %d\n", port->info.device_type);
    debug_print("  ATAPI: %s\n", port->info.atapi ? "yes" : "no");
}
