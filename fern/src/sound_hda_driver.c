#include "include/pci.h"
#include "include/io_ports.h"
#include "include/screen.h"
#include "include/mm.h"
#include "include/string.h"
#include "include/sound.h"
#include "include/memory.h"

#define HDA_GCAP        0x00
#define HDA_GCTL        0x08
#define HDA_STATESTS    0x0E
#define HDA_INTCTL      0x20
#define HDA_INTSTS      0x24
#define HDA_CORBLBASE   0x40
#define HDA_CORBUBASE   0x44
#define HDA_CORBWP      0x48
#define HDA_CORBRP      0x4A
#define HDA_CORBCTL     0x4C
#define HDA_CORBSTS     0x4E
#define HDA_CORBSIZE    0x4E
#define HDA_RIRBLBASE   0x50
#define HDA_RIRBUBASE   0x54
#define HDA_RIRBWP      0x58
#define HDA_RINTCNT     0x5A
#define HDA_RIRBCTL     0x5C
#define HDA_RIRBSTS     0x5E
#define HDA_RIRBSIZE    0x5E
#define HDA_DPLBASE     0x70
#define HDA_DPUBASE     0x74
#define HDA_SD_BASE     0x80
#define HDA_SD_SIZE     0x20

#define HDA_CMD_GET_PARAMETER 0xF00
#define HDA_CMD_SET_CONVERTER_FORMAT 0x200
#define HDA_CMD_SET_CONVERTER_STREAM 0x706
#define HDA_CMD_SET_PIN_WIDGET_CONTROL 0x707

typedef struct {
    pci_device_t pci;
    uint32 mmio_base;
    bool initialized;
} hda_state_t;

static inline void hda_write32(hda_state_t* state, uint32 offset, uint32 value) {
    *(volatile uint32*)(state->mmio_base + offset) = value;
}

static inline uint32 hda_read32(hda_state_t* state, uint32 offset) {
    return *(volatile uint32*)(state->mmio_base + offset);
}

static inline void hda_write16(hda_state_t* state, uint32 offset, uint16 value) {
    *(volatile uint16*)(state->mmio_base + offset) = value;
}

static inline uint16 hda_read16(hda_state_t* state, uint32 offset) {
    return *(volatile uint16*)(state->mmio_base + offset);
}

static inline void hda_write8(hda_state_t* state, uint32 offset, uint8 value) {
    *(volatile uint8*)(state->mmio_base + offset) = value;
}

static inline uint8 hda_read8(hda_state_t* state, uint32 offset) {
    return *(volatile uint8*)(state->mmio_base + offset);
}

static bool hda_detect(SoundDriver* driver) {
    if (!driver) {
        return false;
    }
    hda_state_t* state = (hda_state_t*)driver->state;
    if (!state) {
        static hda_state_t static_state;
        state = &static_state;
        driver->state = state;
        memset(state, 0, sizeof(hda_state_t));
    }

    pci_device_t device;
    bool found = false;

    /* Try class code: HD Audio = 0x04/0x03 */
    if (pci_find_by_class(0x04, 0x03, &device)) {
        found = true;
    }

    /* Fallback: known Intel HDA device IDs */
    if (!found) {
        uint16_t hda_devices[] = { 0x2668, 0x27D8, 0x284B, 0x293E, 0x3B56, 0x1C20, 0x1E20 };
        for (uint32_t i = 0; i < sizeof(hda_devices)/sizeof(hda_devices[0]); i++) {
            if (pci_find_by_vendor_device(0x8086, hda_devices[i], &device)) {
                found = true;
                break;
            }
        }
    }

    if (!found) {
        return false;
    }

    state->pci = device;

    uint32 bar0 = device.bar[0];
    if (bar0 & 0x1) {
        return false; /* Must be MMIO */
    }
    state->mmio_base = bar0 & ~0xFu;

    /* Enable bus mastering and memory decode */
    uint16 command = pci_config_read16(device.segment, device.bus, device.device, device.function, 4);
    command |= 0x0006;
    pci_config_write16(device.segment, device.bus, device.device, device.function, 4, command);

    return true;
}

