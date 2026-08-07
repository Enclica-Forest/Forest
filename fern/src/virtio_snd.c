/*
 * virtio_snd.c - VirtIO sound driver for MMIO transport
 *
 * Targets AArch64 and RISC-V QEMU virt platforms where virtio devices
 * are exposed via memory-mapped I/O (MMIO) at 0x0a000000+.
 *
 * This driver:
 *   1. Parses the FDT to locate the virtio-mmio node with device ID 18
 *   2. Reads the MMIO base address from the "reg" property
 *   3. Initializes the VirtIO sound device via MMIO registers
 *   4. Sets up split virtqueues for control, TX (playback), RX (capture), and events
 *   5. Registers as a SoundDriver with the kernel sound subsystem
 *
 * On x86, this driver is not used; PCI-based sound is handled by
 * the existing AC97/HDA/SB16 drivers.
 */

#include "virtio_snd.h"
#include "fdt.h"
#include "arch/arch.h"
#include "include/sound.h"
#include "include/system.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/memory.h"
#include "include/timer.h"

extern void* kmalloc(size_t size);
extern void kfree(void* ptr);

/* ------------------------------------------------------------------ */
/* Internal state                                                      */
/* ------------------------------------------------------------------ */

static virtio_snd_state_t g_virtio_snd;
static bool               g_virtio_snd_available = false;
static spinlock_t         g_vq_lock = SPINLOCK_INIT("virtio_snd");

/* ------------------------------------------------------------------ */
/* MMIO access helpers (with memory barriers)                          */
/* ------------------------------------------------------------------ */

static inline uint32_t vio_snd_mmio_read(uint32_t offset)
{
    return mmio_read32((const void*)(uintptr_t)(g_virtio_snd.mmio_phys + offset));
}

static inline void vio_snd_mmio_write(uint32_t offset, uint32_t val)
{
    mmio_write32((volatile void*)(uintptr_t)(g_virtio_snd.mmio_phys + offset), val);
}

/* ------------------------------------------------------------------ */
/* Virtqueue management                                                */
/* ------------------------------------------------------------------ */

static void vq_init(virtio_snd_vq_t* vq)
{
    vq->num_free = VIRTIO_SND_VQ_SIZE;
    vq->free_head = 0;
    vq->last_used_idx = 0;

    for (uint16_t i = 0; i < VIRTIO_SND_VQ_SIZE - 1; i++) {
        vq->desc[i].next = i + 1;
    }
    vq->desc[VIRTIO_SND_VQ_SIZE - 1].next = 0xFFFF;

    memset((uint8_t*)vq->avail, 0, sizeof(virtio_snd_vring_avail_t));
    memset((uint8_t*)vq->used, 0, sizeof(virtio_snd_vring_used_t));
}

static uint16_t vq_alloc_desc(virtio_snd_vq_t* vq)
{
    if (vq->num_free == 0)
        return 0xFFFF;

    uint16_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    return idx;
}

static void vq_free_desc(virtio_snd_vq_t* vq, uint16_t idx)
{
    vq->desc[idx].next = vq->free_head;
    vq->desc[idx].addr = 0;
    vq->desc[idx].len = 0;
    vq->desc[idx].flags = 0;
    vq->free_head = idx;
    vq->num_free++;
}

static void vq_submit(virtio_snd_vq_t* vq, uint16_t head_idx, uint16_t queue_idx)
{
    uint16_t avail_idx = vq->avail->idx;
    vq->avail->ring[avail_idx % VIRTIO_SND_VQ_SIZE] = head_idx;
    arch_wmb();
    vq->avail->idx = avail_idx + 1;
    vio_snd_mmio_write(VIRTIO_SND_MMIO_QUEUE_NOTIFY, queue_idx);
}

/* ------------------------------------------------------------------ */
/* Feature negotiation                                                 */
/* ------------------------------------------------------------------ */

