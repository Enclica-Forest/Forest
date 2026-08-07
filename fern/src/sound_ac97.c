#include "include/sound.h"
#include "include/pci.h"
#include "include/io_ports.h"
#include "include/screen.h"
#include "include/memory.h"
#include "include/libc/string.h"
#include "include/debuglog.h"
#include "include/interrupt.h"
#include "include/timer.h"
#include "include/bitmap_pmm.h"
#include "include/cpu_ops.h"

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#define AC97_NUM_BUFFERS 2
#define AC97_VRA_BIT 0x0001
#define AC97_NDRA_BIT 0x0002

#define AC97_NAM_RESET            0x00
#define AC97_NAM_MASTER_VOL       0x02
#define AC97_NAM_HP_VOL           0x04
#define AC97_NAM_MONO_VOL         0x06
#define AC97_NAM_LINE_IN_VOL      0x08
#define AC97_NAM_CD_VOL           0x0A
#define AC97_NAM_PCM_VOL          0x18
#define AC97_NAM_POWER            0x26
#define AC97_NAM_EXT_AUDIO_ID     0x28
#define AC97_NAM_EXT_AUDIO_CTRL   0x2A
#define AC97_NAM_FRONT_DAC_RATE   0x2C
#define AC97_NAM_SURR_DAC_RATE    0x2E
#define AC97_NAM_LFE_DAC_RATE     0x30
#define AC97_NAM_ADC_RATE         0x32
#define AC97_NAM_EXT_STATUS       0x58
#define AC97_NAM_VENDOR_ID1       0x7C
#define AC97_NAM_VENDOR_ID2       0x7E

#define AC97_POWER_LINE_OUT       (1 << 0)
#define AC97_POWER_ADC            (1 << 1)
#define AC97_POWER_DAC            (1 << 2)
#define AC97_POWER_MIXER          (1 << 3)
#define AC97_POWER_VREF           (1 << 4)
#define AC97_POWER_ANL            (1 << 5)
#define AC97_POWER_REF            (1 << 6)
#define AC97_POWER_STATUS_MASK    0x7F
#define AC97_POWER_D3             0xFF00
#define AC97_POWER_D0             0x0000

#define AC97_NABM_PO_BDBAR       0x10
#define AC97_NABM_PO_CIV         0x14
#define AC97_NABM_PO_LVI         0x15
#define AC97_NABM_PO_SR          0x16
#define AC97_NABM_PO_PICB        0x18
#define AC97_NABM_PO_CR          0x1B
#define AC97_NABM_GLOBAL_CTRL     0x2C
#define AC97_NABM_GLOBAL_STATUS   0x30

#define AC97_PO_CR_RUN           0x01
#define AC97_PO_CR_RESET         0x02
#define AC97_PO_CR_IOCE          0x04
#define AC97_PO_CR_FEIE          0x08

#define AC97_SR_DSCP             0x10
#define AC97_SR_BCIS             0x08
#define AC97_SR_LVBCI            0x04
#define AC97_SR_CE               0x02
#define AC97_SR_DCH              0x01

#define AC97_GLOBAL_CTRL_SRIE    0x02
#define AC97_GLOBAL_CTRL_COLD_RST 0x02
#define AC97_GLOBAL_CTRL_RUN     0x01
#define AC97_GLOBAL_CTRL_PIIE    0x08
#define AC97_GLOBAL_CTRL_AIE     0x10
#define AC97_GLOBAL_CTRL_MIE     0x20

#define AC97_GLOBAL_STATUS_PSO   (1 << 9)
#define AC97_GLOBAL_STATUS_PIINT (1 << 2)

#define AC97_EXT_STATUS_SPSR_48K 0x0040
#define AC97_EXT_STATUS_SPSR_44K 0x0020
#define AC97_EXT_STATUS_SPSR_32K 0x0010
#define AC97_EXT_STATUS_VRA      0x0001
#define AC97_EXT_STATUS_NDRA     0x0002

#define AC97_VOL_MAX             31
#define AC97_VOL_MASK            0x1F
#define AC97_VOL_MUTE            (1 << 15)

typedef struct {
    uint32_t phys_addr;
    uint16_t length;
    uint16_t flags;
} __attribute__((packed)) ac97_bdl_entry_t;

