#include "include/gameport.h"
#include "include/system.h"
#include "include/driver.h"
#include "include/memory.h"
#include "include/mm.h"
#include "include/string.h"
#include "include/screen.h"

#define GFP_KERNEL 0x01

static gameport_t* g_gameports[4];
static uint32 g_gameport_count = 0;

static uint8 gameport_read_status(void) {
    return inportb(GAMEPORT_IO_PORT);
}

static void gameport_write_trigger(void) {
    outportb(GAMEPORT_IO_PORT, 0xFF);
}

bool gameport_read_button(gameport_t* gp, uint8 button_mask) {
    (void)gp;
    
    if (button_mask == 0) {
        return false;
    }

    uint8 status = gameport_read_status();
    return (status & button_mask) == 0;
}

uint16_t gameport_read_axis(gameport_t* gp, uint8 axis_mask) {
    (void)gp;
    
    uint32 timeout = 0;
    
    gameport_write_trigger();
    
    while (1) {
        uint8 status = gameport_read_status();
        if (status & axis_mask) {
            return (uint16_t)timeout;
        } else if (timeout >= GAMEPORT_TIMEOUT) {
            return 0;
        }
        timeout++;
    }
}

bool gameport_poll_joystick(gameport_t* gp, joystick_status_t* status) {
    if (!gp || !status) {
        return false;
    }

    status->button_a = gameport_read_button(gp, JOYSTICK_BUTTON_A);
    status->button_b = gameport_read_button(gp, JOYSTICK_BUTTON_B);
    status->button_c = gameport_read_button(gp, JOYSTICK_BUTTON_C);
    status->button_d = gameport_read_button(gp, JOYSTICK_BUTTON_D);

    status->axis_x = (int16_t)gameport_read_axis(gp, JOYSTICK_AXIS_X);
    status->axis_y = (int16_t)gameport_read_axis(gp, JOYSTICK_AXIS_Y);
    status->delta_x = (int16_t)gameport_read_axis(gp, JOYSTICK_AXIS_DELTA_X);
    status->delta_y = (int16_t)gameport_read_axis(gp, JOYSTICK_AXIS_DELTA_Y);

    status->present = (status->axis_x > 0 || status->axis_y > 0);

    return true;
}

gameport_t* gameport_allocate_device(void) {
    if (g_gameport_count >= 4) {
        return 0;
    }

    gameport_t* gp = (gameport_t*)kmalloc(sizeof(gameport_t));
    if (!gp) {
        return 0;
    }

    memory_set((uint8*)gp, 0, sizeof(gameport_t));

    gp->io_port = GAMEPORT_IO_PORT;
    gp->read_button = gameport_read_button;
    gp->poll = gameport_poll_joystick;

    g_gameports[g_gameport_count++] = gp;

    return gp;
}

void gameport_free_device(gameport_t* gp) {
    if (!gp) {
        return;
    }

    for (uint32 i = 0; i < g_gameport_count; i++) {
        if (g_gameports[i] == gp) {
            g_gameports[i] = 0;
            break;
        }
    }

    kfree(gp);
}

static bool gameport_detect_device(void) {
    gameport_write_trigger();
    
    timer_sleep_ms(1);
    
    uint8 status = gameport_read_status();
    
    if ((status & 0x0F) == 0x0F) {
        return false;
    }

    return true;
}

bool gameport_init(void) {
    print("[GAMEPORT] Initializing Game Port driver...\n");

    memory_set((uint8*)g_gameports, 0, sizeof(g_gameports));
    g_gameport_count = 0;

    if (gameport_detect_device()) {
        print("[GAMEPORT] Game port device detected\n");
        gameport_t* gp = gameport_allocate_device();
        if (gp) {
            gp->initialized = true;

            joystick_status_t status;
            if (gameport_poll_joystick(gp, &status)) {
                print("[GAMEPORT] Joystick status: X=");
                print_dec(status.axis_x);
                print(" Y=");
                print_dec(status.axis_y);
                print("\n");
            }
        }
    } else {
        print("[GAMEPORT] No game port device detected\n");
    }

    print("[GAMEPORT] Game Port driver initialized\n");
    return true;
}

void gameport_shutdown(void) {
    print("[GAMEPORT] Shutting down Game Port driver...\n");

    for (uint32 i = 0; i < g_gameport_count; i++) {
        if (g_gameports[i]) {
            gameport_free_device(g_gameports[i]);
        }
    }

    g_gameport_count = 0;
}
