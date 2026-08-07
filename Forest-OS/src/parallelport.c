#include "include/parallelport.h"
#include "include/system.h"
#include "include/driver.h"
#include "include/memory.h"
#include "include/mm.h"
#include "include/string.h"
#include "include/screen.h"

#define GFP_KERNEL 0x01

static parallelport_t* g_lpt_ports[LPT_MAX_PORTS];
static uint16 g_lpt_io_bases[LPT_MAX_PORTS] = {
    LPT1_IO_BASE,
    LPT2_IO_BASE,
    LPT3_IO_BASE
};
static uint32 g_lpt_count = 0;

static uint8 lpt_read_register(parallelport_t* lpt, uint8 offset) {
    return inportb(lpt->io_base + offset);
}

static void lpt_write_register(parallelport_t* lpt, uint8 offset, uint8 data) {
    outportb(lpt->io_base + offset, data);
}

bool lpt_detect_port(uint16 io_base) {
    outportb(io_base + LPT_CONTROL_REG, 0x00);
    timer_sleep_ms(1);
    
    uint8 control = inportb(io_base + LPT_CONTROL_REG);
    outportb(io_base + LPT_CONTROL_REG, control | 0x04);
    timer_sleep_ms(1);
    
    uint8 status = inportb(io_base + LPT_STATUS_REG);
    
    if ((status & 0xB8) == 0xB8) {
        return true;
    }

    return false;
}

bool lpt_write_data(parallelport_t* lpt, uint8 data) {
    if (!lpt) {
        return false;
    }

    uint8 status = lpt_read_register(lpt, LPT_STATUS_REG);
    while (!(status & LPT_STATUS_BUSY)) {
        timer_sleep_ms(1);
        status = lpt_read_register(lpt, LPT_STATUS_REG);
    }

    lpt_write_register(lpt, LPT_DATA_REG, data);
    lpt->registers.data = data;

    return true;
}

bool lpt_read_status(parallelport_t* lpt, uint8* status) {
    if (!lpt || !status) {
        return false;
    }

    *status = lpt_read_register(lpt, LPT_STATUS_REG);
    lpt->registers.status = *status;

    return true;
}

bool lpt_write_control(parallelport_t* lpt, uint8 control) {
    if (!lpt) {
        return false;
    }

    lpt_write_register(lpt, LPT_CONTROL_REG, control);
    lpt->registers.control = control;

    return true;
}

bool lpt_send_byte(parallelport_t* lpt, uint8 data) {
    if (!lpt) {
        return false;
    }

    if (!lpt_write_data(lpt, data)) {
        return false;
    }

    uint8 control = lpt_read_register(lpt, LPT_CONTROL_REG);
    control |= LPT_CONTROL_STROBE;
    lpt_write_register(lpt, LPT_CONTROL_REG, control);
    timer_sleep_ms(1);

    control &= ~LPT_CONTROL_STROBE;
    lpt_write_register(lpt, LPT_CONTROL_REG, control);
    timer_sleep_ms(1);

    uint8 status;
    while (1) {
        if (lpt_read_status(lpt, &status)) {
            if (status & LPT_STATUS_BUSY) {
                break;
            }
        }
        timer_sleep_ms(1);
    }

    return true;
}

bool lpt_print_string(parallelport_t* lpt, const char* str) {
    if (!lpt || !str) {
        return false;
    }

    while (*str) {
        if (*str == '\n') {
            if (!lpt_send_byte(lpt, '\r')) {
                return false;
            }
            timer_sleep_ms(1);
        }

        if (!lpt_send_byte(lpt, *str)) {
            return false;
        }

        timer_sleep_ms(1);
        str++;
    }

    return true;
}

parallelport_t* lpt_allocate_port(uint8 port_num) {
    if (port_num >= LPT_MAX_PORTS || g_lpt_ports[port_num]) {
        return 0;
    }

    parallelport_t* lpt = (parallelport_t*)kmalloc(sizeof(parallelport_t));
    if (!lpt) {
        return 0;
    }

    memory_set((uint8*)lpt, 0, sizeof(parallelport_t));

    lpt->io_base = g_lpt_io_bases[port_num];
    lpt->port_num = port_num;
    lpt->mode = LPT_MODE_SPP;

    lpt->write_data = lpt_write_data;
    lpt->read_status = lpt_read_status;
    lpt->write_control = lpt_write_control;
    lpt->send_byte = lpt_send_byte;

    lpt_write_register(lpt, LPT_CONTROL_REG, 0x00);
    timer_sleep_ms(1);

    lpt->initialized = true;
    g_lpt_ports[port_num] = lpt;
    g_lpt_count++;

    return lpt;
}

void lpt_free_port(parallelport_t* lpt) {
    if (!lpt) {
        return;
    }

    if (lpt->port_num < LPT_MAX_PORTS) {
        g_lpt_ports[lpt->port_num] = 0;
        g_lpt_count--;
    }

    kfree(lpt);
}

bool lpt_init(void) {
    print("[LPT] Initializing Parallel Port driver...\n");

    memory_set((uint8*)g_lpt_ports, 0, sizeof(g_lpt_ports));
    g_lpt_count = 0;

    for (uint8 i = 0; i < LPT_MAX_PORTS; i++) {
        if (lpt_detect_port(g_lpt_io_bases[i])) {
            print("[LPT] Parallel port ");
            print_dec(i + 1);
            print(" detected at 0x");
            print_hex(g_lpt_io_bases[i]);
            print("\n");

            parallelport_t* lpt = lpt_allocate_port(i);
            if (lpt) {
                print("[LPT] Port ");
                print_dec(i + 1);
                print(" initialized\n");
            }
        }
    }

    print("[LPT] Parallel Port driver initialized\n");
    return g_lpt_count > 0;
}

void lpt_shutdown(void) {
    print("[LPT] Shutting down Parallel Port driver...\n");

    for (uint8 i = 0; i < LPT_MAX_PORTS; i++) {
        if (g_lpt_ports[i]) {
            lpt_free_port(g_lpt_ports[i]);
        }
    }

    g_lpt_count = 0;
}