typedef struct {
    pci_device_t pci;
    uint16_t nam_base;
    uint16_t nabm_base;
    ac97_bdl_entry_t* bdl;
    uint32_t bdl_phys;
    uint8_t* dma_buffer_virt;
    uint32_t dma_buffer_phys;
    uint32_t dma_buffer_size;
    bool initialized;
    bool vra_supported;
    bool ndra_supported;
    uint32_t native_rate;
    uint32_t current_sample_rate;
    uint8_t irq;
    uint16_t vendor_id1;
    uint16_t vendor_id2;
    uint16_t ext_status;
    uint8_t master_vol;
    uint8_t hp_vol;
    uint8_t pcm_vol;
    bool master_muted;
    bool hp_muted;
    bool pcm_muted;
    volatile uint8_t active_buffer;
    volatile bool buffer_done[AC97_NUM_BUFFERS];
    volatile bool dma_active;
    uint32_t buffer_frame;
    uint32_t bdl_frame;
    uint32_t num_pages;
    bool power_line_out;
    bool power_adc;
    bool power_dac;
    bool power_mixer;
    bool power_vref;
    bool power_anl;
    bool power_ref;
} ac97_state_t;

static ac97_state_t g_ac97_state = {0};
static volatile bool g_ac97_streaming = false;

static inline uint16_t ac97_read_nam(ac97_state_t* state, uint8_t reg) {
    return inportw(state->nam_base + reg);
}

static inline void ac97_write_nam(ac97_state_t* state, uint8_t reg, uint16_t value) {
    outportw(state->nam_base + reg, value);
}

static inline uint32_t ac97_read_nabm32(ac97_state_t* state, uint8_t reg) {
    return inportd(state->nabm_base + reg);
}

static inline void ac97_write_nabm32(ac97_state_t* state, uint8_t reg, uint32_t value) {
    outportd(state->nabm_base + reg, value);
}

static inline void ac97_write_nabm8(ac97_state_t* state, uint8_t reg, uint8_t value) {
    outportb(state->nabm_base + reg, value);
}

static inline uint8_t ac97_read_nabm8(ac97_state_t* state, uint8_t reg) {
    return inportb(state->nabm_base + reg);
}

static void ac97_interrupt_handler(struct interrupt_frame* frame, uint32_t error_code) {
    (void)frame;
    (void)error_code;

    ac97_state_t* state = &g_ac97_state;
    if (!state->initialized) return;

    uint32_t status = ac97_read_nabm32(state, AC97_NABM_PO_SR);
    if (!status) {
        uint32_t glob_status = ac97_read_nabm32(state, AC97_NABM_GLOBAL_STATUS);
        if (!(glob_status & AC97_GLOBAL_STATUS_PIINT)) return;
    }

    if (status & AC97_SR_BCIS) {
        state->buffer_done[0] = true;
        state->active_buffer = 1;
    }

    if (status & AC97_SR_LVBCI) {
        state->buffer_done[1] = true;
        state->active_buffer = 0;
    }

    ac97_write_nabm32(state, AC97_NABM_PO_SR, status);
}

#define AC97_VENDOR_INTEL       0x8086
#define AC97_DEVICE_ICH_2415    0x2415
#define AC97_DEVICE_ICH2_2425   0x2425
#define AC97_DEVICE_ICH4_24D5   0x24D5
#define AC97_DEVICE_ICH5_24DD   0x24DD
#define AC97_DEVICE_ICH6_266E   0x266E
#define AC97_VENDOR_ENSONIQ     0x1274
#define AC97_DEVICE_ES1371      0x1371

static bool ac97_detect(SoundDriver* driver) {
    if (!driver) return false;

    ac97_state_t* state = &g_ac97_state;
    driver->state = state;
    memset(state, 0, sizeof(ac97_state_t));

    pci_device_t device;
    bool found = false;

    if (pci_find_by_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, &device)) {
        debuglog(DEBUG_INFO, "AC97: Found by class 0x04/0x01\n");
        found = true;
    }

    if (!found) {
        struct { uint16_t vendor; uint16_t device; } ac97_ids[] = {
            { AC97_VENDOR_INTEL,   AC97_DEVICE_ICH_2415  },
            { AC97_VENDOR_INTEL,   AC97_DEVICE_ICH2_2425 },
            { AC97_VENDOR_INTEL,   AC97_DEVICE_ICH4_24D5 },
            { AC97_VENDOR_INTEL,   AC97_DEVICE_ICH5_24DD },
            { AC97_VENDOR_INTEL,   AC97_DEVICE_ICH6_266E },
            { AC97_VENDOR_ENSONIQ, AC97_DEVICE_ES1371    },
        };
        for (uint32_t i = 0; i < sizeof(ac97_ids)/sizeof(ac97_ids[0]); i++) {
            if (pci_find_by_vendor_device(ac97_ids[i].vendor, ac97_ids[i].device, &device)) {
                debuglog(DEBUG_INFO, "AC97: Found %04X:%04X\n", ac97_ids[i].vendor, ac97_ids[i].device);
                found = true;
                break;
            }
        }
    }

    if (!found) return false;

    state->pci = device;
    state->nam_base = (uint16_t)(device.bar[0] & ~0x3);
    state->nabm_base = (uint16_t)(device.bar[1] & ~0x3);

    if (!state->nam_base || !state->nabm_base) {
        debuglog(DEBUG_ERROR, "AC97: Invalid BARs BAR0=0x%x BAR1=0x%x\n", device.bar[0], device.bar[1]);
        return false;
    }

    state->vendor_id1 = pci_config_read16(device.segment, device.bus, device.device, device.function, 0);
    state->vendor_id2 = pci_config_read16(device.segment, device.bus, device.device, device.function, 2);
    debuglog(DEBUG_INFO, "AC97: PCI %04X:%04X BAR0=0x%X BAR1=0x%X\n",
             state->vendor_id1, state->vendor_id2, state->nam_base, state->nabm_base);

    return true;
}