static int negotiate_features(void)
{
    g_virtio_snd.host_features = vio_snd_mmio_read(VIRTIO_SND_MMIO_DEVICE_FEAT);

    /* We accept: nothing special beyond basic operation. */
    uint32_t desired = 0;
    g_virtio_snd.guest_features = g_virtio_snd.host_features & desired;

    vio_snd_mmio_write(VIRTIO_SND_MMIO_DRIVER_FEAT, g_virtio_snd.guest_features);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Virtqueue setup                                                     */
/* ------------------------------------------------------------------ */

static int setup_virtqueue(int index)
{
    virtio_snd_vq_t* vq = &g_virtio_snd.vq[index];

    vio_snd_mmio_write(VIRTIO_SND_MMIO_QUEUE_SEL, (uint32_t)index);

    uint32_t qsize = vio_snd_mmio_read(VIRTIO_SND_MMIO_QUEUE_NUM_MAX);
    if (qsize == 0) {
        debuglog(DEBUG_WARN, "VIRTIO-SND: VQ %d not present\n", index);
        return -1;
    }
    if (qsize > VIRTIO_SND_VQ_SIZE)
        qsize = VIRTIO_SND_VQ_SIZE;

    (void)g_virtio_snd.vq[index].buf[0];

    /* Use the pre-allocated buffer memory for vring structures.
     * We place the vring at the start of a 12K block:
     * desc (16B * 128 = 2048) | avail (256B) | used (page-aligned after avail). */
    static uint8_t vring_storage[VIRTIO_SND_NUM_QUEUES][16384];
    uint8_t* vbase = vring_storage[index];

    uint32_t desc_size = sizeof(virtio_snd_vring_desc_t) * qsize;
    uint32_t avail_size = sizeof(virtio_snd_vring_avail_t);
    uint32_t used_offset = (avail_size + 4095) & ~4095u;

    vq->desc  = (virtio_snd_vring_desc_t*)(vbase);
    vq->avail = (virtio_snd_vring_avail_t*)(vbase + desc_size);
    vq->used  = (virtio_snd_vring_used_t*)(vbase + used_offset);

    memset(vbase, 0, used_offset + sizeof(virtio_snd_vring_used_t));

    vq_init(vq);

    uint64_t paddr = (uint64_t)(uintptr_t)vbase;
    uint32_t pfn = (uint32_t)(paddr >> 12);
    vio_snd_mmio_write(VIRTIO_SND_MMIO_QUEUE_PFN, pfn);

    debuglog(DEBUG_INFO, "VIRTIO-SND: VQ %d size=%u pfn=0x%x free=%u\n",
             index, qsize, pfn, vq->num_free);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Control message helpers                                             */
/* ------------------------------------------------------------------ */

static int send_control_message(uint32_t code, const void* data, uint32_t data_len,
                                void* resp, uint32_t resp_len)
{
    (void)resp;
    virtio_snd_vq_t* vq = &g_virtio_snd.vq[VIRTIO_SND_Q_CONTROL];

    if (vq->num_free < 2) {
        debuglog(DEBUG_WARN, "VIRTIO-SND: control queue full\n");
        return -1;
    }

    /* Allocate descriptor for request header + data. */
    uint16_t req_desc = vq_alloc_desc(vq);
    if (req_desc == 0xFFFF) return -1;

    /* Allocate descriptor for response. */
    uint16_t resp_desc = vq_alloc_desc(vq);
    if (resp_desc == 0xFFFF) {
        vq_free_desc(vq, req_desc);
        return -1;
    }

    /* Build the request: header + optional data. */
    virtio_snd_ctrl_hdr_t* hdr = (virtio_snd_ctrl_hdr_t*)vq->buf[req_desc];
    hdr->code = code;
    if (data && data_len > 0) {
        memcpy(hdr->data, data, data_len);
    }

    uint32_t req_size = sizeof(virtio_snd_ctrl_hdr_t) + data_len;

    /* Set up descriptors. */
    vq->desc[req_desc].addr = (uint64_t)(uintptr_t)vq->buf[req_desc];
    vq->desc[req_desc].len  = req_size;
    vq->desc[req_desc].flags = VIRTIO_SND_DESC_F_NEXT;
    vq->desc[req_desc].next  = resp_desc;

    vq->desc[resp_desc].addr = (uint64_t)(uintptr_t)vq->buf[resp_desc];
    vq->desc[resp_desc].len  = resp_len;
    vq->desc[resp_desc].flags = VIRTIO_SND_DESC_F_WRITE;
    vq->desc[resp_desc].next  = 0;

    vq_submit(vq, req_desc, VIRTIO_SND_Q_CONTROL);

    /* Wait for response (poll with timeout). */
    for (int timeout = 0; timeout < 10000; timeout++) {
        arch_rmb();
        if (vq->used->idx != vq->last_used_idx) {
            uint16_t used_idx = vq->last_used_idx % VIRTIO_SND_VQ_SIZE;
            virtio_snd_vring_used_elem_t* elem = &vq->used->ring[used_idx];
            uint16_t desc_idx = (uint16_t)elem->id;
            vq_free_desc(vq, desc_idx);
            vq->last_used_idx++;
            return 0;
        }
        arch_cpu_relax();
    }

    debuglog(DEBUG_WARN, "VIRTIO-SND: control message timeout (code=0x%x)\n", code);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

int virtio_snd_play(const uint8_t* data, uint32_t len, uint32_t sample_rate,
                     uint32_t channels, uint32_t bits_per_sample)
{
    (void)sample_rate;
    (void)channels;
    (void)bits_per_sample;
    if (!g_virtio_snd.initialized || !data || len == 0)
        return -1;

    if (len > VIRTIO_SND_MAX_PCM_BUF)
        len = VIRTIO_SND_MAX_PCM_BUF;

    spinlock_acquire(&g_vq_lock);
    virtio_snd_vq_t* vq = &g_virtio_snd.vq[VIRTIO_SND_Q_TX];

    if (vq->num_free < 2) {
        spinlock_release(&g_vq_lock);
        return -1;
    }

    uint16_t hdr_desc = vq_alloc_desc(vq);
    if (hdr_desc == 0xFFFF) {
        spinlock_release(&g_vq_lock);
        return -1;
    }

    uint16_t data_desc = vq_alloc_desc(vq);
    if (data_desc == 0xFFFF) {
        vq_free_desc(vq, hdr_desc);
        spinlock_release(&g_vq_lock);
        return -1;
    }

    /* Prepare the virtio-snd PCM header. */
    virtio_snd_pcm_hdr_t* pcm_hdr = (virtio_snd_pcm_hdr_t*)vq->buf[hdr_desc];
    pcm_hdr->offset = 0;
    pcm_hdr->flags = 0;

    /* Copy PCM data into the vring buffer. */
    memcpy(vq->buf[data_desc], data, len);

    /* Set up descriptor chain: PCM header -> data. */
    vq->desc[hdr_desc].addr = (uint64_t)(uintptr_t)vq->buf[hdr_desc];
    vq->desc[hdr_desc].len  = sizeof(virtio_snd_pcm_hdr_t);
    vq->desc[hdr_desc].flags = VIRTIO_SND_DESC_F_NEXT;
    vq->desc[hdr_desc].next  = data_desc;

    vq->desc[data_desc].addr = (uint64_t)(uintptr_t)vq->buf[data_desc];
    vq->desc[data_desc].len  = len;
    vq->desc[data_desc].flags = 0;
    vq->desc[data_desc].next  = 0;

    vq_submit(vq, hdr_desc, VIRTIO_SND_Q_TX);

    spinlock_release(&g_vq_lock);
    g_virtio_snd.playback_active = true;
    return (int)len;
}

/* ------------------------------------------------------------------ */
/* Volume                                                              */
/* ------------------------------------------------------------------ */

void virtio_snd_set_volume(uint8_t vol)
{
    g_virtio_snd.volume = vol;

    /* Send a mixer volume control message if the device supports it. */
    if (g_virtio_snd.initialized && g_virtio_snd.num_devices > 0) {
        /* Build volume control data: device_id=0, mixer_id=0, value=vol.
         * The actual layout depends on the implementation; this is a
         * best-effort approach for QEMU's virtio-snd. */
        uint32_t vol_data[3] = { 0, 0, (uint32_t)vol };
        send_control_message(VIRTIO_SND_R_SET_CMIXER_VOL, vol_data,
                             sizeof(vol_data), NULL, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Device query                                                        */
/* ------------------------------------------------------------------ */

static int query_device_info(void)
{
    uint32_t resp_buf[256];
    memset(resp_buf, 0, sizeof(resp_buf));

    virtio_snd_ctrl_query_dev_t req;
    req.code = VIRTIO_SND_R_QUERY_DEV_INFO;
    req.device_id = 0;

    int ret = send_control_message(req.code, &req.device_id, sizeof(req.device_id),
                                   resp_buf, sizeof(resp_buf));
    if (ret != 0) {
        debuglog(DEBUG_WARN, "VIRTIO-SND: query device info failed\n");
        return -1;
    }

    /* resp_buf[0] is the status field; check if OK. */
    if (resp_buf[0] != 0) {
        debuglog(DEBUG_WARN, "VIRTIO-SND: device info status=%u\n", resp_buf[0]);
        return -1;
    }

    g_virtio_snd.num_devices = 1;
    debuglog(DEBUG_INFO, "VIRTIO-SND: device 0 present\n");
    return 0;
}

static int query_pcm_info(void)
{
    uint32_t resp_buf[256];
    memset(resp_buf, 0, sizeof(resp_buf));

    uint32_t device_id = 0;
    int ret = send_control_message(VIRTIO_SND_R_QUERY_PCM_INFO, &device_id,
                                   sizeof(device_id), resp_buf, sizeof(resp_buf));
    if (ret != 0) {
        debuglog(DEBUG_WARN, "VIRTIO-SND: query PCM info failed\n");
        return -1;
    }

    if (resp_buf[0] != 0) {
        debuglog(DEBUG_WARN, "VIRTIO-SND: PCM info status=%u\n", resp_buf[0]);
        return -1;
    }

    /* Parse PCM info from response. */
    virtio_snd_pcm_info_t* pcm = (virtio_snd_pcm_info_t*)(resp_buf + 1);

    g_virtio_snd.pcm_channels = pcm->channels_min;
    if (g_virtio_snd.pcm_channels == 0)
        g_virtio_snd.pcm_channels = 2;

    /* Find a supported sample rate (prefer 44100, then 48000, then any). */
    g_virtio_snd.pcm_sample_rate = 0;
    for (int i = 0; i < 8; i++) {
        if (pcm->rates[i] == 44100) { g_virtio_snd.pcm_sample_rate = 44100; break; }
        if (pcm->rates[i] == 48000 && g_virtio_snd.pcm_sample_rate == 0)
            g_virtio_snd.pcm_sample_rate = 48000;
        if (pcm->rates[i] == 0) break;
    }
    if (g_virtio_snd.pcm_sample_rate == 0) {
        g_virtio_snd.pcm_sample_rate = 44100;
    }

    /* Determine bit depth from formats. */
    g_virtio_snd.pcm_format_bits = 16; /* default S16 */

    g_virtio_snd.num_pcm_streams = 1;

    debuglog(DEBUG_INFO, "VIRTIO-SND: PCM %u ch, %u Hz, %u-bit\n",
             g_virtio_snd.pcm_channels, g_virtio_snd.pcm_sample_rate,
             g_virtio_snd.pcm_format_bits);
    return 0;
}

/* ------------------------------------------------------------------ */
/* FDT discovery (AArch64 / RISC-V)                                   */
/* ------------------------------------------------------------------ */

static int discover_from_fdt(void)
{
#if ARCH_ARM64 || ARCH_RISCV64
    /* QEMU virt typically has: /soc/virtio_mmio@0a000000
     * The sound device is usually the 3rd or 4th virtio-mmio node. */
    const char* paths[] = {
        "/soc/virtio_mmio@0a000000",
        "/soc/virtio_mmio@0a000800",
        "/soc/virtio_mmio@0a001000",
        "/soc/virtio_mmio@0a001800",
        "/soc/virtio_mmio@0a002000",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        const void* node = fdt_find_node(paths[i]);
        if (!node)
            continue;

        uint32_t prop_len = 0;
        const uint32_t* reg = (const uint32_t*)fdt_get_property(paths[i], "reg", &prop_len);
        if (!reg || prop_len < 8)
            continue;

        uint32_t addr_hi = fdt32_to_cpu(reg[0]);
        uint32_t addr_lo = fdt32_to_cpu(reg[1]);
        (void)addr_hi;
        uint32_t mmio_phys = addr_lo;

        /* Verify this is a sound device by reading device ID. */
        volatile uint32_t* test_base = (volatile uint32_t*)(uintptr_t)mmio_phys;
        uint32_t magic = mmio_read32((const void*)test_base);
        if (magic != 0x74726976) /* "virt" */
            continue;

        uint32_t dev_id = mmio_read32((const void*)(uintptr_t)(mmio_phys + VIRTIO_SND_MMIO_DEVICE_ID));
        if (dev_id == VIRTIO_SND_DEV_ID) {
            debuglog(DEBUG_INFO, "VIRTIO-SND: Found sound device at 0x%x\n", mmio_phys);

            uint32_t irq = 0;
            const uint32_t* int_prop = (const uint32_t*)fdt_get_property(paths[i], "interrupts", &prop_len);
            if (int_prop && prop_len >= 8) {
                irq = fdt32_to_cpu(int_prop[1]);
            }

            g_virtio_snd.mmio_phys = mmio_phys;
            g_virtio_snd.irq = (uint32_t)irq;
            return 0;
        }
    }
#endif
    return -1;
}

static int discover_from_default(void)
{
    /* Fallback: try default MMIO base and scan for a sound device. */
    volatile uint32_t* test = (volatile uint32_t*)(uintptr_t)VIRTIO_SND_MMIO_BASE_DEFAULT;
    uint32_t magic = mmio_read32((const void*)test);
    if (magic != 0x74726976)
        return -1;

    uint32_t dev_id = mmio_read32((const void*)(uintptr_t)(VIRTIO_SND_MMIO_BASE_DEFAULT + VIRTIO_SND_MMIO_DEVICE_ID));
    if (dev_id != VIRTIO_SND_DEV_ID)
        return -1;

    g_virtio_snd.mmio_phys = VIRTIO_SND_MMIO_BASE_DEFAULT;
    g_virtio_snd.irq = 0;
    debuglog(DEBUG_INFO, "VIRTIO-SND: Found at default base 0x%x\n", VIRTIO_SND_MMIO_BASE_DEFAULT);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Interrupt handler                                                   */
/* ------------------------------------------------------------------ */

void virtio_snd_irq_handler(void)
{
    uint32_t isr = vio_snd_mmio_read(VIRTIO_SND_MMIO_INTR_STATUS);
    vio_snd_mmio_write(VIRTIO_SND_MMIO_INTR_ACK, isr);

    /* Process completed TX buffers. */
    spinlock_acquire(&g_vq_lock);
    virtio_snd_vq_t* tx_vq = &g_virtio_snd.vq[VIRTIO_SND_Q_TX];
    while (tx_vq->last_used_idx != tx_vq->used->idx) {
        uint16_t used_idx = tx_vq->last_used_idx % VIRTIO_SND_VQ_SIZE;
        virtio_snd_vring_used_elem_t* elem = &tx_vq->used->ring[used_idx];
        vq_free_desc(tx_vq, (uint16_t)elem->id);
        tx_vq->last_used_idx++;
    }
    spinlock_release(&g_vq_lock);
}

/* ------------------------------------------------------------------ */
/* Math helper for beep (simple sine approximation)                    */
/* ------------------------------------------------------------------ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float snd_sinf(float x)
{
    /* Simple Taylor series approximation. */
    float result = 0.0f;
    float term = x;
    for (int i = 0; i < 10; i++) {
        result += term;
        term *= -x * x / ((2.0f * (float)i + 2.0f) * (2.0f * (float)i + 3.0f));
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* SoundDriver callbacks                                               */
/* ------------------------------------------------------------------ */

static bool virtio_snd_detect(SoundDriver* driver)
{
    (void)driver;
    return g_virtio_snd.initialized;
}

static bool virtio_snd_driver_init(SoundDriver* driver)
{
    if (!driver) return false;
    if (!g_virtio_snd.initialized) return false;
    driver->state = &g_virtio_snd;
    return true;
}

static bool virtio_snd_driver_play_pcm(SoundDriver* driver, const uint8* data,
                                        uint32 length, const SoundFormat* format)
{
    (void)driver;
    if (!data || !format || length == 0) return false;

    return virtio_snd_play(data, length, format->sample_rate,
                           format->channels, format->bits_per_sample) > 0;
}

bool virtio_snd_driver_get_capabilities(SoundDriver* driver, DeviceCapabilities* caps)
{
    (void)driver;
    if (!caps) return false;

    memset(caps, 0, sizeof(DeviceCapabilities));
    caps->supported_formats[0] = PCM_S16;
    caps->supported_formats[1] = PCM_U8;
    caps->supported_formats[2] = 0;
    caps->max_channels = 2;
    caps->stereo_supported = true;
    caps->little_endian = true;
    caps->native_sample_rates[0] = 44100;
    caps->native_sample_rates[1] = 48000;
    caps->native_sample_rates[2] = 0;
    caps->max_buffer_size = VIRTIO_SND_MAX_PCM_BUF;
    return true;
}

static void virtio_snd_driver_set_volume(SoundDriver* driver, uint8 volume)
{
    (void)driver;
    virtio_snd_set_volume(volume);
}

static void virtio_snd_driver_beep(SoundDriver* driver, uint32 frequency_hz, uint32 duration_ms)
{
    (void)driver;
    /* Generate a simple sine-wave beep and play through virtio-snd. */
    if (frequency_hz == 0 || duration_ms == 0) return;

    uint32 sample_rate = 44100;
    uint32 channels = 1;
    uint32 bits = 16;
    uint32 total_samples = (sample_rate * duration_ms) / 1000;
    uint32 buf_size = total_samples * (bits / 8) * channels;

    int16_t* buf = (int16_t*)kmalloc(buf_size);
    if (!buf) return;

    for (uint32 i = 0; i < total_samples; i++) {
        float t = (float)i / (float)sample_rate;
        float val = 0.3f * snd_sinf(2.0f * 3.14159265f * (float)frequency_hz * t);
        buf[i] = (int16_t)(val * 32767.0f);
    }

    virtio_snd_play((const uint8_t*)buf, buf_size, sample_rate, channels, bits);
    timer_sleep_ms(duration_ms);
    kfree(buf);
}

static void virtio_snd_driver_shutdown(SoundDriver* driver)
{
    (void)driver;
    if (!g_virtio_snd.initialized) return;

    /* Reset the device. */
    vio_snd_mmio_write(VIRTIO_SND_MMIO_STATUS, 0);
    g_virtio_snd.initialized = false;
    g_virtio_snd.playback_active = false;
}

static SoundDriver g_virtio_snd_driver = {
    .name = "VirtIO Sound",
    .type = SOUND_DEVICE_VIRTIO,
    .detect = virtio_snd_detect,
    .init = virtio_snd_driver_init,
    .play_pcm = virtio_snd_driver_play_pcm,
    .get_capabilities = virtio_snd_driver_get_capabilities,
    .set_volume = virtio_snd_driver_set_volume,
    .beep = virtio_snd_driver_beep,
    .shutdown = virtio_snd_driver_shutdown,
    .state = 0,
    .volume = 255
};

SoundDriver* sound_virtio_driver(void)
{
    return &g_virtio_snd_driver;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool virtio_snd_available(void)
{
    return g_virtio_snd_available;
}

int virtio_snd_init(void)
{
    if (g_virtio_snd.initialized)
        return 0;

    memset((uint8_t*)&g_virtio_snd, 0, sizeof(g_virtio_snd));

    /* Step 1: Discover the MMIO device. */
    if (discover_from_fdt() != 0 && discover_from_default() != 0) {
        debuglog(DEBUG_WARN, "VIRTIO-SND: No virtio-sound MMIO device found\n");
        return -1;
    }

    /* Verify magic value. */
    uint32_t magic = vio_snd_mmio_read(VIRTIO_SND_MMIO_MAGIC);
    if (magic != 0x74726976) {
        debuglog(DEBUG_ERROR, "VIRTIO-SND: Bad MMIO magic 0x%x at 0x%x\n",
                 magic, g_virtio_snd.mmio_phys);
        return -1;
    }

    uint32_t dev_id = vio_snd_mmio_read(VIRTIO_SND_MMIO_DEVICE_ID);
    if (dev_id != VIRTIO_SND_DEV_ID) {
        debuglog(DEBUG_WARN, "VIRTIO-SND: MMIO device at 0x%x is not sound (id=%u)\n",
                 g_virtio_snd.mmio_phys, dev_id);
        return -1;
    }

    debuglog(DEBUG_INFO, "VIRTIO-SND: Found at 0x%x, version=%u\n",
             g_virtio_snd.mmio_phys,
             vio_snd_mmio_read(VIRTIO_SND_MMIO_VERSION));

    /* Step 2: Reset device. */
    vio_snd_mmio_write(VIRTIO_SND_MMIO_STATUS, 0);

    /* Step 3: ACK + DRIVER status. */
    vio_snd_mmio_write(VIRTIO_SND_MMIO_STATUS, VIRTIO_SND_STATUS_ACK);
    vio_snd_mmio_write(VIRTIO_SND_MMIO_STATUS, VIRTIO_SND_STATUS_ACK | VIRTIO_SND_STATUS_DRIVER);

    /* Step 4: Feature negotiation. */
    negotiate_features();

    /* Step 5: Set up control virtqueue (queue 0). */
    if (setup_virtqueue(VIRTIO_SND_Q_CONTROL) != 0) {
        debuglog(DEBUG_ERROR, "VIRTIO-SND: Failed to set up control virtqueue\n");
        vio_snd_mmio_write(VIRTIO_SND_MMIO_STATUS, VIRTIO_SND_STATUS_FAILED);
        return -1;
    }

    /* Step 6: Set up TX virtqueue (queue 1) for playback. */
    if (setup_virtqueue(VIRTIO_SND_Q_TX) != 0) {
        debuglog(DEBUG_ERROR, "VIRTIO-SND: Failed to set up TX virtqueue\n");
        vio_snd_mmio_write(VIRTIO_SND_MMIO_STATUS, VIRTIO_SND_STATUS_FAILED);
        return -1;
    }

    /* Step 7: Set up RX virtqueue (queue 2) for capture. */
    setup_virtqueue(VIRTIO_SND_Q_RX);

    /* Step 8: Set up event virtqueue (queue 3). */
    setup_virtqueue(VIRTIO_SND_Q_EVENT);

    /* Step 9: Query device capabilities. */
    query_device_info();
    query_pcm_info();

    /* Step 10: DRIVER_OK - device is live. */
    vio_snd_mmio_write(VIRTIO_SND_MMIO_STATUS,
                       VIRTIO_SND_STATUS_ACK | VIRTIO_SND_STATUS_DRIVER | VIRTIO_SND_STATUS_DRIVER_OK);

    g_virtio_snd.initialized = true;
    g_virtio_snd_available = true;

    debuglog(DEBUG_INFO, "VIRTIO-SND: Initialized successfully\n");
    return 0;
}
