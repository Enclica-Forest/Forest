#include "include/sound.h"
#include "include/pci.h"
#include "include/io_ports.h"
#include "include/screen.h"
#include "include/libc/string.h"
#include "include/debuglog.h"
#include "include/memory.h"
#include "include/bitmap_pmm.h"
#include "include/interrupt.h"
#include "include/device_fs.h"

#define HDA_REG_GCAP       0x00
#define HDA_REG_VMIN       0x02
#define HDA_REG_VMAJ       0x03
#define HDA_REG_OUTPAY     0x04
#define HDA_REG_INPAY      0x06
#define HDA_REG_GCTL       0x08
#define HDA_REG_WAKEEN     0x0C
#define HDA_REG_STATESTS   0x0E
#define HDA_REG_GSTS       0x10
#define HDA_REG_OUTSTRMPAY 0x18
#define HDA_REG_INSTRMPAY  0x1A
#define HDA_REG_INTCTL     0x20
#define HDA_REG_INTSTS     0x24
#define HDA_REG_WALCLK     0x30
#define HDA_REG_SSYNC      0x34
#define HDA_REG_CORBLBASE  0x40
#define HDA_REG_CORBUBASE  0x44
#define HDA_REG_CORBWP     0x48
#define HDA_REG_CORBRP     0x4A
#define HDA_REG_CORBCTL    0x4C
#define HDA_REG_CORBSTS    0x4D
#define HDA_REG_CORBSIZE   0x4E
#define HDA_REG_RIRBLBASE  0x50
#define HDA_REG_RIRBUBASE  0x54
#define HDA_REG_RIRBWP     0x58
#define HDA_REG_RINTCNT    0x5A
#define HDA_REG_RIRBCTL    0x5C
#define HDA_REG_RIRBSTS    0x5D
#define HDA_REG_RIRBSIZE   0x5E
#define HDA_REG_DPLBASE    0x70
#define HDA_REG_DPUBASE    0x74
#define HDA_REG_ICW        0x68
#define HDA_REG_IRR        0x6C
#define HDA_REG_ICS        0x70

#define HDA_GCTL_CRST      (1u << 0)
#define HDA_GCTL_FCNTRL    (1u << 1)
#define HDA_GCTL_UNSOL     (1u << 8)

#define HDA_CORBCTL_RUN    (1u << 1)
#define HDA_CORBCTL_DMA    (1u << 0)
#define HDA_RIRBCTL_RUN    (1u << 1)
#define HDA_RIRBCTL_INT_EN (1u << 0)
#define HDA_RIRBCTL_DMA_EN (1u << 2)

#define HDA_CORBSIZE_2B    0x01
#define HDA_CORBSIZE_16B   0x10
#define HDA_CORBSIZE_256B  0x02
#define HDA_RIRBSIZE_2B    0x01
#define HDA_RIRBSIZE_16B   0x10
#define HDA_RIRBSIZE_256B  0x02

#define HDA_SD_BASE        0x80
#define HDA_SD_SIZE        0x20

#define HDA_SD_CTL0        0x00
#define HDA_SD_CTL1        0x01
#define HDA_SD_CTL2        0x02
#define HDA_SD_STATUS      0x03
#define HDA_SD_LPIB        0x04
#define HDA_SD_CBL         0x08
#define HDA_SD_LVI         0x0C
#define HDA_SD_FIFOD       0x0E
#define HDA_SD_FMT         0x12
#define HDA_SD_BDPL        0x18
#define HDA_SD_BDPU        0x1C

#define HDA_SD_CTL_RUN     (1u << 1)
#define HDA_SD_CTL_SRST    (1u << 0)
#define HDA_SD_CTL_IOCE    (1u << 2)
#define HDA_SD_CTL_FEIE    (1u << 3)
#define HDA_SD_CTL_DEIE    (1u << 4)

#define HDA_SD_STAT_BCE    (1u << 2)
#define HDA_SD_STAT_FIFOE  (1u << 3)
#define HDA_SD_STAT_DESC  (1u << 4)

#define HDA_INT_GLOBAL     (1u << 31)
#define HDA_INT_CONTROLLER  (1u << 0)

#define HDA_VERB_SHORT     0
#define HDA_VERB_LONG      1

#define HDA_GET_PARAMETER  0xF00
#define HDA_SET_CONNECT_SEL 0x701
#define HDA_SET_PIN_WIDGET_CONTROL 0x707
#define HDA_SET_PIN_SENSE  0x709
#define HDA_SET_CONVERTER_FORMAT 0x200
#define HDA_SET_CONVERTER_STREAM 0x706
#define HDA_SET_STREAM_FORMAT 0x200
#define HDA_SET_PROCESSING_COEFF 0x500
#define HDA_GET_PROCESSING_COEFF 0xD00
#define HDA_SET_COEFFICIENT_INDEX 0x500

#define HDA_PARAM_NODE_COUNT   0x04
#define HDA_PARAM_AUDIO_WIDGET_CAP 0x0D
#define HDA_PARAM_PIN_CAP      0x0C
#define HDA_PARAM_CONV_FMT_CAP 0x0A
#define HDA_PARAM_STREAM_FORMATS 0x0B
#define HDA_PARAM_SUBNODE_COUNT 0x04
#define HDA_PARAM_OVERRIDE_PIN_CAP 0x11
#define HDA_PARAM_SUPPORTED_SIZE_RATE 0x0A
#define HDA_PARAM_AMP_IN_CAP   0x09

#define HDA_WIDGET_TYPE_OUTPUT  0x00
#define HDA_WIDGET_TYPE_INPUT   0x01
#define HDA_WIDGET_TYPE_PIN     0x04
#define HDA_WIDGET_TYPE_MIXER   0x05
#define HDA_WIDGET_TYPE_SELECTOR 0x06
#define HDA_WIDGET_TYPE_POWER   0x07

#define HDA_PIN_CAP_OUT  (1u << 4)
#define HDA_PIN_CAP_HP   (1u << 10)
#define HDA_PIN_CAP_EAPD (1u << 16)

#define HDA_PIN_CONTROL_OUT_EN  (1u << 6)
#define HDA_PIN_CONTROL_HP_EN   (1u << 7)

#define HDA_POWER_STATE_D0  0x00
#define HDA_POWER_STATE_D1  0x01
#define HDA_POWER_STATE_D2  0x02
#define HDA_POWER_STATE_D3  0x03

#define HDA_BDL_MAX_ENTRIES 256
#define HDA_DMA_BUFFER_SIZE (64 * 1024)
#define HDA_BUFFER_PERIODS  4
#define HDA_MAX_OUTPUT_STREAMS 4
#define HDA_MAX_CODECS 15
#define HDA_MAX_WIDGETS 256
#define HDA_CORB_ENTRIES 256
#define HDA_RIRB_ENTRIES 256

#define HDA_VENDOR_INTEL    0x8086

typedef struct {
    uint32_t addr;
    uint32_t length;
    uint32_t ioc;
    uint32_t reserved;
} hda_bdl_entry_t;