static void ac97_power_up(ac97_state_t* state) {
    ac97_write_nam(state, AC97_NAM_POWER, AC97_POWER_D0);
    for (volatile int i = 0; i < 5000; i++) { __asm__ volatile("nop"); }

    uint16_t power = ac97_read_nam(state, AC97_NAM_POWER);
    state->power_line_out = !(power & AC97_POWER_LINE_OUT);
    state->power_adc = !(power & AC97_POWER_ADC);
    state->power_dac = !(power & AC97_POWER_DAC);
    state->power_mixer = !(power & AC97_POWER_MIXER);
    state->power_vref = !(power & AC97_POWER_VREF);
    state->power_anl = !(power & AC97_POWER_ANL);
    state->power_ref = !(power & AC97_POWER_REF);
    debuglog(DEBUG_INFO, "AC97: Power status: 0x%04X (line_out=%d adc=%d dac=%d mixer=%d vref=%d anl=%d ref=%d)\n",
             power, state->power_line_out, state->power_adc, state->power_dac,
             state->power_mixer, state->power_vref, state->power_anl, state->power_ref);
}

static void ac97_power_down(ac97_state_t* state) {
    uint16_t power = AC97_POWER_D3;
    ac97_write_nam(state, AC97_NAM_POWER, power);
    for (volatile int i = 0; i < 2000; i++) { __asm__ volatile("nop"); }

    state->power_line_out = false;
    state->power_adc = false;
    state->power_dac = false;
    state->power_mixer = false;
    state->power_vref = false;
    state->power_anl = false;
    state->power_ref = false;
}

static bool ac97_verify_codec(ac97_state_t* state) {
    state->vendor_id1 = ac97_read_nam(state, AC97_NAM_VENDOR_ID1);
    state->vendor_id2 = ac97_read_nam(state, AC97_NAM_VENDOR_ID2);

    if (state->vendor_id1 == 0xFFFF || state->vendor_id1 == 0x0000) {
        debuglog(DEBUG_ERROR, "AC97: Codec ID1 invalid: 0x%04X\n", state->vendor_id1);
        return false;
    }
    if (state->vendor_id2 == 0xFFFF || state->vendor_id2 == 0x0000) {
        debuglog(DEBUG_ERROR, "AC97: Codec ID2 invalid: 0x%04X\n", state->vendor_id2);
        return false;
    }

    debuglog(DEBUG_INFO, "AC97: Codec ID: vendor=0x%04X device=0x%04X\n", state->vendor_id1, state->vendor_id2);
    return true;
}

static void ac97_detect_extended_caps(ac97_state_t* state) {
    state->ext_status = ac97_read_nam(state, AC97_NAM_EXT_STATUS);
    state->vra_supported = (state->ext_status & AC97_EXT_STATUS_VRA) != 0;
    state->ndra_supported = (state->ext_status & AC97_EXT_STATUS_NDRA) != 0;

    debuglog(DEBUG_INFO, "AC97: Ext status 0x%04X VRA=%d NDRA=%d\n",
             state->ext_status, state->vra_supported, state->ndra_supported);

    uint16_t caps = ac97_read_nam(state, AC97_NAM_EXT_STATUS);
    if (caps & AC97_EXT_STATUS_SPSR_48K) debuglog(DEBUG_INFO, "AC97: 48kHz rate supported\n");
    if (caps & AC97_EXT_STATUS_SPSR_44K) debuglog(DEBUG_INFO, "AC97: 44.1kHz rate supported\n");
    if (caps & AC97_EXT_STATUS_SPSR_32K) debuglog(DEBUG_INFO, "AC97: 32kHz rate supported\n");
}

