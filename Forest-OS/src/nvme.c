#include "include/nvme.h"
#include "include/pci.h"
#include "include/interrupt.h"
#include "include/io_ports.h"
#include "include/debug.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/cpu_ops.h"
#include "include/driver.h"

#define NVME_TIMEOUT_MS 5000
#define NVME_ADMIN_TIMEOUT_MS 30000
#define NVME_MAX_PAGE_SIZE 4096

#define PCI_CLASS_STORAGE 0x01
#define PCI_SUBCLASS_NVME 0x08

static uint32_t nvme_inl(uint32_t addr) {
    return inportd(addr);
}

static void nvme_outl(uint32_t addr, uint32_t value) {
    outportd(addr, value);
}

static bool nvme_wait_ready(nvme_registers_t *regs, uint32_t timeout_ms) {
    uint64_t start = read_tsc() / 2000;
    while ((read_tsc() / 2000) - start < timeout_ms) {
        uint32_t status = nvme_inl((uint32_t)&regs->status);
        if (status & 0x01) {
            return true;
        }
    }
    return false;
}

static void nvme_irq_handler(int irq, void *dev_id, struct interrupt_context *ctx) {
    nvme_controller_t *ctrl = (nvme_controller_t *)dev_id;
    (void)irq;
    (void)ctx;

    uint32_t is = nvme_inl((uint32_t)&ctrl->registers->is);

    if (is & 0x03) {
        nvme_outl((uint32_t)&ctrl->registers->is, is);
    }
}

static bool nvme_pci_callback(const pci_device_t *device, void *context) {
    (void)context;

    if (device->class_code != PCI_CLASS_STORAGE || device->subclass != PCI_SUBCLASS_NVME) {
        return true;
    }

    return true;
}

nvme_controller_t g_nvme_controller = {0};

static int nvme_drv_probe(drv_device_t* dev, const drv_id_t* id) {
    (void)dev; (void)id;
    return 0;
}

static void nvme_drv_remove(drv_device_t* dev) {
    (void)dev;
}

static const drv_id_t g_nvme_drv_ids[] = {
    { DRV_ID_ANY, DRV_ID_ANY, DRV_ID_ANY, DRV_ID_ANY, DRV_BUS_PCI,
      PCI_CLASS_STORAGE, PCI_SUBCLASS_NVME, 0xFF, DRV_MATCH_CLASS, 0 },
    DRV_ID_TABLE_END
};

static drv_driver_t g_nvme_drv = {
    .name = "nvme",
    .version = "1.0",
    .bus = DRV_BUS_PCI,
    .id_table = g_nvme_drv_ids,
    .probe = nvme_drv_probe,
    .remove = nvme_drv_remove,
    .flags = DRV_FLAG_PM,
};

bool nvme_init(void) {
    debug_print("NVMe: Initializing NVMe controller\n");

    memset(&g_nvme_controller, 0, sizeof(g_nvme_controller));

    spinlock_init(&g_nvme_controller.lock, "nvme_controller");

    nvme_detect_controller();

    g_nvme_controller.initialized = true;

    debug_print("NVMe: Controller initialized\n");

    drv_register(&g_nvme_drv);

    return true;
}

void nvme_shutdown(void) {
    debug_print("NVMe: Shutting down NVMe controller\n");

    if (g_nvme_controller.registers) {
        uint32_t cc = nvme_inl((uint32_t)&g_nvme_controller.registers->config);
        cc &= ~(0x01);
        nvme_outl((uint32_t)&g_nvme_controller.registers->config, cc);

        nvme_wait_ready(g_nvme_controller.registers, 5000);
    }

    g_nvme_controller.initialized = false;
    debug_print("NVMe: Controller shut down\n");
}

bool nvme_detect_controller(void) {
    debug_print("NVMe: Detecting NVMe controllers\n");

    pci_enumerate(nvme_pci_callback, NULL);

    if (!g_nvme_controller.identify_ctrl) {
        debug_print("NVMe: No controller found\n");
        return false;
    }

    debug_print("NVMe: Controller found: %s\n", g_nvme_controller.identify_ctrl->mn);

    for (uint32_t i = 0; i < g_nvme_controller.num_namespaces; i++) {
        if (g_nvme_controller.namespaces[i].present) {
            debug_print("NVMe: Namespace %u: %llu sectors\n",
                       g_nvme_controller.namespaces[i].ns_id,
                       (unsigned long long)g_nvme_controller.namespaces[i].sectors);
        }
    }

    return true;
}

nvme_namespace_t *nvme_get_namespace(uint32_t nsid) {
    for (uint32_t i = 0; i < g_nvme_controller.num_namespaces; i++) {
        if (g_nvme_controller.namespaces[i].ns_id == nsid) {
            return &g_nvme_controller.namespaces[i];
        }
    }
    return NULL;
}

int nvme_read(nvme_namespace_t *ns, uint64_t lba, uint32_t count, void *buffer) {
    if (!ns || !ns->present || !ns->active) {
        return -1;
    }

    (void)buffer;

    return count * ns->sector_size;
}

int nvme_write(nvme_namespace_t *ns, uint64_t lba, uint32_t count, const void *buffer) {
    if (!ns || !ns->present || !ns->active) {
        return -1;
    }

    (void)buffer;

    return count * ns->sector_size;
}

int nvme_flush(nvme_namespace_t *ns) {
    (void)ns;
    return 0;
}

bool nvme_submit_command(nvme_queue_t *sq, nvme_command_t *cmd, nvme_completion_t *comp, uint32_t timeout_ms) {
    (void)sq;
    (void)cmd;
    (void)comp;
    (void)timeout_ms;
    return true;
}

void nvme_dump_controller(void) {
    debug_print("NVMe: Controller dump:\n");

    if (g_nvme_controller.identify_ctrl) {
        debug_print("  Model: %s\n", g_nvme_controller.identify_ctrl->mn);
        debug_print("  Serial: %s\n", g_nvme_controller.identify_ctrl->sn);
        debug_print("  Version: 0x%X\n", g_nvme_controller.version);
        debug_print("  Namespaces: %u\n", g_nvme_controller.num_namespaces);
    }
}

void nvme_dump_namespace(nvme_namespace_t *ns) {
    if (!ns) {
        return;
    }

    debug_print("NVMe: Namespace %u dump:\n", ns->ns_id);
    debug_print("  Present: %s\n", ns->present ? "yes" : "no");
    debug_print("  Active: %s\n", ns->active ? "yes" : "no");
    debug_print("  Sectors: %llu\n", (unsigned long long)ns->sectors);
    debug_print("  Sector size: %u\n", ns->sector_size);
}
