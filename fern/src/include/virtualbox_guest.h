#ifndef VIRTUALBOX_GUEST_H
#define VIRTUALBOX_GUEST_H

#include "types.h"
#include <stdbool.h>

/* VirtualBox Guest Device PCI IDs */
#define VBOX_VENDOR_ID          0x80EE
#define VBOX_DEVICE_ID          0xCAFE

/* VirtualBox Guest Additions Protocol Version */
#define VBOX_VMMDEV_VERSION     0x00010003
#define VBOX_REQUEST_HEADER_VERSION 0x10001

/* VMMDev Port Offset */
#define VBOX_VMMDEV_PORT_OFFSET 0

/* VirtualBox Request Types */
#define VBOX_REQUEST_INVALID                    0
#define VBOX_REQUEST_GET_MOUSE                  1
#define VBOX_REQUEST_SET_MOUSE                  2
#define VBOX_REQUEST_SET_POINTER_SHAPE         3
#define VBOX_REQUEST_ACKNOWLEDGE_EVENTS         41
#define VBOX_REQUEST_GET_DISPLAY_CHANGE         51
#define VBOX_REQUEST_SET_GUEST_CAPS             55
#define VBOX_REQUEST_GUEST_INFO                 50
#define VBOX_REQUEST_CONNECT                   60
#define VBOX_REQUEST_DISCONNECT                61
#define VBOX_REQUEST_CALL_FUNCTION_32          62
#define VBOX_REQUEST_CALL_FUNCTION_64          63
#define VBOX_REQUEST_CANCEL_REQUEST             64
#define VBOX_REQUEST_SET_VISIBLE_REGION         71

/* Guest Capability Flags */
#define VBOX_GUEST_CAPS_GRAPHICS                (1 << 2)
#define VBOX_GUEST_CAPS_SEAMLESS                (1 << 3)
#define VBOX_GUEST_CAPS_MOUSE                   (1 << 0)

/* Event Flags */
#define VBOX_EVENT_MOUSE_POSITION_CHANGED      (1 << 0)
#define VBOX_EVENT_MOUSE_CAPABILITIES_CHANGED   (1 << 1)
#define VBOX_EVENT_DISPLAY_CHANGE_REQUEST       (1 << 2)
#define VBOX_EVENT_SEAMLESS_MODE_CHANGE_REQUEST (1 << 3)
#define VBOX_EVENT_BACKGROUND_MODE_CHANGE       (1 << 4)

/* Mouse Feature Flags */
#define VBOX_MOUSE_FEATURE_HOST_WANTS_ABSOLUTE (1 << 0)
#define VBOX_MOUSE_FEATURE_GUEST_NEEDS_ABSOLUTE (1 << 4)

/* HGCM Service Locations */
#define VBOX_HGCM_LOC_DEFAULT                  0
#define VBOX_HGCM_LOC_LOCAL                    1
#define VBOX_HGCM_LOC_HOSTSIDE                 2

/* HGCM Service Names */
#define VBOX_HGCM_SERVICE_SHARED_FOLDERS       "VBoxSharedFolders"
#define VBOX_HGCM_SERVICE_CLIPBOARD            "VBoxSharedClipboard"
#define VBOX_HGCM_SERVICE_GUEST_PROPS          "VBoxGuestPropSvc"

/* Request Header */
struct vbox_request_header {
    uint32_t size;
    uint32_t version;
    uint32_t request_type;
    int32_t  rc;
    uint32_t reserved1;
    uint32_t reserved2;
} __attribute__((packed));

/* Guest Info Request */
struct vbox_guest_info {
    struct vbox_request_header header;
    uint32_t version;
    uint32_t ostype;
} __attribute__((packed));

/* Guest Capabilities Request */
struct vbox_guest_caps {
    struct vbox_request_header header;
    uint32_t caps;
} __attribute__((packed));

/* Acknowledge Events Request */
struct vbox_ack_events {
    struct vbox_request_header header;
    uint32_t events;
} __attribute__((packed));

/* Display Change Request */
struct vbox_display_change {
    struct vbox_request_header header;
    uint32_t xres;
    uint32_t yres;
    uint32_t bpp;
    uint32_t eventack;
} __attribute__((packed));

/* Mouse Absolute Request */
struct vbox_mouse_absolute {
    struct vbox_request_header header;
    uint32_t features;
    int32_t  x;
    int32_t  y;
} __attribute__((packed));

/* Visible Region Request */
struct vbox_rtrect {
    int32_t x_left;
    int32_t y_top;
    int32_t x_right;
    int32_t y_bottom;
} __attribute__((packed));

struct vbox_visible_region {
    struct vbox_request_header header;
    uint32_t count;
    struct vbox_rtrect rect[];
} __attribute__((packed));

/* HGCM Connect Request */
struct vbox_hgcm_connect {
    struct vbox_request_header header;
    uint32_t location_type;
    char     location[128];
    uint32_t client_id;
} __attribute__((packed));