static bool ac97_init_codec(ac97_state_t* state) {
    debuglog(DEBUG_INFO, "AC97: Codec init...\n");

    uint16_t pci_cmd = pci_config_read16(state->pci.segment, state->pci.bus,
                                          state->pci.device, state->pci.function, 0x04);
    pci_cmd |= 0x0005;
    pci_config_write16(state->pci.segment, state->pci.bus,
                       state->pci.device, state->pci.function, 0x04, pci_cmd);

    ac97_power_up(state);

    ac97_write_nam(state, AC97_NAM_RESET, 0);
    for (volatile int i = 0; i < 5000; i++) { __asm__ volatile("nop"); }

    uint32_t ctrl = ac97_read_nabm32(state, AC97_NABM_GLOBAL_CTRL);
    ctrl |= AC97_GLOBAL_CTRL_COLD_RST;
    ac97_write_nabm32(state, AC97_NABM_GLOBAL_CTRL, ctrl);
    for (volatile int i = 0; i < 10000; i++) { __asm__ volatile("nop"); }
    ctrl |= AC97_GLOBAL_CTRL_RUN;
    ac97_write_nabm32(state, AC97_NABM_GLOBAL_CTRL, ctrl);
    for (volatile int i = 0; i < 10000; i++) { __asm__ volatile("nop"); }

    if (!ac97_verify_codec(state)) {
        debuglog(DEBUG_ERROR, "AC97: Codec verification failed\n");
        return false;
    }

    ac97_detect_extended_caps(state);

    return true;
}

static bool ac97_allocate_dma(ac97_state_t* state) {
    state->dma_buffer_size = 8192;
    state->num_pages = (state->dma_buffer_size + 4095) / 4096;

    uint32_t frame = bitmap_pmm_alloc_pages(state->num_pages, PMM_ALLOC_LOW_MEMORY);
    if (frame == 0 || frame == (uint32_t)-1) {
        debuglog(DEBUG_ERROR, "AC97: DMA alloc failed\n");
        return false;
    }

    state->dma_buffer_phys = frame * 4096;
    state->dma_buffer_virt = (uint8_t*)mm_map_physical_page(state->dma_buffer_phys, 0);
    state->buffer_frame = frame;

    if (!state->dma_buffer_virt) {
        debuglog(DEBUG_ERROR, "AC97: DMA map failed\n");
        bitmap_pmm_free_pages(frame, state->num_pages);
        return false;
    }

    memset(state->dma_buffer_virt, 0, state->dma_buffer_size);

    uint32_t bdl_frame = bitmap_pmm_alloc_pages(1, PMM_ALLOC_LOW_MEMORY);
    if (bdl_frame == 0 || bdl_frame == (uint32_t)-1) {
        debuglog(DEBUG_ERROR, "AC97: BDL alloc failed\n");
        return false;
    }

    state->bdl_phys = bdl_frame * 4096;
    state->bdl = (ac97_bdl_entry_t*)mm_map_physical_page(state->bdl_phys, 0);
    state->bdl_frame = bdl_frame;

    if (!state->bdl) {
        debuglog(DEBUG_ERROR, "AC97: BDL map failed\n");
        bitmap_pmm_free_pages(bdl_frame, 1);
        return false;
    }

    memset(state->bdl, 0, sizeof(ac97_bdl_entry_t) * AC97_NUM_BUFFERS);
    debuglog(DEBUG_INFO, "AC97: DMA buf phys=0x%08X virt=0x%08X size=%u BDL phys=0x%08X\n",
             state->dma_buffer_phys, (uint32_t)state->dma_buffer_virt,
             state->dma_buffer_size, state->bdl_phys);

    return true;
}

static void ac97_program_bdl(ac97_state_t* state) {
    uint32_t half = state->dma_buffer_size / AC97_NUM_BUFFERS;

    for (int i = 0; i < AC97_NUM_BUFFERS; i++) {
        state->bdl[i].phys_addr = state->dma_buffer_phys + i * half;
        state->bdl[i].length = (uint16_t)half;
        state->bdl[i].flags = 0x8000;
    }

    ac97_write_nabm32(state, AC97_NABM_PO_BDBAR, state->bdl_phys);
}

static void ac97_reset_channel(ac97_state_t* state) {
    ac97_write_nabm8(state, AC97_NABM_PO_CR, AC97_PO_CR_RESET);
    for (volatile int i = 0; i < 10000; i++) { __asm__ volatile("nop"); }
    ac97_write_nabm8(state, AC97_NABM_PO_CR, 0);
    for (volatile int i = 0; i < 10000; i++) { __asm__ volatile("nop"); }
}

