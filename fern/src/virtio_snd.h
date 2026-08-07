#ifndef VIRTIO_SND_H
#define VIRTIO_SND_H

#include "include/types.h"
#include "include/sound.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * VirtIO Sound driver for MMIO transport (QEMU virt platform).
 *
 * On AArch64 and RISC-V, QEMU's "virt" machine exposes virtio devices
 * through MMIO at 0x0a000000 (and subsequent addresses for additional
 * devices).  This driver parses the FDT to locate the virtio-mmio node
 * with device ID 18 (sound), initializes the device via MMIO registers,
 * sets up split virtqueues for control and PCM playback, and registers
 * as a SoundDriver with the kernel sound subsystem.
 *
 * VirtIO Sound spec reference:
 *   - Device ID: 18
 *   - Control queue (queue 0): one-directional, driver -> device
 *   - TX queue (queue 1): playback (driver -> device)
 *   - RX queue (queue 2): capture (device -> driver)
 *   - Event queue (queue 3): status events (device -> driver)
 */

/* VirtIO Sound device ID. */
#define VIRTIO_SND_DEV_ID          18

/* VirtIO MMIO default base address on QEMU virt. */
#define VIRTIO_SND_MMIO_BASE_DEFAULT  0x0a000000u

/* VirtIO MMIO register offsets (legacy 0x100-byte region). */
#define VIRTIO_SND_MMIO_MAGIC       0x000
#define VIRTIO_SND_MMIO_VERSION     0x004
#define VIRTIO_SND_MMIO_DEVICE_ID   0x008
#define VIRTIO_SND_MMIO_VENDOR_ID   0x00c
#define VIRTIO_SND_MMIO_DEVICE_FEAT 0x010
#define VIRTIO_SND_MMIO_DRIVER_FEAT 0x020
#define VIRTIO_SND_MMIO_QUEUE_SEL   0x030
#define VIRTIO_SND_MMIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_SND_MMIO_QUEUE_NUM   0x044
#define VIRTIO_SND_MMIO_QUEUE_ALIGN 0x048
#define VIRTIO_SND_MMIO_QUEUE_PFN   0x050
#define VIRTIO_SND_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_SND_MMIO_INTR_STATUS 0x060
#define VIRTIO_SND_MMIO_INTR_ACK   0x064
#define VIRTIO_SND_MMIO_STATUS     0x070
#define VIRTIO_SND_MMIO_CONFIG     0x100

/* VirtIO device status bits. */
#define VIRTIO_SND_STATUS_ACK       0x01
#define VIRTIO_SND_STATUS_DRIVER    0x02
#define VIRTIO_SND_STATUS_DRIVER_OK 0x04
#define VIRTIO_SND_STATUS_FEAT_OK   0x08
#define VIRTIO_SND_STATUS_FAILED    0x80

/* VirtIO feature bits for sound devices. */
#define VIRTIO_SND_F_CBCM          (1u << 0)   /* Control message channel */
#define VIRTIO_SND_F_CTRL_MAGIC    (1u << 1)   /* Control message magic */
#define VIRTIO_SND_F_SHM_PERIOD    (1u << 2)   /* Shared memory period */
#define VIRTIO_SND_F_BATCH         (1u << 3)   /* Batch mode */
#define VIRTIO_SND_F_TS_COMPOSE    (1u << 4)   /* Timestamp compose */

/* VirtIO split virtqueue descriptor flags. */
#define VIRTIO_SND_DESC_F_NEXT       1
#define VIRTIO_SND_DESC_F_WRITE      2
#define VIRTIO_SND_DESC_F_INDIRECT   4

/* Queue indices. */
#define VIRTIO_SND_Q_CONTROL    0
#define VIRTIO_SND_Q_TX         1
#define VIRTIO_SND_Q_RX         2
#define VIRTIO_SND_Q_EVENT      3
#define VIRTIO_SND_NUM_QUEUES   4

/* Number of descriptors per virtqueue. */
#define VIRTIO_SND_VQ_SIZE      128

/* Maximum PCM buffer size per transfer. */
#define VIRTIO_SND_MAX_PCM_BUF  65536

/* VirtIO Sound control request types. */
#define VIRTIO_SND_R_QUERY_DEV_INFO   0x0100
#define VIRTIO_SND_R_QUERY_PCM_INFO   0x0101
#define VIRTIO_SND_R_SET_CMIXER       0x1000
#define VIRTIO_SND_R_GET_CMIXER       0x1001
#define VIRTIO_SND_R_SET_CMIXER_VOL   0x1002
#define VIRTIO_SND_R_GET_CMIXER_VOL   0x1003

/* PCM access modes. */
#define VIRTIO_SND_PCM_A_Q              (1u << 0)  /* queue */
#define VIRTIO_SND_PCM_A_RW             (1u << 1)  /* read/write */
#define VIRTIO_SND_PCM_A_SHM_PERIOD     (1u << 2)  /* shared memory period */

