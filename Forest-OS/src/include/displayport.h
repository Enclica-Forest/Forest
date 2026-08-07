#ifndef DISPLAYPORT_H
#define DISPLAYPORT_H

#include "types.h"
#include <stdbool.h>
#include <stdint.h>

#define DISPLAYPORT_AUX_MAX_PACKET_SIZE 20

typedef enum {
    DP_LINK_RATE_162GBPS = 0x06,
    DP_LINK_RATE_270GBPS = 0x0A,
    DP_LINK_RATE_540GBPS = 0x14,
    DP_LINK_RATE_810GBPS = 0x1E
} dp_link_rate_t;

typedef enum {
    DP_LANE_COUNT_1 = 1,
    DP_LANE_COUNT_2 = 2,
    DP_LANE_COUNT_4 = 4
} dp_lane_count_t;

typedef enum {
    DP_AUX_TYPE_NATIVE = 0,
    DP_AUX_TYPE_I2C = 1
} dp_aux_type_t;

typedef enum {
    DP_AUX_REQUEST_WRITE = 0,
    DP_AUX_REQUEST_READ = 1,
    DP_AUX_REQUEST_WRITE_STATUS = 2
} dp_aux_request_t;

typedef enum {
    DP_AUX_REPLY_ACK = 0,
    DP_AUX_REPLY_NACK = 1,
    DP_AUX_REPLY_DEFER = 2
} dp_aux_reply_t;

typedef struct {
    uint8 type;
    uint8 request;
    uint8 address;
    uint8 length;
    uint8 data[DISPLAYPORT_AUX_MAX_PACKET_SIZE - 4];
} __attribute__((packed)) dp_aux_packet_t;

typedef struct {
    uint8 native_reply;
    uint8 i2c_reply;
    uint8 padding[2];
    uint8 data[DISPLAYPORT_AUX_MAX_PACKET_SIZE];
} __attribute__((packed)) dp_aux_reply_packet_t;

typedef struct {
    uint8 version;
    uint8 revision;
    uint32 max_link_rate;
    uint8 max_lane_count;
    uint8 max_downspread;
    uint8 nozzle_count;
    uint8 downstream_port_present;
    uint8 main_link_type;
    uint8 aux_cross_coupling;
    uint8 dp_edp;
    uint16 dp_max_link_rate;
    uint8 dp_max_num_lanes;
    uint8 dp_max_downspread;
    uint8 dp_no_aux_handshake;
    uint16 dp_max_aux_wireless_microsecond_delay;
    uint16 dp_max_aux_wireless_byte_delay;
    uint32 dp_i2c_speed;
    uint8 dp_edp_display_control_capable;
    uint8 dp_train_aux;
    uint8 dp_train_aux_rd_interval;
} __attribute__((packed)) dp_link_bws_capability_t;

typedef struct {
    dp_link_rate_t link_rate;
    dp_lane_count_t lane_count;
    bool enhanced_framing;
    bool scrambler_enable;
} dp_link_config_t;

typedef struct displayport displayport_t;
typedef bool (*dp_aux_read_func)(displayport_t* dp, uint8 address, void* data, uint8 length);
typedef bool (*dp_aux_write_func)(displayport_t* dp, uint8 address, const void* data, uint8 length);

struct displayport {
    uint16 vendor_id;
    uint16 device_id;
    void* private_data;
    
    dp_aux_read_func aux_read;
    dp_aux_write_func aux_write;
    
    dp_link_config_t link_config;
    uint8 dpcd_data[256];
    bool initialized;
};

bool displayport_init(void);
void displayport_shutdown(void);
displayport_t* displayport_allocate_device(void);
void displayport_free_device(displayport_t* dp);

bool dp_aux_native_read(displayport_t* dp, uint32 address, void* data, uint8 length);
bool dp_aux_native_write(displayport_t* dp, uint32 address, const void* data, uint8 length);
bool dp_aux_i2c_read(displayport_t* dp, uint8 address, void* data, uint8 length);
bool dp_aux_i2c_write(displayport_t* dp, uint8 address, const void* data, uint8 length);

bool dp_dpcd_read(displayport_t* dp, uint32 address, void* data, uint8 length);
bool dp_dpcd_write(displayport_t* dp, uint32 address, const void* data, uint8 length);
bool dp_get_edid(displayport_t* dp, void* buffer, uint32 length);
bool dp_configure_link(displayport_t* dp, dp_link_rate_t rate, dp_lane_count_t lanes);
bool dp_set_power_state(displayport_t* dp, bool powered);

#endif