static uint32_t ac97_rate_to_reg(uint32_t rate) {
    if (rate > 48000) rate = 48000;
    if (rate < 4000) rate = 4000;
    return rate;
}

static bool ac97_set_rate(ac97_state_t* state, uint32_t rate) {
    if (rate > 48000) rate = 48000;
    if (rate < 4000) rate = 4000;

    if (rate == 48000) {
        state->native_rate = 48000;
        state->current_sample_rate = 48000;
        ac97_write_nam(state, AC97_NAM_FRONT_DAC_RATE, 48000);
        return true;
    }

    if (state->vra_supported) {
        uint16_t ext_ctrl = ac97_read_nam(state, AC97_NAM_EXT_AUDIO_CTRL);
        ext_ctrl |= AC97_VRA_BIT;
        ac97_write_nam(state, AC97_NAM_EXT_AUDIO_CTRL, ext_ctrl);

        ac97_write_nam(state, AC97_NAM_FRONT_DAC_RATE, (uint16_t)rate);
        for (volatile int i = 0; i < 2000; i++) { __asm__ volatile("nop"); }

        uint16_t actual = ac97_read_nam(state, AC97_NAM_FRONT_DAC_RATE);
        debuglog(DEBUG_INFO, "AC97: Requested rate %u, actual %u\n", rate, actual);

        if (actual != 0 && actual != 0xFFFF) {
            state->native_rate = actual;
            state->current_sample_rate = rate;
            return true;
        }
    }

    state->native_rate = 48000;
    state->current_sample_rate = 48000;
    ac97_write_nam(state, AC97_NAM_FRONT_DAC_RATE, 48000);
    debuglog(DEBUG_INFO, "AC97: VRA not available, falling back to 48kHz\n");
    return false;
}

static uint32_t ac97_resample_mono_s16(const int16_t* src, uint32_t src_frames,
                                        int16_t* dst, uint32_t dst_frames,
                                        uint32_t src_rate, uint32_t dst_rate) {
    uint32_t out = 0;
    for (uint32_t i = 0; i < dst_frames && out < dst_frames; i++) {
        uint64_t pos = (uint64_t)i * src_rate;
        uint32_t idx = (uint32_t)(pos / dst_rate);
        uint32_t frac = (uint32_t)(pos % dst_rate);

        if (idx >= src_frames) break;

        int32_t s0 = src[idx];
        int32_t s1 = (idx + 1 < src_frames) ? src[idx + 1] : s0;
        int32_t result = s0 + (int32_t)(((int64_t)(s1 - s0) * (int64_t)frac) / (int64_t)dst_rate);

        if (result > 32767) result = 32767;
        if (result < -32768) result = -32768;
        dst[out++] = (int16_t)result;
    }
    return out;
}

static uint32_t ac97_resample_stereo_s16(const int16_t* src, uint32_t src_frames,
                                          int16_t* dst, uint32_t dst_frames,
                                          uint32_t src_rate, uint32_t dst_rate) {
    uint32_t out = 0;
    for (uint32_t i = 0; i < dst_frames && out < dst_frames; i++) {
        uint64_t pos = (uint64_t)i * src_rate;
        uint32_t idx = (uint32_t)(pos / dst_rate);
        uint32_t frac = (uint32_t)(pos % dst_rate);

        if (idx >= src_frames) break;

        for (int ch = 0; ch < 2; ch++) {
            int32_t s0 = src[idx * 2 + ch];
            int32_t s1 = (idx + 1 < src_frames) ? src[(idx + 1) * 2 + ch] : s0;
            int32_t result = s0 + (int32_t)(((int64_t)(s1 - s0) * (int64_t)frac) / (int64_t)dst_rate);
            if (result > 32767) result = 32767;
            if (result < -32768) result = -32768;
            dst[out * 2 + ch] = (int16_t)result;
        }
        out++;
    }
    return out;
}

