#include "include/displayport.h"
#include "include/pci.h"
#include "include/system.h"
#include "include/driver.h"
#include "include/memory.h"
#include "include/memory_safe.h"
#include "include/string.h"
#include "include/screen.h"

/* Compatibility macro */
#ifndef kmalloc
#define kmalloc(size) memory_heap_alloc(size)
#endif

#define PCI_CLASS_DISPLAY 0x03
#define PCI_SUBCLASS_DISPLAY_VGA 0x00

#define DP_DPCD_RECEIVER_CAP_FIELD 0x00000
#define DP_DPCD_LINK_BW_SET 0x00100
#define DP_DPCD_LANE_COUNT_SET 0x00104
#define DP_DPCD_ENHANCED_FRAME_EN 0x00108
#define DP_DPCD_TRAINING_PATTERN_SET 0x00102
#define DP_DPCD_LINK_QUAL_PATTERN_SET 0x00106
#define DP_DPCD_SCRAMBLER_DISABLE 0x00109
#define DP_DPCD_DOWNSPREAD_CTRL 0x0010B

#define DP_TRAINING_PATTERN_1 1
#define DP_TRAINING_PATTERN_2 2
#define DP_TRAINING_PATTERN_OFF 0

#define DP_MAX_READ_ATTEMPTS 3
#define DP_MAX_DEFER_COUNT 5

static displayport_t* g_dp_devices[4];
static uint32 g_dp_count = 0;

static bool dp_aux_wait_ready(displayport_t* dp, uint32 timeout_ms) {
    (void)dp;
    (void)timeout_ms;
    timer_sleep_ms(1);
    return true;
}

bool dp_aux_native_read(displayport_t* dp, uint32 address, void* data, uint8 length) {
    if (!dp || !dp->aux_read || !data) {
        return false;
    }

    for (uint8 attempt = 0; attempt < DP_MAX_READ_ATTEMPTS; attempt++) {
        if (dp->aux_read(dp, address & 0xFFFF, data, length)) {
            return true;
        }
        timer_sleep_ms(10);
    }

    return false;
}

bool dp_aux_native_write(displayport_t* dp, uint32 address, const void* data, uint8 length) {
    if (!dp || !dp->aux_write || !data) {
        return false;
    }

    for (uint8 attempt = 0; attempt < DP_MAX_READ_ATTEMPTS; attempt++) {
        if (dp->aux_write(dp, address & 0xFFFF, data, length)) {
            return true;
        }
        timer_sleep_ms(10);
    }

    return false;
}

bool dp_aux_i2c_read(displayport_t* dp, uint8 address, void* data, uint8 length) {
    if (!dp || !data || length == 0) {
        return false;
    }

    uint8 mot = (length > 1) ? 1 : 0;

    uint8 setup_packet[3] = {
        (DP_AUX_TYPE_I2C << 7) | (mot << 6) | DP_AUX_REQUEST_READ,
        0,
        address
    };

    if (!dp_aux_native_write(dp, 0, setup_packet, 3)) {
        return false;
    }

    timer_sleep_ms(1);

    if (length == 1) {
        return dp_aux_native_read(dp, 0, data, length);
    }

    for (uint8 i = 0; i < length; i++) {
        uint8 addr_packet[4] = {
            (DP_AUX_TYPE_I2C << 7) | (mot << 6) | DP_AUX_REQUEST_READ,
            0,
            address,
            0
        };

        if (!dp_aux_native_write(dp, 0, addr_packet, 4)) {
            return false;
        }

        timer_sleep_ms(1);

        if (!dp_aux_native_read(dp, 0, (uint8*)data + i, 1)) {
            return false;
        }

        mot = (i < length - 2) ? 1 : 0;
    }

    return true;
}

bool dp_aux_i2c_write(displayport_t* dp, uint8 address, const void* data, uint8 length) {
    if (!dp || !data || length == 0) {
        return false;
    }

    uint8 mot = (length > 1) ? 1 : 0;

    uint8 setup_packet[3] = {
        (DP_AUX_TYPE_I2C << 7) | (mot << 6) | DP_AUX_REQUEST_WRITE,
        0,
        address
    };

    if (!dp_aux_native_write(dp, 0, setup_packet, 3)) {
        return false;
    }

    timer_sleep_ms(1);

    uint8 len_packet[4] = {
        (DP_AUX_TYPE_I2C << 7) | (mot << 6) | DP_AUX_REQUEST_WRITE,
        0,
        address,
        length - 1
    };

    if (!dp_aux_native_write(dp, 0, len_packet, 4)) {
        return false;
    }

    timer_sleep_ms(1);

    return dp_aux_native_write(dp, 0, data, length);
}

bool dp_dpcd_read(displayport_t* dp, uint32 address, void* data, uint8 length) {
    if (!dp || !data || length == 0 || length > 16) {
        return false;
    }

    uint8 request[4] = {
        (DP_AUX_TYPE_NATIVE << 7) | DP_AUX_REQUEST_READ,
        (address >> 8) & 0xFF,
        address & 0xFF,
        length - 1
    };

    if (!dp_aux_native_write(dp, 0, request, 4)) {
        return false;
    }

    timer_sleep_ms(1);

    return dp_aux_native_read(dp, 0, data, length);
}

