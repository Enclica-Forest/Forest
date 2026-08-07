#ifndef PARALLELPORT_H
#define PARALLELPORT_H

#include "types.h"
#include <stdbool.h>
#include <stdint.h>

#define LPT1_IO_BASE 0x378
#define LPT2_IO_BASE 0x278
#define LPT3_IO_BASE 0x3BC

#define LPT_MAX_PORTS 3

typedef enum {
    LPT_MODE_SPP = 0,
    LPT_MODE_BIDIRECTIONAL = 1,
    LPT_MODE_EPP = 2,
    LPT_MODE_ECP = 3
} lpt_mode_t;

typedef struct {
    uint8 control;
    uint8 status;
    uint8 data;
} lpt_register_t;

#define LPT_DATA_REG 0x00
#define LPT_STATUS_REG 0x01
#define LPT_CONTROL_REG 0x02

#define LPT_STATUS_ERROR 0x08
#define LPT_STATUS_SLCT 0x10
#define LPT_STATUS_PE 0x20
#define LPT_STATUS_ACK  0x40
#define LPT_STATUS_BUSY 0x80

#define LPT_CONTROL_STROBE 0x01
#define LPT_CONTROL_AUTO_LF 0x02
#define LPT_CONTROL_INIT 0x04
#define LPT_CONTROL_SLCT_IN 0x08
#define LPT_CONTROL_IRQ_EN 0x10
#define LPT_CONTROL_DIR 0x20

typedef struct parallelport parallelport_t;
typedef bool (*lpt_write_data_func)(parallelport_t* lpt, uint8 data);
typedef bool (*lpt_read_status_func)(parallelport_t* lpt, uint8* status);
typedef bool (*lpt_write_control_func)(parallelport_t* lpt, uint8 control);
typedef bool (*lpt_send_byte_func)(parallelport_t* lpt, uint8 data);

struct parallelport {
    uint16 io_base;
    uint8 port_num;
    lpt_mode_t mode;
    lpt_register_t registers;
    void* private_data;
    
    lpt_write_data_func write_data;
    lpt_read_status_func read_status;
    lpt_write_control_func write_control;
    lpt_send_byte_func send_byte;
    
    bool initialized;
};

bool lpt_init(void);
void lpt_shutdown(void);
parallelport_t* lpt_allocate_port(uint8 port_num);
void lpt_free_port(parallelport_t* lpt);

bool lpt_detect_port(uint16 io_base);
bool lpt_write_data(parallelport_t* lpt, uint8 data);
bool lpt_read_status(parallelport_t* lpt, uint8* status);
bool lpt_write_control(parallelport_t* lpt, uint8 control);
bool lpt_send_byte(parallelport_t* lpt, uint8 data);
bool lpt_print_string(parallelport_t* lpt, const char* str);

#endif