static bool ac97_init(SoundDriver* driver) {
    if (!driver || !driver->state) return false;
    ac97_state_t* state = (ac97_state_t*)driver->state;

    debuglog(DEBUG_INFO, "AC97: Init...\n");

    if (!ac97_init_codec(state)) {
        debuglog(DEBUG_ERROR, "AC97: Codec init failed\n");
        return false;
    }

    if (!ac97_allocate_dma(state)) {
        debuglog(DEBUG_ERROR, "AC97: DMA alloc failed\n");
        return false;
    }

    state->irq = pci_config_read8(state->pci.segment, state->pci.bus,
                                   state->pci.device, state->pci.function, 0x3C);
    debuglog(DEBUG_INFO, "AC97: IRQ %u\n", state->irq);

    if (state->irq != 0xFF && state->irq != 0) {
        interrupt_set_handler_legacy(0x20 + state->irq, ac97_interrupt_handler);
        debuglog(DEBUG_INFO, "AC97: IRQ handler registered on IRQ %u\n", state->irq);
    }

    ac97_program_bdl(state);
    ac97_reset_channel(state);

    ac97_set_rate(state, 48000);

    ac97_write_nam(state, AC97_NAM_MASTER_VOL, 0x0000);
    ac97_write_nam(state, AC97_NAM_HP_VOL, 0x0000);
    ac97_write_nam(state, AC97_NAM_PCM_VOL, 0x0808);
    state->master_vol = 31;
    state->hp_vol = 31;
    state->pcm_vol = 8;
    state->master_muted = false;
    state->hp_muted = false;
    state->pcm_muted = false;

    uint32_t ctrl = ac97_read_nabm32(state, AC97_NABM_GLOBAL_CTRL);
    ctrl |= AC97_GLOBAL_CTRL_SRIE | AC97_GLOBAL_CTRL_PIIE;
    ac97_write_nabm32(state, AC97_NABM_GLOBAL_CTRL, ctrl);

    ac97_write_nabm8(state, AC97_NABM_PO_CR, 0);
    ac97_write_nabm32(state, AC97_NABM_PO_SR, 0x1C);

    state->active_buffer = 0;
    state->buffer_done[0] = false;
    state->buffer_done[1] = false;
    state->dma_active = false;
    state->initialized = true;

    debuglog(DEBUG_INFO, "AC97: Driver ready, codec=0x%04X:0x%04X\n", state->vendor_id1, state->vendor_id2);
    return true;
}

static bool ac97_play_pcm(SoundDriver* driver, const uint8_t* data, uint32_t length, const SoundFormat* format) {
    if (!driver || !driver->state || !data || !format || length == 0) return false;
    ac97_state_t* state = (ac97_state_t*)driver->state;
    if (!state->initialized) return false;

    if (state->dma_active) {
        ac97_write_nabm8(state, AC97_NABM_PO_CR, 0);
        for (volatile int i = 0; i < 1000; i++) { __asm__ volatile("nop"); }
        state->dma_active = false;
        g_ac97_streaming = false;
    }

    bool rate_ok = ac97_set_rate(state, format->sample_rate);

    uint8_t* play_buf = state->dma_buffer_virt;
    uint32_t play_len = length;

    if (!rate_ok && format->sample_rate != 48000 && format->sample_rate != 0) {
        uint32_t src_frames = length / (format->bits_per_sample / 8 * format->channels);
        uint32_t dst_frames = (src_frames * 48000 + format->sample_rate - 1) / format->sample_rate;
        uint32_t dst_bytes = dst_frames * (format->bits_per_sample / 8) * format->channels;

        if (dst_bytes <= state->dma_buffer_size) {
            if (format->bits_per_sample == 16 && format->channels == 2) {
                dst_frames = ac97_resample_stereo_s16((const int16_t*)data, src_frames,
                                                       (int16_t*)state->dma_buffer_virt, dst_frames,
                                                       format->sample_rate, 48000);
            } else if (format->bits_per_sample == 16 && format->channels == 1) {
                dst_frames = ac97_resample_mono_s16((const int16_t*)data, src_frames,
                                                     (int16_t*)state->dma_buffer_virt, dst_frames,
                                                     format->sample_rate, 48000);
            } else {
                memcpy(state->dma_buffer_virt, data, MIN(length, state->dma_buffer_size));
                dst_bytes = length;
            }
            play_buf = state->dma_buffer_virt;
            play_len = dst_frames * (format->bits_per_sample / 8) * format->channels;
            debuglog(DEBUG_INFO, "AC97: Resampled %u->%u frames for 48kHz\n", src_frames, dst_frames);
        } else {
            memcpy(state->dma_buffer_virt, data, state->dma_buffer_size);
            play_len = state->dma_buffer_size;
        }
    } else {
        uint32_t copy = MIN(length, state->dma_buffer_size);
        memcpy(state->dma_buffer_virt, data, copy);
        play_len = copy;
    }

    if (play_len > state->dma_buffer_size) play_len = state->dma_buffer_size;

    uint32_t half = state->dma_buffer_size / AC97_NUM_BUFFERS;
    if (play_len <= half) {
        state->bdl[0].length = (uint16_t)play_len;
        state->bdl[0].flags = 0x8000;
        state->bdl[1].length = 0;
        state->bdl[1].flags = 0x8000;
    } else {
        state->bdl[0].length = (uint16_t)half;
        state->bdl[0].flags = 0x8000;
        state->bdl[1].length = (uint16_t)(play_len - half);
        state->bdl[1].flags = 0x8000;
    }

    state->active_buffer = 0;
    state->buffer_done[0] = false;
    state->buffer_done[1] = false;

    ac97_write_nabm32(state, AC97_NABM_PO_BDBAR, state->bdl_phys);

    uint8_t lvi = (play_len <= half) ? 0 : 1;
    ac97_write_nabm8(state, AC97_NABM_PO_CIV, 0);
    ac97_write_nabm32(state, AC97_NABM_PO_SR, 0x1C);
    ac97_write_nabm8(state, AC97_NABM_PO_LVI, lvi);

    uint8_t cr = AC97_PO_CR_RUN | AC97_PO_CR_IOCE | AC97_PO_CR_FEIE;
    ac97_write_nabm8(state, AC97_NABM_PO_CR, cr);

    state->dma_active = true;
    g_ac97_streaming = true;

    debuglog(DEBUG_INFO, "AC97: Playing %u bytes, rate=%u, lvi=%u\n", play_len, format->sample_rate, lvi);
    return true;
}