/* HGCM Disconnect Request */
struct vbox_hgcm_disconnect {
    struct vbox_request_header header;
    uint32_t client_id;
} __attribute__((packed));

/* HGCM Call Request Header */
struct vbox_hgcm_call_header {
    uint32_t type;
    uint32_t client_id;
    uint32_t function;
    uint32_t param_count;
} __attribute__((packed));

/* HGCM Parameter Types */
#define VBOX_HGCM_PARAM_TYPE_32BIT          1
#define VBOX_HGCM_PARAM_TYPE_64BIT          2
#define VBOX_HGCM_PARAM_TYPE_PHYSICAL_ADDR  3
#define VBOX_HGCM_PARAM_TYPE_LINEAR_ADDR    4
#define VBOX_HGCM_PARAM_TYPE_H2G_LINEAR     5
#define VBOX_HGCM_PARAM_TYPE_G2H_LINEAR     6
#define VBOX_HGCM_PARAM_TYPE_PRELOCKED_LINEAR   7
#define VBOX_HGCM_PARAM_TYPE_H2G_PRELOCKED      8
#define VBOX_HGCM_PARAM_TYPE_G2H_PRELOCKED      9

/* HGCM Parameter (32-bit) */
struct vbox_hgcm_param_32 {
    uint32_t type;
    uint32_t value;
} __attribute__((packed));

/* HGCM Parameter with Buffer */
struct vbox_hgcm_param_buffer {
    uint32_t type;
    uint32_t buffer_size;
    uint32_t buffer_addr;
} __attribute__((packed));

/* Shared Folder Functions */
#define VBOX_SHARED_FOLDER_FUNC_QUERY_MAPPINGS 1
#define VBOX_SHARED_FOLDER_FUNC_QUERY_MAP_NAME  2
#define VBOX_SHARED_FOLDER_FUNC_CREATE_HANDLE   3
#define VBOX_SHARED_FOLDER_FUNC_CLOSE_HANDLE    4
#define VBOX_SHARED_FOLDER_FUNC_READ            5
#define VBOX_SHARED_FOLDER_FUNC_WRITE           6

/* Shared Folder Mapping Record */
struct vbox_shfl_mapping {
    uint32_t flags;
    uint32_t root;
} __attribute__((packed));

/* Display Change Event Data */
struct vbox_display_change_event {
    uint32_t xres;
    uint32_t yres;
    uint32_t bpp;
} __attribute__((packed));

/* Mouse Position Event Data */
struct vbox_mouse_position_event {
    int32_t x;
    int32_t y;
} __attribute__((packed));

/* VirtualBox Guest Device State */
struct vbox_guest_state {
    bool initialized;
    bool pci_found;
    uint16_t pci_vendor;
    uint16_t pci_device;
    
    /* Hardware resources */
    uint32_t mmio_base;
    uint32_t mmio_size;
    uint32_t vmmdev_port;
    uint32_t *vmmdev_mem;
    
    /* Guest capabilities */
    uint32_t guest_caps;
    uint32_t event_mask;
    
    /* Display information */
    struct vbox_display_change_event current_display;
    bool display_resize_enabled;
    
    /* Mouse information */
    struct vbox_mouse_position_event current_mouse;
    bool mouse_absolute_enabled;
    bool mouse_integration_active;
    
    /* Shared folders */
    bool shared_folders_available;
    uint32_t hgcm_client_id;
    
    /* Statistics */
    uint32_t interrupt_count;
    uint32_t display_change_count;
    uint32_t mouse_event_count;
    uint32_t hgcm_call_count;
};

/* Event Callbacks */
typedef void (*vbox_display_change_callback_t)(const struct vbox_display_change_event *event);
typedef void (*vbox_mouse_position_callback_t)(const struct vbox_mouse_position_event *event);

/* Core Functions */
int vbox_guest_init(void);
bool vbox_guest_is_available(void);
void vbox_guest_cleanup(void);

/* Display Functions */
int vbox_enable_display_resize(void);
int vbox_disable_display_resize(void);
void vbox_set_display_change_callback(vbox_display_change_callback_t callback);
int vbox_get_current_display_mode(struct vbox_display_change_event *mode);

/* Mouse Functions */
int vbox_enable_mouse_integration(void);
int vbox_disable_mouse_integration(void);
void vbox_set_mouse_position_callback(vbox_mouse_position_callback_t callback);
bool vbox_is_mouse_integration_active(void);

/* Shared Folders Functions */
int vbox_init_shared_folders(void);
int vbox_query_shared_folders(void);
int vbox_shared_folder_connect(const char *service_name, uint32_t *client_id);
int vbox_shared_folder_disconnect(uint32_t client_id);

/* Event Handling */
void vbox_acknowledge_events(uint32_t events);
void vbox_set_event_mask(uint32_t mask);
uint32_t vbox_get_pending_events(void);

/* Statistics */
void vbox_get_statistics(struct vbox_guest_state *stats);

#endif /* VIRTUALBOX_GUEST_H */