typedef struct {
    uint8_t codec_addr;
    uint8_t node_id;
    uint8_t widget_type;
    uint8_t pin_default_config;
    uint32_t widget_cap;
    uint32_t pin_cap;
    uint32_t conv_fmt_cap;
    bool present;
    bool configured;
    uint8_t power_state;
    uint8_t stream_tag;
} hda_widget_t;

typedef struct {
    uint8_t codec_addr;
    bool present;
    uint16_t vendor_id;
    uint16_t revision_id;
    uint8_t start_node;
    uint8_t num_nodes;
    hda_widget_t output_widgets[32];
    uint8_t num_outputs;
    hda_widget_t pin_widgets[32];
    uint8_t num_pins;
} hda_codec_t;

typedef struct {
    volatile uint32* mmio;
    uint8_t stream_tag;
    uint8_t codec_addr;
    uint8_t output_node;
    uint8_t pin_node;
    uint8_t converter_node;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint32_t dma_buffer_phys;
    int16_t* dma_buffer_virt;
    uint32_t dma_buffer_size;
    hda_bdl_entry_t* bdl_virt;
    uint32_t bdl_phys;
    uint32_t bdl_frame;
    uint32_t dma_frame;
    uint32_t dma_pages;
    bool active;
    bool allocated;
} hda_stream_t;

typedef struct {
    pci_device_t pci;
    volatile uint32* mmio;
    bool initialized;

    uint16_t gcap;
    uint8_t num_output_streams;
    uint8_t num_input_streams;
    uint8_t num_bidirectional;
    uint8_t gcap_ver;
    uint8_t output_pay;
    uint8_t input_pay;

    uint32_t* corb_virt;
    uint32_t corb_phys;
    uint32_t corb_frame;
    uint32_t corb_write;

    uint64_t* rirb_virt;
    uint32_t rirb_phys;
    uint32_t rirb_frame;
    uint32_t rirb_read;

    hda_codec_t codecs[HDA_MAX_CODECS];
    uint8_t num_codecs;

    hda_widget_t widgets[HDA_MAX_WIDGETS];
    uint8_t num_widgets;

    hda_stream_t streams[HDA_MAX_OUTPUT_STREAMS];
    uint8_t next_stream_tag;
    uint8_t active_streams;

    uint32_t default_sample_rate;
    uint8_t default_channels;
    uint8_t default_bits_per_sample;

    uint8_t irq_line;
    volatile bool interrupt_fired;
} hda_state_t;

static hda_state_t* g_hda_state = 0;

static inline void hda_write(hda_state_t* state, uint16_t offset, uint32_t value) {
    state->mmio[offset / 4] = value;
}

static inline uint32_t hda_read(hda_state_t* state, uint16_t offset) {
    return state->mmio[offset / 4];
}

static inline void hda_write8(hda_state_t* state, uint16_t offset, uint8_t value) {
    *(volatile uint8_t*)((uint8_t*)state->mmio + offset) = value;
}

static inline uint8_t hda_read8(hda_state_t* state, uint16_t offset) {
    return *(volatile uint8_t*)((uint8_t*)state->mmio + offset);
}

static inline void hda_write16(hda_state_t* state, uint16_t offset, uint16_t value) {
    *(volatile uint16_t*)((uint8_t*)state->mmio + offset) = value;
}

static inline uint16_t hda_read16(hda_state_t* state, uint16_t offset) {
    return *(volatile uint16_t*)((uint8_t*)state->mmio + offset);
}

static inline void hda_write32(hda_state_t* state, uint16_t offset, uint32_t value) {
    *(volatile uint32_t*)((uint8_t*)state->mmio + offset) = value;
}

static inline uint32_t hda_read32(hda_state_t* state, uint16_t offset) {
    return *(volatile uint32_t*)((uint8_t*)state->mmio + offset);
}

static void hda_delay_us(uint32_t us) {
    for (uint32_t i = 0; i < us * 10; i++) {
        __asm__ volatile("nop");
    }
}

static bool hda_corb_send_verb(hda_state_t* state, uint32_t verb) {
    uint32_t rp = (hda_read(state, HDA_REG_CORBRP) >> 4) & 0xFF;
    uint32_t wp = state->corb_write & 0xFF;

    uint32_t next_wp = (wp + 1) & 0xFF;
    if (next_wp == rp) {
        return false;
    }

    state->corb_virt[wp] = verb;
    state->corb_write = next_wp;
    hda_write16(state, HDA_REG_CORBWP, next_wp << 4);

    for (uint32_t i = 0; i < 1000; i++) {
        uint32_t cur_rp = (hda_read(state, HDA_REG_CORBRP) >> 4) & 0xFF;
        uint32_t cur_wp = (hda_read(state, HDA_REG_CORBWP) >> 4) & 0xFF;
        if (cur_rp == cur_wp) {
            return true;
        }
        hda_delay_us(10);
    }
    return false;
}

static uint32_t hda_rirb_read_response(hda_state_t* state) {
    uint32_t wp = (hda_read(state, HDA_REG_RIRBWP) >> 4) & 0xFF;
    uint32_t timeout = 1000;

    while (state->rirb_read == wp && timeout > 0) {
        hda_delay_us(10);
        wp = (hda_read(state, HDA_REG_RIRBWP) >> 4) & 0xFF;
        timeout--;
    }

    if (state->rirb_read == wp) {
        return 0;
    }

    uint32_t response = (uint32_t)state->rirb_virt[state->rirb_read];
    state->rirb_read = (state->rirb_read + 1) % HDA_RIRB_ENTRIES;

    return response;
}

static uint32_t hda_send_verb(hda_state_t* state, uint8_t codec_addr, uint16_t node_id, uint16_t verb_id, uint8_t param) {
    uint32_t verb = ((uint32_t)codec_addr << 28) |
                    ((uint32_t)node_id << 20) |
                    ((uint32_t)verb_id << 8) |
                    (uint32_t)param;

    if (!hda_corb_send_verb(state, verb)) {
        return 0;
    }

    return hda_rirb_read_response(state);
}

static uint32_t hda_get_parameter(hda_state_t* state, uint8_t codec_addr, uint8_t node_id, uint8_t param) {
    return hda_send_verb(state, codec_addr, node_id, HDA_GET_PARAMETER, param);
}