bool dp_dpcd_write(displayport_t* dp, uint32 address, const void* data, uint8 length) {
    if (!dp || !data || length == 0 || length > 16) {
        return false;
    }

    uint8 request[4] = {
        (DP_AUX_TYPE_NATIVE << 7) | DP_AUX_REQUEST_WRITE,
        (address >> 8) & 0xFF,
        address & 0xFF,
        length - 1
    };

    if (!dp_aux_native_write(dp, 0, request, 4)) {
        return false;
    }

    timer_sleep_ms(1);

    return dp_aux_native_write(dp, 0, data, length);
}

bool dp_get_edid(displayport_t* dp, void* buffer, uint32 length) {
    if (!dp || !buffer || length < 128) {
        return false;
    }

    uint8 offset = 0;
    uint8 segment = 0;
    uint32 total_read = 0;

    while (total_read < 128 && total_read < length) {
        uint8 i2c_addr = 0x50 | segment;

        if (!dp_aux_i2c_write(dp, i2c_addr, &offset, 1)) {
            return false;
        }

        timer_sleep_ms(1);

        uint8 bytes_to_read = (length - total_read > 16) ? 16 : (length - total_read);
        if (!dp_aux_i2c_read(dp, i2c_addr, (uint8*)buffer + total_read, bytes_to_read)) {
            return false;
        }

        total_read += bytes_to_read;
        offset += bytes_to_read;

        if (offset == 0) {
            segment = 1;
            offset = 0x80;
        }
    }

    return true;
}

bool dp_configure_link(displayport_t* dp, dp_link_rate_t rate, dp_lane_count_t lanes) {
    if (!dp) {
        return false;
    }

    uint8 link_bw_set = (uint8)rate;
    if (!dp_dpcd_write(dp, DP_DPCD_LINK_BW_SET, &link_bw_set, 1)) {
        return false;
    }

    uint8 lane_count_set = (uint8)lanes;
    if (!dp_dpcd_write(dp, DP_DPCD_LANE_COUNT_SET, &lane_count_set, 1)) {
        return false;
    }

    uint8 enhanced_frame = 1;
    if (!dp_dpcd_write(dp, DP_DPCD_ENHANCED_FRAME_EN, &enhanced_frame, 1)) {
        return false;
    }

    uint8 scrambler_disable = 0;
    if (!dp_dpcd_write(dp, DP_DPCD_SCRAMBLER_DISABLE, &scrambler_disable, 1)) {
        return false;
    }

    uint8 downspread = 0;
    if (!dp_dpcd_write(dp, DP_DPCD_DOWNSPREAD_CTRL, &downspread, 1)) {
        return false;
    }

    dp->link_config.link_rate = rate;
    dp->link_config.lane_count = lanes;
    dp->link_config.enhanced_framing = true;
    dp->link_config.scrambler_enable = true;

    print("[DP] Link configured: Rate=");
    switch (rate) {
        case DP_LINK_RATE_162GBPS: print("1.62"); break;
        case DP_LINK_RATE_270GBPS: print("2.7"); break;
        case DP_LINK_RATE_540GBPS: print("5.4"); break;
        case DP_LINK_RATE_810GBPS: print("8.1"); break;
        default: print("Unknown"); break;
    }
    print(" Gbps, Lanes=");
    print_dec(lanes);
    print("\n");

    return true;
}

bool dp_set_power_state(displayport_t* dp, bool powered) {
    if (!dp) {
        return false;
    }

    print("[DP] Setting power state: ");
    print(powered ? "On" : "Off");
    print("\n");

    return true;
}

displayport_t* displayport_allocate_device(void) {
    if (g_dp_count >= 4) {
        return 0;
    }

    displayport_t* dp = (displayport_t*)kmalloc(sizeof(displayport_t));
    if (!dp) {
        return 0;
    }

    memory_set((uint8*)dp, 0, sizeof(displayport_t));

    dp->link_config.link_rate = DP_LINK_RATE_162GBPS;
    dp->link_config.lane_count = DP_LANE_COUNT_1;
    dp->link_config.enhanced_framing = false;
    dp->link_config.scrambler_enable = false;

    g_dp_devices[g_dp_count++] = dp;

    return dp;
}

void displayport_free_device(displayport_t* dp) {
    if (!dp) {
        return;
    }

    for (uint32 i = 0; i < g_dp_count; i++) {
        if (g_dp_devices[i] == dp) {
            g_dp_devices[i] = 0;
            break;
        }
    }

    kfree(dp);
}

static bool dp_pci_callback(const pci_device_t* device, void* context) {
    (void)context;

    if (device->class_code != PCI_CLASS_DISPLAY) {
        return false;
    }

    print("[DP] Found display device: VID=");
    print_hex(device->vendor_id);
    print(" PID=");
    print_hex(device->device_id);
    print("\n");

    displayport_t* dp = displayport_allocate_device();
    if (dp) {
        dp->vendor_id = device->vendor_id;
        dp->device_id = device->device_id;
        dp->initialized = true;
    }

    return false;
}

bool displayport_init(void) {
    print("[DP] Initializing DisplayPort driver...\n");

    memory_set((uint8*)g_dp_devices, 0, sizeof(g_dp_devices));
    g_dp_count = 0;

    pci_enumerate(dp_pci_callback, 0);

    print("[DP] DisplayPort driver initialized\n");
    return true;
}

void displayport_shutdown(void) {
    print("[DP] Shutting down DisplayPort driver...\n");

    for (uint32 i = 0; i < g_dp_count; i++) {
        if (g_dp_devices[i]) {
            displayport_free_device(g_dp_devices[i]);
        }
    }

    g_dp_count = 0;
}
