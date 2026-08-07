#ifndef NVME_H
#define NVME_H

#include <stdbool.h>
#include <stdint.h>
#include "types.h"
#include "spinlock.h"

#define NVME_MAX_QUEUES 65536
#define NVME_MAX_IO_QUEUES 65535
#define NVME_MAX_IO_COMPLETION_ENTRIES 65536
#define NVME_MAX_IO_SUBMISSION_ENTRIES 65536
#define NVME_MAX_NAMESPACES 1024
#define NVME_MAX_CMD_SIZE 64
#define NVME_MAX_ADMIN_CMD_SIZE 4096
#define NVME_SECTOR_SIZE 512
#define NVME_SECTOR_SIZE_LBA_SHIFT 9

typedef enum {
    NVME_CSI_NVM = 0x00,
    NVME_CSI_KEEP_ALIVE = 0x01,
    NVME_CSI_CSI_SPECIFIC = 0x02
} nvme_command_set_id_t;

typedef enum {
    NVME_FEAT_ARBITRATION = 0x01,
    NVME_FEAT_POWER_MANAGEMENT = 0x02,
    NVME_FEAT_LBA_RANGE = 0x03,
    NVME_FEAT_NUMBER_OF_QUEUES = 0x07,
    NVME_FEAT_INTERRUPT_COALESCING = 0x08,
    NVME_FEAT_INTERRUPT_VECTOR_CONFIG = 0x09,
    NVME_FEAT_WRITE_ATOMICITY = 0x0A,
    NVME_FEAT_ASYNC_EVENT_CONFIG = 0x0B,
    NVME_FEAT_AUTO_POWER_STATE_TRANSITION = 0x0C,
    NVME_FEAT_HOST_MEMORY_BUFFER = 0x0D,
    NVME_FEAT_TIMESTAMP = 0x0E,
    NVME_FEAT_HOST_IDENTIFIER = 0x1B,
    NVME_FEAT_RESERVED = 0x7F
} nvme_feature_id_t;

typedef enum {
    NVME_LOG_ERROR = 0x01,
    NVME_LOG_HEALTH_INFORMATION = 0x02,
    NVME_LOG_FIRMWARE_SLOT = 0x03,
    NVME_LOG_CHANGED_NAMESPACE_LIST = 0x04,
    NVME_LOG_COMMAND_EFFECTS = 0x05,
    NVME_LOG_DEVICE_SELF_TEST = 0x06,
    NVME_LOG_TELEMETRY_HOST = 0x07,
    NVME_LOG_TELEMETRY_CONTROLLER = 0x08,
    NVME_LOG_ENDURANCE_GROUP_INFORMATION = 0x09,
    NVME_LOG_RESERVED = 0xFF
} nvme_log_page_id_t;

typedef enum {
    NVME_ADMIN_DELETE_IO_SQ = 0x00,
    NVME_ADMIN_CREATE_IO_SQ = 0x01,
    NVME_ADMIN_GET_LOG_PAGE = 0x02,
    NVME_ADMIN_DELETE_IO_CQ = 0x04,
    NVME_ADMIN_CREATE_IO_CQ = 0x05,
    NVME_ADMIN_IDENTIFY = 0x06,
    NVME_ADMIN_ABORT = 0x08,
    NVME_ADMIN_SET_FEATURES = 0x09,
    NVME_ADMIN_GET_FEATURES = 0x0A,
    NVME_ADMIN_ASYNC_EVENT_REQUEST = 0x0C,
    NVME_ADMIN_NAMESPACE_MANAGEMENT = 0x0D,
    NVME_ADMIN_RESERVED = 0x1F,
    NVME_NVM_FLUSH = 0x00,
    NVME_NVM_WRITE = 0x01,
    NVME_NVM_READ = 0x02,
    NVME_NVM_WRITE_UNCORRECTABLE = 0x04,
    NVME_NVM_COMPARE = 0x05,
    NVME_NVM_WRITE_ZEROES = 0x08,
    NVME_NVM_DATASET_MANAGEMENT = 0x09,
    NVME_NVM_RESERVED = 0x7F
} nvme_opcode_t;