/* PCM channel status. */
#define VIRTIO_SND_CHST_OK              0
#define VIRTIO_SND_CHST_NOT_SUPP        1
#define VIRTIO_SND_CHST_ERR             2

/* PCM feature bits. */
#define VIRTIO_SND_PCM_F_RATE               (1u << 0)
#define VIRTIO_SND_PCM_F_BITS               (1u << 1)
#define VIRTIO_SND_PCM_F_USALIGN          (1u << 2)
#define VIRTIO_SND_PCM_F_CHANNELS           (1u << 3)
#define VIRTIO_SND_PCM_F_PERIOD             (1u << 4)
#define VIRTIO_SND_PCM_F_BLOCKSIZE          (1u << 5)
#define VIRTIO_SND_PCM_F_BUFFERSIZE         (1u << 6)

/* Common PCM rates. */
#define VIRTIO_SND_PCM_R_8000    8000
#define VIRTIO_SND_PCM_R_11025   11025
#define VIRTIO_SND_PCM_R_16000   16000
#define VIRTIO_SND_PCM_R_22050   22050
#define VIRTIO_SND_PCM_R_32000   32000
#define VIRTIO_SND_PCM_R_44100   44100
#define VIRTIO_SND_PCM_R_48000   48000

/* ------------------------------------------------------------------ */
/* Control message structures                                          */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint32_t code;
    uint32_t data[];
} virtio_snd_ctrl_hdr_t;

/* Device info request (code = R_QUERY_DEV_INFO). */
typedef struct __attribute__((packed)) {
    uint32_t code;
    uint32_t device_id;
} virtio_snd_ctrl_query_dev_t;

/* PCM info request (code = R_QUERY_PCM_INFO). */
typedef struct __attribute__((packed)) {
    uint32_t code;
    uint32_t device_id;
} virtio_snd_ctrl_query_pcm_t;

/* PCM info response. */
typedef struct __attribute__((packed)) {
    uint32_t features;
    uint32_t channels_min;
    uint32_t channels_max;
    uint32_t rates[8];
    uint32_t formats[8];
    uint32_t periods[8];
    uint32_t period_size[8];
    uint32_t buffer_size[8];
    uint32_t access_modes;
} virtio_snd_pcm_info_t;

/* PCM header prepended to each TX/RX transfer. */
typedef struct __attribute__((packed)) {
    uint32_t offset;
    uint32_t flags;
} virtio_snd_pcm_hdr_t;

/* Vring descriptor (16 bytes, per VirtIO spec). */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtio_snd_vring_desc_t;

/* Vring available ring. */
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_SND_VQ_SIZE];
} virtio_snd_vring_avail_t;

/* Vring used element. */
typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtio_snd_vring_used_elem_t;

/* Vring used ring. */
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    virtio_snd_vring_used_elem_t ring[VIRTIO_SND_VQ_SIZE];
} virtio_snd_vring_used_t;

/* Per-virtqueue state. */
typedef struct {
    virtio_snd_vring_desc_t*   desc;
    virtio_snd_vring_avail_t*  avail;
    virtio_snd_vring_used_t*   used;
    uint16_t        num_free;
    uint16_t        free_head;
    uint16_t        last_used_idx;
    uint16_t        queue_notify_off;
    /* Pre-allocated buffers for queue data. */
    uint8_t         buf[VIRTIO_SND_VQ_SIZE][4096];
} virtio_snd_vq_t;

/* Overall virtio-snd device state. */
typedef struct {
    volatile uint32_t*  mmio_base;
    uint32_t            mmio_phys;
    uint32_t            irq;
    bool                initialized;

    /* Feature negotiation. */
    uint32_t            host_features;
    uint32_t            guest_features;

    /* Four virtqueues: control, TX, RX, event. */
    virtio_snd_vq_t     vq[VIRTIO_SND_NUM_QUEUES];

    /* Device capabilities (queried during init). */
    uint32_t            num_devices;
    uint32_t            num_pcm_streams;
    uint32_t            pcm_sample_rate;
    uint32_t            pcm_channels;
    uint32_t            pcm_format_bits;

    /* Current playback state. */
    bool                playback_active;
    uint8_t             volume;
} virtio_snd_state_t;

/* Public API. */
int virtio_snd_init(void);
int virtio_snd_play(const uint8_t* data, uint32_t len, uint32_t sample_rate,
                     uint32_t channels, uint32_t bits_per_sample);
void virtio_snd_set_volume(uint8_t vol);
bool virtio_snd_available(void);
void virtio_snd_irq_handler(void);

/* SoundDriver factory function (matches the pattern of sound_pc_speaker_driver, etc.). */
SoundDriver* sound_virtio_driver(void);

/* Get capabilities from the virtio-snd device. */
bool virtio_snd_driver_get_capabilities(SoundDriver* driver, DeviceCapabilities* caps);

#ifdef __cplusplus
}
#endif

#endif /* VIRTIO_SND_H */