static bool hda_allocate_buffer(hda_state_t* state, hda_stream_t* stream, uint32_t size) {
    uint32_t num_pages = (size + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE;
    uint32_t frame = bitmap_pmm_alloc_pages(num_pages, PMM_ALLOC_LOW_MEMORY);
    if (frame == 0) {
        debuglog(DEBUG_ERROR, "HDA: Failed to allocate DMA buffer\n");
        return false;
    }

    uint32_t phys = frame * MEMORY_PAGE_SIZE;
    uintptr_t virt = (uintptr_t)0xF0000000u + (uintptr_t)phys;
    memory_result_t map_result = MEMORY_OK;

    for (uint32_t i = 0; i < num_pages; i++) {
        memory_result_t result = vmm_map_page(vmm_get_current_page_directory(),
                                              (uint32_t)(virt + (uintptr_t)i * MEMORY_PAGE_SIZE),
                                              frame + i,
                                              PAGE_PRESENT | PAGE_WRITABLE);
        if (result != MEMORY_OK) {
            map_result = result;
            break;
        }
    }

    if (map_result != MEMORY_OK) {
        bitmap_pmm_free_pages(frame, num_pages);
        return false;
    }

    stream->dma_buffer_phys = phys;
    stream->dma_buffer_virt = (int16_t*)virt;
    stream->dma_buffer_size = size;
    stream->dma_frame = frame;
    stream->dma_pages = num_pages;

    memset(stream->dma_buffer_virt, 0, size);
    return true;
}

static bool hda_allocate_bdl(hda_state_t* state, hda_stream_t* stream) {
    uint32_t bdl_frame = bitmap_pmm_alloc_page(PMM_ALLOC_LOW_MEMORY);
    if (bdl_frame == 0) {
        return false;
    }

    uint32_t bdl_phys = bdl_frame * MEMORY_PAGE_SIZE;
    uintptr_t bdl_virt = (uintptr_t)0xE0000000u + (uintptr_t)bdl_phys;

    memory_result_t map_result = vmm_map_page(vmm_get_current_page_directory(),
                                              (uint32_t)bdl_virt, bdl_frame,
                                              PAGE_PRESENT | PAGE_WRITABLE);
    if (map_result != MEMORY_OK) {
        bitmap_pmm_free_page(bdl_frame);
        return false;
    }

    stream->bdl_phys = bdl_phys;
    stream->bdl_virt = (hda_bdl_entry_t*)bdl_virt;
    stream->bdl_frame = bdl_frame;

    return true;
}

static void hda_setup_bdl(hda_stream_t* stream) {
    uint32_t period_size = stream->dma_buffer_size / HDA_BUFFER_PERIODS;

    for (int i = 0; i < HDA_BUFFER_PERIODS; i++) {
        stream->bdl_virt[i].addr = stream->dma_buffer_phys + i * period_size;
        stream->bdl_virt[i].length = period_size;
        stream->bdl_virt[i].ioc = 0x80000000;
        stream->bdl_virt[i].reserved = 0;
    }
}

static bool hda_setup_corb_rirb(hda_state_t* state) {
    uint32_t corb_frame = bitmap_pmm_alloc_page(PMM_ALLOC_LOW_MEMORY);
    if (corb_frame == 0) {
        debuglog(DEBUG_ERROR, "HDA: Failed to allocate CORB page\n");
        return false;
    }

    uint32_t corb_phys = corb_frame * MEMORY_PAGE_SIZE;
    uintptr_t corb_virt = (uintptr_t)0xD0000000u + (uintptr_t)corb_phys;

    memory_result_t result = vmm_map_page(vmm_get_current_page_directory(),
                                          (uint32_t)corb_virt, corb_frame,
                                          PAGE_PRESENT | PAGE_WRITABLE);
    if (result != MEMORY_OK) {
        bitmap_pmm_free_page(corb_frame);
        return false;
    }

    state->corb_virt = (uint32_t*)corb_virt;
    state->corb_phys = corb_phys;
    state->corb_frame = corb_frame;
    state->corb_write = 0;

    uint32_t rirb_frame = bitmap_pmm_alloc_page(PMM_ALLOC_LOW_MEMORY);
    if (rirb_frame == 0) {
        debuglog(DEBUG_ERROR, "HDA: Failed to allocate RIRB page\n");
        bitmap_pmm_free_page(corb_frame);
        return false;
    }

    uint32_t rirb_phys = rirb_frame * MEMORY_PAGE_SIZE;
    uintptr_t rirb_virt = (uintptr_t)0xD1000000u + (uintptr_t)rirb_phys;

    result = vmm_map_page(vmm_get_current_page_directory(),
                          (uint32_t)rirb_virt, rirb_frame,
                          PAGE_PRESENT | PAGE_WRITABLE);
    if (result != MEMORY_OK) {
        bitmap_pmm_free_page(rirb_frame);
        bitmap_pmm_free_page(corb_frame);
        return false;
    }

    state->rirb_virt = (uint64_t*)rirb_virt;
    state->rirb_phys = rirb_phys;
    state->rirb_frame = rirb_frame;
    state->rirb_read = 0;

    memset(state->corb_virt, 0, HDA_CORB_ENTRIES * sizeof(uint32_t));
    memset(state->rirb_virt, 0, HDA_RIRB_ENTRIES * sizeof(uint64_t));

    hda_write(state, HDA_REG_CORBLBASE, corb_phys & 0xFFFFFFFF);
    hda_write(state, HDA_REG_CORBUBASE, 0);

    hda_write8(state, HDA_REG_CORBSIZE, HDA_CORBSIZE_256B);
    hda_delay_us(100);

    uint32_t corbctl = hda_read8(state, HDA_REG_CORBCTL);
    corbctl |= HDA_CORBCTL_DMA;
    hda_write8(state, HDA_REG_CORBCTL, corbctl);
    hda_delay_us(100);

    corbctl |= HDA_CORBCTL_RUN;
    hda_write8(state, HDA_REG_CORBCTL, corbctl);
    hda_delay_us(100);

    hda_write(state, HDA_REG_RIRBLBASE, rirb_phys & 0xFFFFFFFF);
    hda_write(state, HDA_REG_RIRBUBASE, 0);

    hda_write8(state, HDA_REG_RIRBSIZE, HDA_RIRBSIZE_256B);
    hda_delay_us(100);

    hda_write16(state, HDA_REG_RINTCNT, HDA_RIRB_ENTRIES / 2);

    uint32_t rirbctl = hda_read8(state, HDA_REG_RIRBCTL);
    rirbctl |= HDA_RIRBCTL_DMA_EN | HDA_RIRBCTL_INT_EN;
    hda_write8(state, HDA_REG_RIRBCTL, rirbctl);
    hda_delay_us(100);

    rirbctl |= HDA_RIRBCTL_RUN;
    hda_write8(state, HDA_REG_RIRBCTL, rirbctl);
    hda_delay_us(100);

    return true;
}

static void hda_parse_codec_nodes(hda_state_t* state, uint8_t codec_addr) {
    hda_codec_t* codec = &state->codecs[state->num_codecs];
    codec->codec_addr = codec_addr;
    codec->present = true;

    uint32_t root_response = hda_get_parameter(state, codec_addr, 0, HDA_PARAM_NODE_COUNT);
    codec->start_node = (root_response >> 16) & 0xFF;
    codec->num_nodes = root_response & 0xFF;

    debuglog(DEBUG_INFO, "HDA: Codec %u: nodes %u-%u\n",
             codec_addr, codec->start_node, codec->start_node + codec->num_nodes - 1);

    codec->num_outputs = 0;
    codec->num_pins = 0;

    for (uint8_t node = codec->start_node;
         node < codec->start_node + codec->num_nodes && state->num_widgets < HDA_MAX_WIDGETS;
         node++) {
        uint32_t caps = hda_get_parameter(state, codec_addr, node, HDA_PARAM_AUDIO_WIDGET_CAP);
        uint8_t widget_type = (caps >> 20) & 0x0F;

        hda_widget_t* w = &state->widgets[state->num_widgets];
        memset(w, 0, sizeof(hda_widget_t));
        w->codec_addr = codec_addr;
        w->node_id = node;
        w->widget_type = widget_type;
        w->widget_cap = caps;
        w->power_state = HDA_POWER_STATE_D3;

        if (widget_type == HDA_WIDGET_TYPE_PIN) {
            w->pin_cap = hda_get_parameter(state, codec_addr, node, HDA_PARAM_PIN_CAP);
            w->pin_default_config = (hda_get_parameter(state, codec_addr, node, 0x0C) >> 16) & 0xFF;

            if (w->pin_cap & (HDA_PIN_CAP_OUT | HDA_PIN_CAP_HP)) {
                codec->pin_widgets[codec->num_pins] = *w;
                codec->num_pins++;
            }
        }

        if (widget_type == HDA_WIDGET_TYPE_OUTPUT) {
            w->conv_fmt_cap = hda_get_parameter(state, codec_addr, node, HDA_PARAM_CONV_FMT_CAP);
            codec->output_widgets[codec->num_outputs] = *w;
            codec->num_outputs++;
        }

        state->num_widgets++;
    }

    debuglog(DEBUG_INFO, "HDA: Codec %u: %u outputs, %u pins\n",
             codec_addr, codec->num_outputs, codec->num_pins);
}

static void hda_detect_codecs(hda_state_t* state) {
    state->num_codecs = 0;

    uint32_t statests = hda_read(state, HDA_REG_STATESTS);

    for (int i = 0; i < 4; i++) {
        if (statests & (1 << i)) {
            debuglog(DEBUG_INFO, "HDA: Codec %d detected via STATESTS\n", i);
            hda_parse_codec_nodes(state, i);
            state->num_codecs++;
        }
    }

    if (state->num_codecs == 0) {
        for (int i = 0; i < 4; i++) {
            hda_write(state, HDA_REG_STATESTS, (1 << i));
            hda_delay_us(100);
            uint32_t response = hda_send_verb(state, i, 0, HDA_GET_PARAMETER, 0x00);
            if (response != 0 && response != 0xFFFFFFFF) {
                hda_parse_codec_nodes(state, i);
                state->num_codecs++;
            }
        }
    }

    debuglog(DEBUG_INFO, "HDA: Found %u codecs\n", state->num_codecs);
}

static bool hda_configure_pin(hda_state_t* state, uint8_t codec_addr, uint8_t pin_node) {
    uint32_t pin_cap = hda_get_parameter(state, codec_addr, pin_node, HDA_PARAM_PIN_CAP);

    hda_send_verb(state, codec_addr, pin_node, HDA_SET_PIN_WIDGET_CONTROL,
                  HDA_PIN_CONTROL_OUT_EN | 0x00);

    if (pin_cap & HDA_PIN_CAP_HP) {
        hda_send_verb(state, codec_addr, pin_node, HDA_SET_PIN_WIDGET_CONTROL,
                      HDA_PIN_CONTROL_OUT_EN | HDA_PIN_CONTROL_HP_EN);
    }

    hda_send_verb(state, codec_addr, pin_node, HDA_SET_CONNECT_SEL, 0);

    return true;
}

static bool hda_configure_converter(hda_state_t* state, hda_stream_t* stream,
                                    uint16_t sample_rate, uint8_t channels, uint8_t bits) {
    uint16_t fmt = 0;

    if (sample_rate >= 44100) {
        fmt |= (1 << 14);
    }

    switch (bits) {
        case 8:  fmt |= 0x00; break;
        case 16: fmt |= (1 << 4); break;
        case 20: fmt |= (2 << 4); break;
        case 24: fmt |= (3 << 4); break;
        case 32: fmt |= (4 << 4); break;
        default: fmt |= (1 << 4); break;
    }

    if (channels > 1) {
        fmt |= (channels - 1) & 0x0F;
    }

    hda_send_verb(state, stream->codec_addr, stream->converter_node,
                  HDA_SET_CONVERTER_FORMAT, 0);
    hda_send_verb(state, stream->codec_addr, stream->converter_node,
                  HDA_SET_CONVERTER_STREAM, (stream->stream_tag << 4) | 0);

    uint16_t verb = (fmt << 8) | 0;
    hda_send_verb(state, stream->codec_addr, stream->converter_node,
                  HDA_SET_CONVERTER_FORMAT, (verb & 0xFF));
    hda_send_verb(state, stream->codec_addr, stream->converter_node,
                  HDA_SET_STREAM_FORMAT, ((verb >> 8) & 0xFF));

    stream->sample_rate = sample_rate;
    stream->channels = channels;
    stream->bits_per_sample = bits;

    return true;
}

static void hda_set_stream_format_reg(hda_state_t* state, hda_stream_t* stream) {
    uint32_t sd_offset = HDA_SD_BASE + (stream->stream_tag - 1) * HDA_SD_SIZE;

    uint16_t fmt = 0;
    if (stream->sample_rate >= 44100) {
        fmt |= (1 << 14);
    }

    switch (stream->bits_per_sample) {
        case 8:  fmt |= 0x00; break;
        case 16: fmt |= (1 << 4); break;
        case 20: fmt |= (2 << 4); break;
        case 24: fmt |= (3 << 4); break;
        case 32: fmt |= (4 << 4); break;
        default: fmt |= (1 << 4); break;
    }

    if (stream->channels > 1) {
        fmt |= (stream->channels - 1) & 0x0F;
    }

    hda_write16(state, sd_offset + HDA_SD_FMT, fmt);
}

static void hda_set_stream_descriptor(hda_state_t* state, hda_stream_t* stream) {
    uint32_t sd_offset = HDA_SD_BASE + (stream->stream_tag - 1) * HDA_SD_SIZE;

    hda_write8(state, sd_offset + HDA_SD_CTL0, HDA_SD_CTL_SRST);
    for (int i = 0; i < 100; i++) {
        if (!(hda_read8(state, sd_offset + HDA_SD_CTL0) & HDA_SD_CTL_SRST)) {
            break;
        }
        hda_delay_us(10);
    }

    uint32_t frame_bytes = (stream->bits_per_sample / 8) * stream->channels;
    uint32_t total_bytes = stream->dma_buffer_size;
    uint32_t last_index = (total_bytes / frame_bytes) - 1;
    if (last_index >= HDA_BDL_MAX_ENTRIES) {
        last_index = HDA_BDL_MAX_ENTRIES - 1;
    }

    hda_write8(state, sd_offset + HDA_SD_CTL0, 0);
    hda_write8(state, sd_offset + HDA_SD_CTL1, stream->stream_tag);
    hda_write8(state, sd_offset + HDA_SD_CTL2, 0);

    hda_write32(state, sd_offset + HDA_SD_CBL, total_bytes);
    hda_write16(state, sd_offset + HDA_SD_LVI, last_index);
    hda_write32(state, sd_offset + HDA_SD_BDPL, stream->bdl_phys & 0xFFFFFFFF);
    hda_write32(state, sd_offset + HDA_SD_BDPU, 0);

    hda_set_stream_format_reg(state, stream);

    hda_write8(state, sd_offset + HDA_SD_STATUS, HDA_SD_STAT_DESC |
               HDA_SD_STAT_FIFOE | HDA_SD_STAT_BCE);
}

static void hda_start_stream(hda_state_t* state, hda_stream_t* stream) {
    uint32_t sd_offset = HDA_SD_BASE + (stream->stream_tag - 1) * HDA_SD_SIZE;

    uint8_t ctl = hda_read8(state, sd_offset + HDA_SD_CTL0);
    ctl |= HDA_SD_CTL_IOCE | HDA_SD_CTL_FEIE | HDA_SD_CTL_DEIE;
    hda_write8(state, sd_offset + HDA_SD_CTL0, ctl);

    ctl = hda_read8(state, sd_offset + HDA_SD_CTL0);
    ctl |= HDA_SD_CTL_RUN;
    hda_write8(state, sd_offset + HDA_SD_CTL0, ctl);

    stream->active = true;
}

static void hda_stop_stream(hda_state_t* state, hda_stream_t* stream) {
    uint32_t sd_offset = HDA_SD_BASE + (stream->stream_tag - 1) * HDA_SD_SIZE;

    uint8_t ctl = hda_read8(state, sd_offset + HDA_SD_CTL0);
    ctl &= ~HDA_SD_CTL_RUN;
    hda_write8(state, sd_offset + HDA_SD_CTL0, ctl);

    for (int i = 0; i < 100; i++) {
        if (!(hda_read8(state, sd_offset + HDA_SD_CTL0) & HDA_SD_CTL_RUN)) {
            break;
        }
        hda_delay_us(10);
    }

    stream->active = false;
}

static bool hda_find_output_path(hda_state_t* state, hda_stream_t* stream,
                                 uint32_t sample_rate, uint8_t channels, uint8_t bits) {
    for (uint8_t c = 0; c < state->num_codecs; c++) {
        hda_codec_t* codec = &state->codecs[c];
        if (!codec->present) continue;

        for (uint8_t p = 0; p < codec->num_pins; p++) {
            hda_widget_t* pin = &codec->pin_widgets[p];
            if (!(pin->pin_cap & HDA_PIN_CAP_OUT)) continue;

            for (uint8_t o = 0; o < codec->num_outputs; o++) {
                hda_widget_t* out = &codec->output_widgets[o];

                stream->codec_addr = codec->codec_addr;
                stream->pin_node = pin->node_id;
                stream->converter_node = out->node_id;

                hda_configure_pin(state, codec->codec_addr, pin->node_id);

                hda_configure_converter(state, stream, sample_rate, channels, bits);

                debuglog(DEBUG_INFO, "HDA: Output path: codec %u pin %u converter %u\n",
                         codec->codec_addr, pin->node_id, out->node_id);
                return true;
            }
        }
    }
    return false;
}

static void hda_set_volume(hda_state_t* state, uint8_t volume) {
    for (uint8_t c = 0; c < state->num_codecs; c++) {
        hda_codec_t* codec = &state->codecs[c];
        if (!codec->present) continue;

        for (uint8_t o = 0; o < codec->num_outputs; o++) {
            hda_widget_t* out = &codec->output_widgets[o];
            uint8_t gain = (volume * 39) / 255;

            uint32_t amp_get = hda_send_verb(state, codec->codec_addr, out->node_id,
                                             HDA_GET_PROCESSING_COEFF, 0x00);

            uint32_t amp_set = (1u << 31) |
                               (0u << 30) |
                               (1u << 29) |
                               (gain << 24) |
                               (1u << 15) |
                               (0u << 14) |
                               (1u << 13) |
                               (gain << 8);

            hda_send_verb(state, codec->codec_addr, out->node_id,
                          HDA_SET_PROCESSING_COEFF, amp_set & 0xFF);
            hda_send_verb(state, codec->codec_addr, out->node_id,
                          HDA_SET_PROCESSING_COEFF, (amp_set >> 8) & 0xFF);
        }
    }
}

static void hda_set_power_state(hda_state_t* state, uint8_t codec_addr, uint8_t node, uint8_t power_state) {
    uint32_t response = hda_send_verb(state, codec_addr, node, 0x705, power_state);
    (void)response;
}

static void hda_set_default_format(hda_state_t* state) {
    state->default_sample_rate = 48000;
    state->default_channels = 2;
    state->default_bits_per_sample = 16;
}

static bool hda_negotiate_format(hda_state_t* state, hda_stream_t* stream) {
    uint32_t best_rate = 0;
    uint8_t best_channels = 0;
    uint8_t best_bits = 0;

    for (uint8_t c = 0; c < state->num_codecs; c++) {
        hda_codec_t* codec = &state->codecs[c];
        if (!codec->present) continue;

        for (uint8_t o = 0; o < codec->num_outputs; o++) {
            hda_widget_t* out = &codec->output_widgets[o];

            uint32_t fmt_cap = hda_get_parameter(state, codec->codec_addr,
                                                  out->node_id, HDA_PARAM_CONV_FMT_CAP);

            if (fmt_cap & (1 << 4)) {
                best_bits = 16;
            } else if (fmt_cap & (1 << 3)) {
                best_bits = 20;
            } else if (fmt_cap & (1 << 2)) {
                best_bits = 24;
            } else if (fmt_cap & (1 << 5)) {
                best_bits = 32;
            } else if (fmt_cap & (1 << 1)) {
                best_bits = 8;
            }

            if (fmt_cap & (1 << 20)) {
                best_rate = 48000;
            } else if (fmt_cap & (1 << 11)) {
                best_rate = 44100;
            } else if (fmt_cap & (1 << 18)) {
                best_rate = 96000;
            }

            if (best_rate == 0) best_rate = 48000;
            if (best_bits == 0) best_bits = 16;
            best_channels = 2;

            debuglog(DEBUG_INFO, "HDA: Negotiated: %u Hz, %u-bit, %u ch\n",
                     best_rate, best_bits, best_channels);
        }
    }

    if (best_rate == 0) {
        best_rate = 48000;
        best_bits = 16;
        best_channels = 2;
    }

    stream->sample_rate = best_rate;
    stream->channels = best_channels;
    stream->bits_per_sample = best_bits;

    state->default_sample_rate = best_rate;
    state->default_channels = best_channels;
    state->default_bits_per_sample = best_bits;

    return true;
}

static hda_stream_t* hda_allocate_stream(hda_state_t* state) {
    for (int i = 0; i < HDA_MAX_OUTPUT_STREAMS; i++) {
        if (!state->streams[i].allocated) {
            hda_stream_t* stream = &state->streams[i];
            memset(stream, 0, sizeof(hda_stream_t));
            stream->stream_tag = state->next_stream_tag;
            state->next_stream_tag++;
            if (state->next_stream_tag > state->num_output_streams) {
                state->next_stream_tag = 1;
            }
            stream->allocated = true;
            return stream;
        }
    }
    return 0;
}

static void hda_free_stream(hda_state_t* state, hda_stream_t* stream) {
    if (stream->active) {
        hda_stop_stream(state, stream);
    }

    if (stream->dma_buffer_virt) {
        bitmap_pmm_free_pages(stream->dma_frame, stream->dma_pages);
        stream->dma_buffer_virt = 0;
    }

    if (stream->bdl_virt) {
        bitmap_pmm_free_page(stream->bdl_frame);
        stream->bdl_virt = 0;
    }

    stream->allocated = false;
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

    if (pci_find_by_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_HD_AUDIO, &device)) {
        debuglog(DEBUG_INFO, "HDA: Found by class code 0x04/0x03\n");
        found = true;
    }

    if (!found) {
        struct { uint16_t vendor; uint16_t device; } hda_ids[] = {
            { HDA_VENDOR_INTEL, 0x2668 },
            { HDA_VENDOR_INTEL, 0x27D8 },
            { HDA_VENDOR_INTEL, 0x284B },
            { HDA_VENDOR_INTEL, 0x293E },
            { HDA_VENDOR_INTEL, 0x3B56 },
            { HDA_VENDOR_INTEL, 0x1C20 },
            { HDA_VENDOR_INTEL, 0x1E20 },
        };
        for (uint32_t i = 0; i < sizeof(hda_ids)/sizeof(hda_ids[0]); i++) {
            if (pci_find_by_vendor_device(hda_ids[i].vendor, hda_ids[i].device, &device)) {
                debuglog(DEBUG_INFO, "HDA: Found by vendor:device %04X:%04X\n",
                         hda_ids[i].vendor, hda_ids[i].device);
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
    if (!(bar0 & 0x1)) {
        state->mmio = (volatile uint32*)(uintptr_t)(bar0 & ~0xFu);
    } else {
        return false;
    }

    uint16 command = pci_config_read16(device.segment, device.bus, device.device, device.function, 4);
    command |= 0x0006;
    pci_config_write16(device.segment, device.bus, device.device, device.function, 4, command);

    state->irq_line = pci_config_read8(device.segment, device.bus, device.device, device.function, 0x3C);

    state->gcap = hda_read16(state, HDA_REG_GCAP);
    state->num_output_streams = (state->gcap >> 12) & 0x0F;
    state->num_input_streams = (state->gcap >> 8) & 0x0F;
    state->num_bidirectional = (state->gcap >> 3) & 0x1F;
    state->gcap_ver = state->gcap & 0x07;
    state->output_pay = (hda_read16(state, HDA_REG_OUTPAY) & 0x3F) + 1;
    state->input_pay = (hda_read16(state, HDA_REG_INPAY) & 0x3F) + 1;

    if (state->num_output_streams == 0) {
        debuglog(DEBUG_ERROR, "HDA: No output streams in GCAP\n");
        return false;
    }

    debuglog(DEBUG_INFO, "HDA: GCAP=0x%04X out=%u in=%u bi=%u\n",
             state->gcap, state->num_output_streams, state->num_input_streams, state->num_bidirectional);
    debuglog(DEBUG_INFO, "HDA: Device %04X:%04X MMIO=%p IRQ=%u\n",
             device.vendor_id, device.device_id, (void*)state->mmio, state->irq_line);

    return true;
}

static void hda_interrupt_handler(struct interrupt_frame* frame, uint32_t error_code) {
    (void)frame;
    (void)error_code;

    hda_state_t* state = g_hda_state;
    if (!state || !state->initialized) return;

    uint32_t int_status = hda_read(state, HDA_REG_INTSTS);
    if (!(int_status & HDA_INT_GLOBAL)) return;

    if (int_status & HDA_INT_CONTROLLER) {
        for (int i = 0; i < HDA_MAX_OUTPUT_STREAMS; i++) {
            hda_stream_t* stream = &state->streams[i];
            if (!stream->allocated || !stream->active) continue;

            uint32_t sd_offset = HDA_SD_BASE + (stream->stream_tag - 1) * HDA_SD_SIZE;
            uint8_t status = hda_read8(state, sd_offset + HDA_SD_STATUS);

            if (status & (HDA_SD_STAT_DESC | HDA_SD_STAT_FIFOE)) {
                hda_write8(state, sd_offset + HDA_SD_STATUS, status);
                state->interrupt_fired = true;
            }
        }
    }

    uint32_t rirb_status = hda_read8(state, HDA_REG_RIRBSTS);
    if (rirb_status) {
        hda_write8(state, HDA_REG_RIRBSTS, rirb_status);
    }

    hda_write(state, HDA_REG_INTSTS, int_status);
}

static bool hda_init(SoundDriver* driver) {
    if (!driver || !driver->state) {
        return false;
    }
    hda_state_t* state = (hda_state_t*)driver->state;
    if (!state->mmio) {
        return false;
    }

    debuglog(DEBUG_INFO, "HDA: Initializing controller\n");

    hda_write(state, HDA_REG_GCTL, 0);
    for (int i = 0; i < 1000; i++) {
        if (!(hda_read(state, HDA_REG_GCTL) & HDA_GCTL_CRST)) break;
    }
    hda_delay_us(100);

    hda_write(state, HDA_REG_GCTL, HDA_GCTL_CRST);
    for (int i = 0; i < 1000; i++) {
        if (hda_read(state, HDA_REG_GCTL) & HDA_GCTL_CRST) break;
    }

    if (!(hda_read(state, HDA_REG_GCTL) & HDA_GCTL_CRST)) {
        debuglog(DEBUG_ERROR, "HDA: Controller failed to come out of reset\n");
        return false;
    }

    hda_write(state, HDA_REG_STATESTS, 0x0F);
    hda_delay_us(100);

    if (!hda_setup_corb_rirb(state)) {
        debuglog(DEBUG_ERROR, "HDA: Failed to setup CORB/RIRB\n");
        return false;
    }

    hda_detect_codecs(state);

    state->next_stream_tag = 1;
    state->active_streams = 0;

    for (int i = 0; i < HDA_MAX_OUTPUT_STREAMS; i++) {
        hda_stream_t* stream = &state->streams[i];
        if (!hda_allocate_buffer(state, stream, HDA_DMA_BUFFER_SIZE)) {
            debuglog(DEBUG_ERROR, "HDA: Failed to allocate stream %d buffer\n", i);
            continue;
        }

        if (!hda_allocate_bdl(state, stream)) {
            debuglog(DEBUG_ERROR, "HDA: Failed to allocate stream %d BDL\n", i);
            hda_free_stream(state, stream);
            continue;
        }

        hda_setup_bdl(stream);
        stream->stream_tag = state->next_stream_tag;
        state->next_stream_tag++;
        stream->allocated = true;

        state->active_streams++;
    }

    if (state->active_streams == 0) {
        debuglog(DEBUG_ERROR, "HDA: No streams available\n");
        return false;
    }

    hda_set_default_format(state);

    for (uint8_t c = 0; c < state->num_codecs; c++) {
        hda_codec_t* codec = &state->codecs[c];
        if (!codec->present) continue;

        for (uint8_t p = 0; p < codec->num_pins; p++) {
            hda_set_power_state(state, codec->codec_addr,
                               codec->pin_widgets[p].node_id, HDA_POWER_STATE_D0);
            codec->pin_widgets[p].power_state = HDA_POWER_STATE_D0;
        }

        for (uint8_t o = 0; o < codec->num_outputs; o++) {
            hda_set_power_state(state, codec->codec_addr,
                               codec->output_widgets[o].node_id, HDA_POWER_STATE_D0);
            codec->output_widgets[o].power_state = HDA_POWER_STATE_D0;
        }
    }

    if (state->irq_line != 0 && state->irq_line != 0xFF) {
        interrupt_set_handler_legacy(0x20 + state->irq_line, hda_interrupt_handler);
        debuglog(DEBUG_INFO, "HDA: IRQ handler registered on IRQ %u\n", state->irq_line);
    }

    state->initialized = true;
    g_hda_state = state;
    debuglog(DEBUG_INFO, "HDA: Driver initialized (%u streams, %u codecs)\n",
             state->active_streams, state->num_codecs);

    return true;
}

static bool hda_play_pcm(SoundDriver* driver, const uint8* data, uint32 length, const SoundFormat* format) {
    if (!driver || !driver->state || !data || !format || length == 0) {
        return false;
    }
    hda_state_t* state = (hda_state_t*)driver->state;
    if (!state->initialized) {
        return false;
    }

    hda_stream_t* stream = 0;
    for (int i = 0; i < HDA_MAX_OUTPUT_STREAMS; i++) {
        if (state->streams[i].allocated && !state->streams[i].active) {
            stream = &state->streams[i];
            break;
        }
    }

    if (!stream) {
        for (int i = 0; i < HDA_MAX_OUTPUT_STREAMS; i++) {
            if (state->streams[i].allocated) {
                stream = &state->streams[i];
                hda_stop_stream(state, stream);
                break;
            }
        }
    }

    if (!stream) {
        return false;
    }

    if (format->bits_per_sample != 8 && format->bits_per_sample != 16 &&
        format->bits_per_sample != 24 && format->bits_per_sample != 32) {
        return false;
    }

    if (format->channels < 1 || format->channels > 8) {
        return false;
    }

    if (!stream->converter_node) {
        if (!hda_find_output_path(state, stream, format->sample_rate,
                                  format->channels, format->bits_per_sample)) {
            debuglog(DEBUG_ERROR, "HDA: No output path found\n");
            return false;
        }
    }

    hda_configure_converter(state, stream, format->sample_rate,
                            format->channels, format->bits_per_sample);

    uint32_t bytes_per_sample = format->bits_per_sample / 8;
    uint32_t frame_bytes = bytes_per_sample * format->channels;
    uint32_t copy_len = length;
    uint32_t total_buffer = HDA_DMA_BUFFER_SIZE;

    if (copy_len > total_buffer) {
        copy_len = total_buffer;
    }
    if (copy_len == 0) {
        return false;
    }

    memset(stream->dma_buffer_virt, 0, total_buffer);
    memcpy(stream->dma_buffer_virt, data, copy_len);

    uint32_t num_frames = copy_len / frame_bytes;
    if (num_frames == 0) return false;
    if (num_frames > HDA_BDL_MAX_ENTRIES) {
        num_frames = HDA_BDL_MAX_ENTRIES;
        copy_len = num_frames * frame_bytes;
    }

    uint32_t period_size = total_buffer / HDA_BUFFER_PERIODS;
    uint32_t periods_needed = (copy_len + period_size - 1) / period_size;
    if (periods_needed > HDA_BUFFER_PERIODS) {
        periods_needed = HDA_BUFFER_PERIODS;
    }

    for (uint32_t i = 0; i < periods_needed; i++) {
        stream->bdl_virt[i].addr = stream->dma_buffer_phys + i * period_size;
        stream->bdl_virt[i].length = (i == periods_needed - 1) ?
                                     (copy_len - i * period_size) : period_size;
        stream->bdl_virt[i].ioc = (i == periods_needed - 1) ? 0x80000000 : 0;
        stream->bdl_virt[i].reserved = 0;
    }
    for (uint32_t i = periods_needed; i < HDA_BUFFER_PERIODS; i++) {
        stream->bdl_virt[i].addr = stream->dma_buffer_phys;
        stream->bdl_virt[i].length = 0;
        stream->bdl_virt[i].ioc = 0;
        stream->bdl_virt[i].reserved = 0;
    }

    hda_set_stream_descriptor(state, stream);

    hda_start_stream(state, stream);

    return true;
}

static void hda_set_volume_driver(SoundDriver* driver, uint8 volume) {
    if (!driver || !driver->state) {
        return;
    }
    hda_state_t* state = (hda_state_t*)driver->state;
    if (!state->initialized) {
        return;
    }

    driver->volume = volume;
    hda_set_volume(state, volume);
}

static void hda_beep(SoundDriver* driver, uint32 frequency_hz, uint32 duration_ms) {
    if (!driver || !driver->state) {
        return;
    }
    hda_state_t* state = (hda_state_t*)driver->state;
    if (!state->initialized || state->active_streams == 0) {
        return;
    }

    if (frequency_hz == 0 || duration_ms == 0) {
        return;
    }

    hda_stream_t* stream = &state->streams[0];
    if (!stream->allocated) return;

    if (stream->active) {
        hda_stop_stream(state, stream);
    }

    uint32_t sample_rate = state->default_sample_rate;
    uint32_t samples = (sample_rate * duration_ms) / 1000;
    uint32_t bytes_needed = samples * 4;

    if (bytes_needed > stream->dma_buffer_size) {
        bytes_needed = stream->dma_buffer_size;
        samples = bytes_needed / 4;
    }

    int16_t* buffer = stream->dma_buffer_virt;
    uint32_t period = sample_rate / frequency_hz;
    if (period == 0) period = 1;
    const int16_t amplitude = 8000;

    for (uint32 i = 0; i < samples; i++) {
        int16_t sample = ((i % period) < (period / 2)) ? amplitude : (int16_t)-amplitude;
        buffer[i * 2] = sample;
        buffer[i * 2 + 1] = sample;
    }

    if (!stream->converter_node) {
        hda_find_output_path(state, stream, sample_rate, 2, 16);
    }

    hda_configure_converter(state, stream, sample_rate, 2, 16);

    uint32_t period_size = stream->dma_buffer_size / HDA_BUFFER_PERIODS;
    uint32_t periods = (bytes_needed + period_size - 1) / period_size;
    if (periods > HDA_BUFFER_PERIODS) periods = HDA_BUFFER_PERIODS;

    for (uint32_t i = 0; i < periods; i++) {
        stream->bdl_virt[i].addr = stream->dma_buffer_phys + i * period_size;
        stream->bdl_virt[i].length = (i == periods - 1) ?
                                     (bytes_needed - i * period_size) : period_size;
        stream->bdl_virt[i].ioc = (i == periods - 1) ? 0x80000000 : 0;
        stream->bdl_virt[i].reserved = 0;
    }
    for (uint32_t i = periods; i < HDA_BUFFER_PERIODS; i++) {
        stream->bdl_virt[i].addr = stream->dma_buffer_phys;
        stream->bdl_virt[i].length = 0;
        stream->bdl_virt[i].ioc = 0;
        stream->bdl_virt[i].reserved = 0;
    }

    hda_set_stream_descriptor(state, stream);
    hda_start_stream(state, stream);

    for (uint32_t elapsed = 0; elapsed < duration_ms + 50; elapsed++) {
        hda_delay_us(1000);
        if (state->interrupt_fired) {
            state->interrupt_fired = false;
            break;
        }
    }

    hda_stop_stream(state, stream);

    debuglog(DEBUG_INFO, "HDA: Beep - %u Hz for %u ms\n", frequency_hz, duration_ms);
}

static bool hda_get_capabilities(SoundDriver* driver, DeviceCapabilities* caps) {
    if (!driver || !caps) {
        return false;
    }
    hda_state_t* state = (hda_state_t*)driver->state;

    memset(caps, 0, sizeof(DeviceCapabilities));

    caps->supported_formats[0] = PCM_S16;
    caps->supported_formats[1] = PCM_U8;
    caps->max_channels = 2;
    caps->stereo_supported = true;
    caps->little_endian = true;
    caps->max_buffer_size = HDA_DMA_BUFFER_SIZE;

    if (state) {
        caps->native_sample_rates[0] = state->default_sample_rate;
        if (state->default_sample_rate == 48000) {
            caps->native_sample_rates[1] = 44100;
        } else {
            caps->native_sample_rates[1] = 48000;
        }
        if (state->default_bits_per_sample >= 24) {
            caps->supported_formats[2] = PCM_F32;
        }
    } else {
        caps->native_sample_rates[0] = 44100;
        caps->native_sample_rates[1] = 48000;
    }

    return true;
}

static int16_t pcm_f32_bits_to_s16(uint32 bits) {
    uint32 sign = bits >> 31;
    uint32 exp = (bits >> 23) & 0xFF;
    uint32 mant = bits & 0x7FFFFF;

    if (exp == 0) return 0;
    if (exp == 255) return sign ? (int16_t)-32768 : 32767;

    int32 exponent = (int32)exp - 127;
    uint32 mantissa = (1u << 23) | mant;
    int32 shift = exponent - 23;
    int64 value = (int64)mantissa * 32767;

    if (shift > 0) {
        if (shift >= 31) return sign ? (int16_t)-32768 : 32767;
        value <<= shift;
    } else if (shift < 0) {
        value >>= -shift;
    }

    if (sign) value = -value;
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return (int16_t)value;
}

uint32_t convert_to_hda_pcm(const void* src, uint32_t src_frames, const PcmDesc* src_desc,
                            int16_t* dst, uint32_t dst_max_frames) {
    uint32_t dst_rate = 48000;
    if (src_desc->sample_rate > 44100 && src_desc->sample_rate <= 48000) {
        dst_rate = 48000;
    } else {
        uint32_t d44 = (src_desc->sample_rate > 44100) ?
                       (src_desc->sample_rate - 44100) : (44100 - src_desc->sample_rate);
        uint32_t d48 = (src_desc->sample_rate > 48000) ?
                       (src_desc->sample_rate - 48000) : (48000 - src_desc->sample_rate);
        dst_rate = (d48 < d44) ? 48000 : 44100;
    }

    uint32_t dst_frames = (src_frames * dst_rate) / src_desc->sample_rate;
    if (dst_frames > dst_max_frames) {
        dst_frames = dst_max_frames;
    }

    for (uint32_t i = 0; i < dst_frames; i++) {
        uint32_t src_i = (i * src_desc->sample_rate) / dst_rate;
        if (src_i >= src_frames) src_i = src_frames - 1;

        if (src_desc->format == PCM_S16) {
            const int16_t* s = (const int16_t*)src;
            if (src_desc->channels == 2) {
                dst[i * 2] = s[src_i * 2];
                dst[i * 2 + 1] = s[src_i * 2 + 1];
            } else {
                dst[i * 2] = s[src_i];
                dst[i * 2 + 1] = s[src_i];
            }
        } else if (src_desc->format == PCM_F32) {
            const uint32* s = (const uint32*)src;
            uint32 l_bits = (src_desc->channels == 2) ? s[src_i * 2] : s[src_i];
            uint32 r_bits = (src_desc->channels == 2) ? s[src_i * 2 + 1] : s[src_i];
            dst[i * 2] = pcm_f32_bits_to_s16(l_bits);
            dst[i * 2 + 1] = pcm_f32_bits_to_s16(r_bits);
        } else {
            dst[i * 2] = dst[i * 2 + 1] = 0;
        }
    }

    return dst_frames;
}

static void hda_shutdown(SoundDriver* driver) {
    if (!driver || !driver->state) {
        return;
    }
    hda_state_t* state = (hda_state_t*)driver->state;

    for (int i = 0; i < HDA_MAX_OUTPUT_STREAMS; i++) {
        if (state->streams[i].allocated) {
            hda_free_stream(state, &state->streams[i]);
        }
    }

    for (uint8_t c = 0; c < state->num_codecs; c++) {
        hda_codec_t* codec = &state->codecs[c];
        if (!codec->present) continue;

        for (uint8_t p = 0; p < codec->num_pins; p++) {
            hda_set_power_state(state, codec->codec_addr,
                               codec->pin_widgets[p].node_id, HDA_POWER_STATE_D3);
        }

        for (uint8_t o = 0; o < codec->num_outputs; o++) {
            hda_set_power_state(state, codec->codec_addr,
                               codec->output_widgets[o].node_id, HDA_POWER_STATE_D3);
        }
    }

    uint32_t rirbctl = hda_read8(state, HDA_REG_RIRBCTL);
    rirbctl &= ~(HDA_RIRBCTL_RUN | HDA_RIRBCTL_INT_EN);
    hda_write8(state, HDA_REG_RIRBCTL, rirbctl);

    uint32_t corbctl = hda_read8(state, HDA_REG_CORBCTL);
    corbctl &= ~HDA_CORBCTL_RUN;
    hda_write8(state, HDA_REG_CORBCTL, corbctl);

    if (state->irq_line != 0 && state->irq_line != 0xFF) {
        interrupt_clear_handler(0x20 + state->irq_line);
    }

    if (state->corb_virt) {
        bitmap_pmm_free_page(state->corb_frame);
        state->corb_virt = 0;
    }
    if (state->rirb_virt) {
        bitmap_pmm_free_page(state->rirb_frame);
        state->rirb_virt = 0;
    }

    hda_write(state, HDA_REG_GCTL, 0);

    state->initialized = false;
    debuglog(DEBUG_INFO, "HDA: Shutdown complete\n");
}

static SoundDriver g_hda_driver = {
    .name = "Intel HDA",
    .type = SOUND_DEVICE_HDA,
    .detect = hda_detect,
    .init = hda_init,
    .play_pcm = hda_play_pcm,
    .get_capabilities = hda_get_capabilities,
    .set_volume = hda_set_volume_driver,
    .beep = hda_beep,
    .shutdown = hda_shutdown,
    .state = 0,
    .volume = 255
};

SoundDriver* sound_hda_driver(void) {
    return &g_hda_driver;
}