typedef enum {
    NVME_STATUS_SUCCESS = 0x0000,
    NVME_STATUS_INVALID_COMMAND_OPCODE = 0x0001,
    NVME_STATUS_INVALID_FIELD = 0x0002,
    NVME_STATUS_COMMAND_ID_CONFLICT = 0x0003,
    NVME_STATUS_DATA_TRANSFER_ERROR = 0x0004,
    NVME_STATUS_COMMANDS_ABORTED_POWER_LOSS = 0x0005,
    NVME_STATUS_INTERNAL_DEVICE_ERROR = 0x0006,
    NVME_STATUS_COMMAND_ABORTED_REQUESTED = 0x0007,
    NVME_STATUS_COMMAND_ABORTED_BY_DEVICE = 0x0008,
    NVME_STATUS_NAMESPACE_NOT_READY = 0x0009,
    NVME_STATUS_INVALID_COMMAND_SEQ = 0x000A,
    NVME_STATUS_LBA_RANGE_OUT_OF_RANGE = 0x0080,
    NVME_STATUS_CAPACITY_EXCEEDED = 0x0081,
    NVME_STATUS_NAMESPACE_NOT_SUPPORTED = 0x0082,
    NVME_STATUS_RESERVED = 0xFFFF
} nvme_status_code_t;

typedef struct {
    uint32_t p;
    uint32_t m;
    uint32_t re;
    uint32_t pp;
} __attribute__((packed)) nvme_identification_version_t;

typedef struct {
    uint64_t ieee58_identifier;
    uint8_t vid;
    uint8_t ssvid;
    uint8_t sn[20];
    uint8_t mn[40];
    uint8_t fr[8];
    uint8_t rab;
    uint8_t ieee[3];
    uint8_t cmic;
    uint8_t mdts;
    uint16_t cntrlid;
    uint32_t ver;
    uint32_t rtd3r;
    uint32_t rtd3e;
    uint32_t oaes;
    uint16_t ctratt;
    uint8_t rsvd1[4];
    uint16_t crdt1;
    uint16_t crdt2;
    uint16_t crdt3;
    uint8_t rsvd2[122];
    uint8_t oem[1344];
    uint8_t rsvd3[0];
} __attribute__((packed)) nvme_identify_controller_t;

typedef struct {
    uint64_t nsze;
    uint64_t ncap;
    uint64_t nuse;
    uint8_t nsfeat;
    uint8_t nlbaf;
    uint8_t flbas;
    uint8_t mc;
    uint8_t dpc;
    uint8_t dps;
    uint8_t nmic;
    uint8_t rescap;
    uint8_t fpi;
    uint8_t rsvd1;
    uint32_t anagrpid;
    uint32_t nsattr;
    uint16_t nvmsetid;
    uint16_t endgid;
    uint8_t nguid[16];
    uint64_t eui64;
    uint8_t lbaf[16][32];
    uint8_t rsvd2[192];
} __attribute__((packed)) nvme_identify_namespace_t;

typedef struct {
    uint16_t mptr;
    uint8_t prp1[2];
    uint8_t rsvd1[2];
    uint32_t nsid;
    uint32_t rsvd2[2];
    uint64_t mpr;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_command_t;

typedef struct {
    uint32_t dword0;
    uint32_t dword1;
    uint32_t dword2;
    uint32_t dword3;
    uint32_t dword4;
    uint32_t dword5;
    uint32_t dword6;
    uint32_t dword7;
} __attribute__((packed)) nvme_completion_t;

typedef struct {
    volatile uint32_t cap_lo;
    volatile uint32_t cap_hi;
    volatile uint32_t version;
    volatile uint32_t intms;
    volatile uint32_t intmc;
    volatile uint32_t config;
    volatile uint32_t status;
    volatile uint32_t nvmss;
    volatile uint32_t aqa;
    volatile uint64_t asq;
    volatile uint64_t acq;
    volatile uint32_t cmbloc;
    volatile uint32_t cmbsz;
    volatile uint32_t bpinfo;
    volatile uint32_t bprsel;
    volatile uint64_t bpmbl;
    volatile uint64_t bpml;
    volatile uint32_t cvf;
    volatile uint32_t rsvd1[2];
    volatile uint32_t vsp;
    volatile uint32_t cap_csts;
    volatile uint32_t nssd;
    volatile uint32_t is;
    uint8_t rsvd2[804];
} __attribute__((packed)) nvme_registers_t;

typedef struct {
    uint16_t sq_id;
    uint16_t sq_head;
    uint16_t size;
    uint16_t qid;
    uint64_t io_base;
    uint16_t head;
    uint16_t tail;
    spinlock_t lock;
    bool valid;
} nvme_queue_t;

typedef struct {
    uint32_t nsid;
    uint64_t start_lba;
    uint64_t end_lba;
    uint8_t flags;
    uint8_t rsvd[7];
    uint8_t type[16];
} nvme_lba_range_t;

typedef struct {
    uint32_t ns_id;
    uint64_t sectors;
    uint32_t sector_size;
    uint32_t formatted_lba_size;
    uint32_t metadata_size;
    uint8_t lba_format;
    uint8_t thin_provisioning;
    bool present;
    bool active;
    nvme_lba_range_t lba_ranges[64];
    uint8_t nguid[16];
    uint8_t eui64[8];
    char *identify_buffer;
} nvme_namespace_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint32_t bar0;
    nvme_registers_t *registers;
    uint32_t caps;
    uint32_t version;
    uint32_t page_size;
    uint32_t max_queue_entries;
    uint32_t admin_queue_size;
    uint32_t num_io_queues;
    nvme_identify_controller_t *identify_ctrl;
    nvme_namespace_t namespaces[NVME_MAX_NAMESPACES];
    uint32_t num_namespaces;
    nvme_queue_t admin_queue;
    nvme_queue_t io_queues[NVME_MAX_IO_QUEUES];
    spinlock_t lock;
    bool initialized;
    bool doorbell_stride;
    uint32_t doorbell_size;
    uint64_t last_tick;
    uint32_t submit_timeout;
    uint32_t completion_timeout;
} nvme_controller_t;