static void ac97_set_master_vol(ac97_state_t* state, uint8_t volume) {
    uint8_t atten = (uint8_t)((uint32_t)(255 - volume) * AC97_VOL_MAX / 255);
    uint16_t reg = ((uint16_t)atten << 8) | atten;
    if (state->master_muted) reg |= AC97_VOL_MUTE;
    ac97_write_nam(state, AC97_NAM_MASTER_VOL, reg);
    state->master_vol = atten;
}

static void ac97_set_hp_vol(ac97_state_t* state, uint8_t volume) {
    uint8_t atten = (uint8_t)((uint32_t)(255 - volume) * AC97_VOL_MAX / 255);
    uint16_t reg = ((uint16_t)atten << 8) | atten;
    if (state->hp_muted) reg |= AC97_VOL_MUTE;
    ac97_write_nam(state, AC97_NAM_HP_VOL, reg);
    state->hp_vol = atten;
}

static void ac97_set_pcm_vol(ac97_state_t* state, uint8_t volume) {
    uint8_t atten = (uint8_t)((uint32_t)(255 - volume) * 15 / 255);
    uint16_t reg = ((uint16_t)atten << 8) | atten;
    if (state->pcm_muted) reg |= AC97_VOL_MUTE;
    ac97_write_nam(state, AC97_NAM_PCM_VOL, reg);
    state->pcm_vol = atten;
}

static void ac97_set_volume(SoundDriver* driver, uint8_t volume) {
    if (!driver || !driver->state) return;
    ac97_state_t* state = (ac97_state_t*)driver->state;
    if (!state->initialized) return;

    ac97_set_master_vol(state, volume);
    ac97_set_hp_vol(state, volume);
    ac97_set_pcm_vol(state, volume);
}

static void ac97_set_mute(ac97_state_t* state, bool muted) {
    state->master_muted = muted;
    state->hp_muted = muted;
    state->pcm_muted = muted;

    uint16_t master = ((uint16_t)state->master_vol << 8) | state->master_vol;
    uint16_t hp = ((uint16_t)state->hp_vol << 8) | state->hp_vol;
    uint16_t pcm = ((uint16_t)state->pcm_vol << 8) | state->pcm_vol;

    if (muted) {
        master |= AC97_VOL_MUTE;
        hp |= AC97_VOL_MUTE;
        pcm |= AC97_VOL_MUTE;
    }

    ac97_write_nam(state, AC97_NAM_MASTER_VOL, master);
    ac97_write_nam(state, AC97_NAM_HP_VOL, hp);
    ac97_write_nam(state, AC97_NAM_PCM_VOL, pcm);
}

