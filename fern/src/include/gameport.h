#ifndef GAMEPORT_H
#define GAMEPORT_H

#include "types.h"
#include <stdbool.h>
#include <stdint.h>

#define GAMEPORT_IO_PORT 0x201
#define GAMEPORT_MAX_TIME_ATTEMPTS 1000
#define GAMEPORT_TIMEOUT 1000

typedef enum {
    JOYSTICK_BUTTON_A = 0x10,
    JOYSTICK_BUTTON_B = 0x20,
    JOYSTICK_BUTTON_C = 0x40,
    JOYSTICK_BUTTON_D = 0x80,
    JOYSTICK_AXIS_X = 0x01,
    JOYSTICK_AXIS_Y = 0x02,
    JOYSTICK_AXIS_DELTA_X = 0x04,
    JOYSTICK_AXIS_DELTA_Y = 0x08
} joystick_button_t;

typedef struct {
    int16_t axis_x;
    int16_t axis_y;
    int16_t delta_x;
    int16_t delta_y;
    bool button_a;
    bool button_b;
    bool button_c;
    bool button_d;
    bool present;
} joystick_status_t;

typedef struct gameport gameport_t;
typedef bool (*gameport_read_func)(gameport_t* gp, uint8 button_mask);
typedef bool (*gameport_poll_func)(gameport_t* gp, joystick_status_t* status);

struct gameport {
    uint16 io_port;
    uint16 vendor_id;
    uint16 device_id;
    void* private_data;
    
    gameport_read_func read_button;
    gameport_poll_func poll;
    
    bool initialized;
};

bool gameport_init(void);
void gameport_shutdown(void);
gameport_t* gameport_allocate_device(void);
void gameport_free_device(gameport_t* gp);

bool gameport_read_button(gameport_t* gp, uint8 button_mask);
bool gameport_poll_joystick(gameport_t* gp, joystick_status_t* status);
uint16_t gameport_read_axis(gameport_t* gp, uint8 axis_mask);

#endif