typedef struct {
    nvme_controller_t *controller;
    nvme_queue_t *sq;
    nvme_queue_t *cq;
    nvme_command_t *cmd;
    nvme_completion_t *comp;
    uint16_t cid;
    bool completed;
    uint32_t bytes_transferred;
    uint16_t status;
} nvme_request_t;

typedef void (*nvme_completion_callback_t)(nvme_request_t *req, void *context);

extern nvme_controller_t g_nvme_controller;

bool nvme_init(void);
void nvme_shutdown(void);
bool nvme_detect_controller(void);
nvme_namespace_t *nvme_get_namespace(uint32_t nsid);
int nvme_read(nvme_namespace_t *ns, uint64_t lba, uint32_t count, void *buffer);
int nvme_write(nvme_namespace_t *ns, uint64_t lba, uint32_t count, const void *buffer);
int nvme_flush(nvme_namespace_t *ns);
bool nvme_submit_command(nvme_queue_t *sq, nvme_command_t *cmd, nvme_completion_t *comp, uint32_t timeout_ms);
void nvme_dump_controller(void);
void nvme_dump_namespace(nvme_namespace_t *ns);

#define NVME_BAR_SIZE 0x1000
#define NVME_ADMIN_QUEUE_SIZE 32
#define NVME_TIMEOUT_MS 5000

#define NVME_CAP_LO(a) ((a)[0])
#define NVME_CAP_HI(a) ((a)[1])
#define NVME_CAP_MQES(a) ((a)[0] & 0xFFFF)
#define NVME_CAP_CQR(a) (((a)[0] >> 16) & 0x1)
#define NVME_CAP_AMS(a) (((a)[0] >> 17) & 0x3)
#define NVME_CAP_STRIDE(a) (((a)[0] >> 19) & 0xF)
#define NVME_CAP_PMRS(a) (((a)[0] >> 24) & 0x1)
#define NVME_CAP_CMBS(a) (((a)[0] >> 25) & 0x1)
#define NVME_CAP_MPSMAX(a) (((a)[1] >> 16) & 0xF)
#define NVME_CAP_MPSMIN(a) (((a)[1] >> 20) & 0xF)

#define NVME_STS_RDY(c) ((c) & 0x1)
#define NVME_STS_CFS(c) ((c) >> 1)
#define ST_VALID(c) (((c) >> 0) & 0x1)
#define ST_OVERFLOW(c) (((c) >> 1) & 0x1)
#define ST_ABORT(c) (((c) >> 2) & 0x1)
#define ST_STATUS(c) (((c) >> 17) & 0x7FFF)

#define PRP_ENTRY_SIZE 8
#define MAX_PRP_ENTRIES (NVME_MAX_IO_COMPLETION_ENTRIES * 2)

#endif
