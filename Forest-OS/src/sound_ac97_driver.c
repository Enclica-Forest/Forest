#include "include/pci.h"
#include "include/io_ports.h"
#include "include/screen.h"
#include "include/mm.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/sound.h"
#include "include/debuglog.h"
#include "include/bitmap_pmm.h"

#define AC97_NAM_RESET       0x00
#define AC97_NAM_MASTER_VOL  0x02
#define AC97_NAM_PCM_VOL     0x18
#define AC97_NABM_PO_BDBAR   0x10
#define AC97_NABM_PO_CIV     0x14
#define AC97_NABM_PO_LVI     0x15
#define AC97_NABM_PO_SR      0x16
#define AC97_NABM_PO_PICB    0x18
#define AC97_NABM_PO_PIV     0x1A
#define AC97_NABM_PO_CR      0x1B
#define AC97_NABM_GLOB_CNT   0x2C
#define AC97_NABM_GLOB_STA   0x30

typedef struct {
    pci_device_t pci;
    uint32 nam_base;
    uint32 nabm_base;
    bool initialized;
    uint32 bdl_phys_addr;  // Physical address of BDL for DMA
    uint32* bdl_virt_addr; // Virtual address of BDL
} ac97_state_t;

static inline void ac97_write_nam(ac97_state_t* state, uint16 offset, uint16 value) {
    outportw((uint16)(state->nam_base + offset), value);
}

static inline uint16 ac97_read_nam(ac97_state_t* state, uint16 offset) {
    return inportw((uint16)(state->nam_base + offset));
}

static inline void ac97_write_nabm(ac97_state_t* state, uint8 offset, uint32 value) {
    outportd((uint16)(state->nabm_base + offset), value);
}

static inline uint32 ac97_read_nabm(ac97_state_t* state, uint8 offset) {
    return inportd((uint16)(state->nabm_base + offset));
}

static bool ac97_detect(SoundDriver* driver) {
    if (!driver) {
        return false;
    }
    ac97_state_t* state = (ac97_state_t*)driver->state;
    if (!state) {
        static ac97_state_t static_state;
        state = &static_state;
        driver->state = state;
    }

    pci_device_t device;
    bool found = false;

    /* First try: PCI class code for AC97 audio (0x04/0x01) */
    if (pci_find_by_class(0x04, 0x01, &device)) {
        found = true;
    }

    /* Second try: known AC97 vendor/device IDs */
    if (!found) {
        struct { uint16 vendor; uint16 device; } ac97_ids[] = {
            { 0x8086, 0x2415 }, /* Intel ICH AC97 (QEMU default) */
            { 0x8086, 0x2425 }, /* Intel ICH2 AC97 */
            { 0x8086, 0x24D5 }, /* Intel ICH4 AC97 */
            { 0x8086, 0x24DD }, /* Intel ICH5 AC97 */
            { 0x8086, 0x266E }, /* Intel ICH6 AC97 */
            { 0x1274, 0x1371 }, /* Ensoniq ES1371 */
        };
        for (uint32 i = 0; i < sizeof(ac97_ids)/sizeof(ac97_ids[0]); i++) {
            if (pci_find_by_vendor_device(ac97_ids[i].vendor, ac97_ids[i].device, &device)) {
                found = true;
                break;
            }
        }
    }

    if (!found) {
        return false;
    }

    state->pci = device;
    state->nam_base = device.bar[0] & ~0x1;
    state->nabm_base = device.bar[1] & ~0x1;
    if (!state->nam_base || !state->nabm_base) {
        return false;
    }

    /* Enable bus mastering and I/O space decode */
    uint16 command = pci_config_read16(device.segment, device.bus, device.device, device.function, 4);
    command |= 0x0005; /* Bus master enable + I/O space enable */
    pci_config_write16(device.segment, device.bus, device.device, device.function, 4, command);

    return true;
}

static bool ac97_init(SoundDriver* driver) {
    if (!driver || !driver->state) {
        return false;
    }
    ac97_state_t* state = (ac97_state_t*)driver->state;

    // Reset NAM
    ac97_write_nam(state, AC97_NAM_RESET, 0);

    // Wait
    for (int i = 0; i < 1000; i++) {
        // delay
    }

    // Set master volume to max
    ac97_write_nam(state, AC97_NAM_MASTER_VOL, 0x0000);

    // Set PCM volume to max
    ac97_write_nam(state, AC97_NAM_PCM_VOL, 0x0000);

    // Reset NABM
    ac97_write_nabm(state, AC97_NABM_GLOB_CNT, 0x02); // Cold reset
    ac97_write_nabm(state, AC97_NABM_GLOB_CNT, 0x03); // Enable interrupts

    // Allocate BDL (Buffer Descriptor List) from physically contiguous memory
    // We need at least 16 bytes (4 uint32s) for the BDL entry
    uint32 bdl_frame = bitmap_pmm_alloc_page(PMM_ALLOC_LOW_MEMORY); // Prefer low memory for DMA
    if (bdl_frame == 0) {
        debuglog(DEBUG_ERROR, "AC97: Failed to allocate BDL page\n");
        return false;
    }

    // Convert frame number to physical address (frame * page_size)
    uint32 bdl_phys = bdl_frame * MEMORY_PAGE_SIZE;
    
    // Map the physical frame to a virtual address using proper mapping
    // Use a high virtual address to avoid conflicts
    uint32 bdl_virt = 0xE0000000 + bdl_frame * MEMORY_PAGE_SIZE;
    memory_result_t map_result = vmm_map_page(vmm_get_current_page_directory(),
                                             bdl_virt, bdl_frame,
                                             PAGE_PRESENT | PAGE_WRITABLE);
    if (map_result != MEMORY_OK) {
        debuglog(DEBUG_ERROR, "AC97: Failed to map BDL page: %d\n", map_result);
        bitmap_pmm_free_page(bdl_frame);
        return false;
    }

    state->bdl_phys_addr = bdl_phys;
    state->bdl_virt_addr = (uint32*)bdl_virt;

    debuglog(DEBUG_INFO, "AC97: BDL allocated - frame:0x%08x phys:0x%08x virt:0x%08x\n",
             bdl_frame, state->bdl_phys_addr, (uint32)state->bdl_virt_addr);

    // CRITICAL: Validate BDL physical address is not zero
    if (state->bdl_phys_addr == 0) {
        debuglog(DEBUG_ERROR, "AC97: CRITICAL ERROR - BDL physical address is 0! DMA cannot work.\n");
        return false; // This must fail - DMA cannot work without a valid BDL
    }

    state->initialized = true;
    return true;
}