static bool hda_init(SoundDriver* driver) {
    if (!driver || !driver->state) {
        return false;
    }
    hda_state_t* state = (hda_state_t*)driver->state;

    // Take controller out of reset
    hda_write32(state, HDA_GCTL, 1);

    // Wait for controller to be ready
    for (int i = 0; i < 1000; i++) {
        if (hda_read32(state, HDA_GCTL) & 1) break;
    }

    state->initialized = true;
    return true;
}

static bool hda_play_pcm(SoundDriver* driver, const uint8* data, uint32 length, const SoundFormat* format) {
    if (!driver || !driver->state || !data || !format) {
        return false;
    }
    hda_state_t* state = (hda_state_t*)driver->state;
    if (!state->initialized) {
        return false;
    }

    uint32 frames = length / (format->channels * (format->bits_per_sample / 8));
    if (frames == 0) {
        return false;
    }

    // For simplicity, use stream 1, SD0
    uint32 sd_offset = HDA_SD_BASE + 0 * HDA_SD_SIZE;

    // Reset stream
    hda_write8(state, sd_offset + 0x00, 0x01); // Set reset bit
    // Wait for reset
    while (!(hda_read8(state, sd_offset + 0x00) & 0x01));
    hda_write8(state, sd_offset + 0x00, 0x00); // Clear reset bit
    while (hda_read8(state, sd_offset + 0x00) & 0x01);

    // Set stream format based on actual format
    uint16 fmt = 0;
    if (format->bits_per_sample == 16) fmt |= 0x0010;
    else if (format->bits_per_sample == 24) fmt |= 0x0011;
    else if (format->bits_per_sample == 32) fmt |= 0x0012;
    if (format->channels == 2) fmt |= 0x0001; // stereo
    // Sample rate base is 48kHz, but we can set it
    fmt |= 0x4000; // 48kHz base rate
    hda_write16(state, sd_offset + 0x12, fmt);

    // Set BDL - convert virtual to physical address
    uint32 virt_addr = (uint32)data;
    uint32 phys_addr = vmm_get_physical_addr(vmm_get_current_page_directory(), virt_addr);
    hda_write32(state, sd_offset + 0x18, phys_addr & 0xFFFFFFFF);
    hda_write32(state, sd_offset + 0x1C, phys_addr >> 32);

    // Set cyclic buffer length
    uint32 cbl = length;
    hda_write32(state, sd_offset + 0x08, cbl);

    // Set last valid index
    hda_write16(state, sd_offset + 0x0C, 0); // One entry

    // Set stream number
    hda_write8(state, sd_offset + 0x02, 1); // Stream 1

    // Start stream
    hda_write8(state, sd_offset + 0x00, 0x02); // Set run bit

    // Wait for completion
    uint32 timeout = 0;
    while (timeout < 1000000) {  // Timeout after ~1 second
        uint8 status = hda_read8(state, sd_offset + 0x00);
        if ((status & 0x02) == 0) {  // Run bit cleared when done
            break;
        }
        timeout++;
        __asm__ volatile("nop");
    }

    return true;
}

static void hda_set_volume(SoundDriver* driver, uint8 volume) {
    // Not implemented
}

static void hda_beep(SoundDriver* driver, uint32 frequency_hz, uint32 duration_ms) {
    // Not implemented
}

static SoundDriver g_hda_driver = {
    .name = "Intel HD Audio",
    .type = SOUND_DEVICE_HDA,
    .detect = hda_detect,
    .init = hda_init,
    .play_pcm = hda_play_pcm,
    .set_volume = hda_set_volume,
    .beep = hda_beep,
    .shutdown = NULL,
    .state = 0,
    .volume = 255
};

static SoundDriver* sound_hda_driver_legacy(void) {
    return 0;
}