static void ac97_beep(SoundDriver* driver, uint32_t frequency_hz, uint32_t duration_ms) {
    if (!driver || !driver->state) return;
    ac97_state_t* state = (ac97_state_t*)driver->state;
    if (!state->initialized || !state->dma_buffer_virt) return;
    if (frequency_hz == 0 || frequency_hz > 20000) return;

    uint32_t rate = frequency_hz * 2;
    if (rate > 48000) rate = 48000;
    if (rate < 4000) rate = 4000;

    ac97_set_rate(state, rate);

    uint32_t samples = (rate * duration_ms) / 1000;
    uint32_t bytes = samples * 4;
    if (bytes > state->dma_buffer_size) {
        samples = state->dma_buffer_size / 4;
        bytes = samples * 4;
    }

    memset(state->dma_buffer_virt, 0, state->dma_buffer_size);
    int16_t* buf = (int16_t*)state->dma_buffer_virt;
    for (uint32_t i = 0; i < samples; i++) {
        int16_t val = ((i / (rate / (frequency_hz * 2))) % 2) ? 4096 : -4096;
        buf[i * 2] = val;
        buf[i * 2 + 1] = val;
    }

    uint32_t half = state->dma_buffer_size / AC97_NUM_BUFFERS;
    uint32_t play = (bytes < half) ? bytes : half;
    state->bdl[0].length = (uint16_t)play;
    state->bdl[0].flags = 0x8000;
    state->bdl[1].length = 0;
    state->bdl[1].flags = 0x8000;

    ac97_write_nabm32(state, AC97_NABM_PO_BDBAR, state->bdl_phys);
    ac97_write_nabm8(state, AC97_NABM_PO_CIV, 0);
    ac97_write_nabm32(state, AC97_NABM_PO_SR, 0x1C);
    ac97_write_nabm8(state, AC97_NABM_PO_LVI, 0);
    ac97_write_nabm8(state, AC97_NABM_PO_CR, AC97_PO_CR_RUN | AC97_PO_CR_IOCE);

    uint32_t start = timer_get_ticks();
    state->dma_active = true;
    state->buffer_done[0] = false;

    while (state->dma_active && (timer_get_ticks() - start) < (duration_ms / 10) + 50) {
        if (state->buffer_done[0]) {
            state->dma_active = false;
            break;
        }
        timer_sleep_ms(1);
    }

    ac97_write_nabm8(state, AC97_NABM_PO_CR, 0);
    state->dma_active = false;
    g_ac97_streaming = false;
}

static bool ac97_get_capabilities(SoundDriver* driver, DeviceCapabilities* caps) {
    if (!driver || !driver->state || !caps) return false;
    ac97_state_t* state = (ac97_state_t*)driver->state;
    if (!state->initialized) return false;

    memset(caps, 0, sizeof(DeviceCapabilities));
    caps->supported_formats[0] = PCM_S16;
    caps->supported_formats[1] = PCM_U8;
    caps->max_channels = 2;
    caps->stereo_supported = true;
    caps->little_endian = true;
    caps->native_sample_rates[0] = 48000;
    if (state->vra_supported) {
        caps->native_sample_rates[1] = 44100;
        caps->native_sample_rates[2] = 22050;
        caps->native_sample_rates[3] = 11025;
    } else {
        caps->native_sample_rates[1] = 0;
    }
    caps->max_buffer_size = state->dma_buffer_size;

    return true;
}

static void ac97_shutdown(SoundDriver* driver) {
    if (!driver || !driver->state) return;
    ac97_state_t* state = (ac97_state_t*)driver->state;
    if (!state->initialized) return;

    if (state->dma_active) {
        ac97_write_nabm8(state, AC97_NABM_PO_CR, 0);
        for (volatile int i = 0; i < 5000; i++) { __asm__ volatile("nop"); }
        state->dma_active = false;
    }

    g_ac97_streaming = false;

    if (state->irq != 0xFF && state->irq != 0) {
        interrupt_clear_handler(0x20 + state->irq);
    }

    ac97_write_nam(state, AC97_NAM_MASTER_VOL, AC97_VOL_MUTE);
    ac97_write_nam(state, AC97_NAM_HP_VOL, AC97_VOL_MUTE);
    ac97_write_nam(state, AC97_NAM_PCM_VOL, AC97_VOL_MUTE);

    ac97_power_down(state);

    if (state->dma_buffer_virt) {
        if (state->buffer_frame != 0 && state->num_pages > 0) {
            bitmap_pmm_free_pages(state->buffer_frame, state->num_pages);
        }
        state->dma_buffer_virt = 0;
    }
    state->dma_buffer_phys = 0;
    state->dma_buffer_size = 0;

    if (state->bdl) {
        if (state->bdl_frame != 0) {
            bitmap_pmm_free_pages(state->bdl_frame, 1);
        }
        state->bdl = 0;
    }
    state->bdl_phys = 0;

    state->initialized = false;
    debuglog(DEBUG_INFO, "AC97: Shutdown complete\n");
}

static SoundDriver g_ac97_driver = {
    .name = "AC'97",
    .type = SOUND_DEVICE_AC97,
    .detect = ac97_detect,
    .init = ac97_init,
    .play_pcm = ac97_play_pcm,
    .get_capabilities = ac97_get_capabilities,
    .set_volume = ac97_set_volume,
    .beep = ac97_beep,
    .shutdown = ac97_shutdown,
    .state = 0,
    .volume = 255
};

SoundDriver* sound_ac97_driver(void) {
    return &g_ac97_driver;
}