static bool ac97_play_pcm(SoundDriver* driver, const uint8* data, uint32 length, const SoundFormat* format) {
    if (!driver || !driver->state || !data || !format) {
        return false;
    }
    ac97_state_t* state = (ac97_state_t*)driver->state;
    if (!state->initialized) {
        return false;
    }

    // CRITICAL VALIDATION: Ensure BDL physical address is valid
    if (state->bdl_phys_addr == 0) {
        debuglog(DEBUG_ERROR, "AC97: CRITICAL - BDL physical address is 0! Cannot play audio.\n");
        return false;
    }

    debuglog(DEBUG_INFO, "[SOUND] AC97 playing PCM: %u bytes, %u Hz, %u channels, %u bps\n",
             length, format->sample_rate, format->channels, format->bits_per_sample);

    // For simplicity, assume 16-bit stereo
    uint32 samples = length / 4; // 4 bytes per sample (16-bit stereo)

    // Check if DMA is already running - don't restart it
    uint8 current_cr = ac97_read_nabm(state, AC97_NABM_PO_CR);
    if (current_cr & 0x01) { // Check RUN bit
        debuglog(DEBUG_INFO, "[SOUND] AC97 DMA already running, skipping\n");
        return true;
    }

    // Get physical address of audio data - ensure it's page-aligned for DMA
    uint32 data_vaddr = (uint32)data;
    uint32 data_phys_addr = vmm_get_physical_addr(vmm_get_current_page_directory(), data_vaddr);
    if (data_phys_addr == 0) {
        debuglog(DEBUG_ERROR, "AC97: Failed to get physical address of audio data (vaddr: 0x%08x)\n", data_vaddr);
        return false;
    }
    
    debuglog(DEBUG_INFO, "AC97: Audio data vaddr:0x%08x -> phys:0x%08x\n", data_vaddr, data_phys_addr);

    // Build BDL entry in pre-allocated DMA-safe memory
    // BDL entry format: [phys_addr][reserved][samples][control]
    state->bdl_virt_addr[0] = data_phys_addr;           // Physical address of buffer
    state->bdl_virt_addr[1] = 0;                        // Reserved/control bits
    state->bdl_virt_addr[2] = samples;                   // Number of samples
    state->bdl_virt_addr[3] = 0x80000000;                // IOC bit set (interrupt on completion)

    debuglog(DEBUG_INFO, "[AC97] BDL Entry - addr:0x%08x samples:%u ctrl:0x%08x\n",
             state->bdl_virt_addr[0], state->bdl_virt_addr[2], state->bdl_virt_addr[3]);

    // CRITICAL: Set BDL address (MUST be physical address for DMA)
    debuglog(DEBUG_INFO, "[AC97] Setting BDL BAR to physical address: 0x%08x\n", state->bdl_phys_addr);
    ac97_write_nabm(state, AC97_NABM_PO_BDBAR, state->bdl_phys_addr);

    // Set Last Valid Index (1 entry, index 0)
    ac97_write_nabm(state, AC97_NABM_PO_LVI, 0);

    // Set Current Index to 0
    ac97_write_nabm(state, AC97_NABM_PO_CIV, 0);

    // Clear status register before starting DMA
    ac97_write_nabm(state, AC97_NABM_PO_SR, 0x1C);

    // Set Control: run, interrupt enable
    ac97_write_nabm(state, AC97_NABM_PO_CR, 0x05);

    debuglog(DEBUG_INFO, "[AC97] DMA started with BDL at physical 0x%08x, samples %u\n", 
             state->bdl_phys_addr, samples);

    return true;
}

static void ac97_set_volume(SoundDriver* driver, uint8 volume) {
    if (!driver || !driver->state) {
        return;
    }
    ac97_state_t* state = (ac97_state_t*)driver->state;
    if (!state->initialized) {
        return;
    }

    uint16 vol = (255 - volume) / 2; // Convert 0-255 to 0-127, but AC97 is 0-31.5 dB
    vol = (vol << 8) | vol;
    ac97_write_nam(state, AC97_NAM_MASTER_VOL, vol);
    ac97_write_nam(state, AC97_NAM_PCM_VOL, vol);
}

static void ac97_beep(SoundDriver* driver, uint32 frequency_hz, uint32 duration_ms) {
    // Not implemented
}

static SoundDriver g_ac97_driver = {
    .name = "AC'97",
    .type = SOUND_DEVICE_AC97,
    .detect = ac97_detect,
    .init = ac97_init,
    .play_pcm = ac97_play_pcm,
    .set_volume = ac97_set_volume,
    .beep = ac97_beep,
    .shutdown = NULL,
    .state = 0,
    .volume = 255
};

SoundDriver* sound_ac97_driver(void) {
    return &g_ac97_driver;
}