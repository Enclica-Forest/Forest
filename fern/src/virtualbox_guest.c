/*
 * VirtualBox Guest Additions Driver for Fern
 * Implements display auto-resize, mouse integration, and shared folders support
 */

#include "include/virtualbox_guest.h"
#include "include/pci.h"
#include "include/interrupt.h"
#include "include/memory_safe.h"
#include "include/debuglog.h"
#include "include/debug.h"
#include "include/panic.h"
#include "include/atomic.h"
#include "include/spinlock.h"
#include "include/cpu_ops.h"
#include "include/types.h"
#include "include/string.h"

static inline void outl(uint16_t port, uint32_t data) {
    outportd(port, data);
}

/* Define IRQ number if not defined */
#ifndef IRQ_VBOX_GUEST
#define IRQ_VBOX_GUEST    9
#endif

/* PCI Configuration Registers */
#define PCI_BAR0                0x10    /* MMIO base address */
#define PCI_BAR1                0x14    /* VMMDev memory base */
#define PCI_INTERRUPT_LINE      0x3C    /* Interrupt line */

/* VMMDev Memory Offsets */
#define VBOX_VMMDEV_EVENTS_OFFSET     0   /* Event flags */
#define VBOX_VMMDEV_ACK_OFFSET         12  /* Event acknowledgment */

/* Request Packet Allocation */
#define VBOX_MAX_PACKET_SIZE    4096
#define VBOX_PACKET_ALIGNMENT   16

/* Global state */
static struct vbox_guest_state vbox_state = {0};
static spinlock_t vbox_lock = SPINLOCK_UNLOCKED;

/* Event callbacks */
static vbox_display_change_callback_t g_display_callback = NULL;
static vbox_mouse_position_callback_t g_mouse_callback = NULL;

/* Forward declarations */
static irq_return_t vbox_interrupt_handler(int vector, void *dev_id, struct interrupt_context *ctx);
static int vbox_find_pci_device(void);
static int vbox_setup_hardware_resources(void);
static void *vbox_allocate_packet(uint32_t *phys_addr, uint32_t size);
static int vbox_send_request(void *packet, uint32_t phys_addr);
static int vbox_init_guest_info(void);
static int vbox_set_guest_capabilities(uint32_t caps);
static int vbox_enable_irq_events(void);

/*
 * Allocate a packet for VMMDev communication
 */
static void *vbox_allocate_packet(uint32_t *phys_addr, uint32_t size)
{
    void *virt_addr;
    
    if (size > VBOX_MAX_PACKET_SIZE) {
        size = VBOX_MAX_PACKET_SIZE;
    }
    
    /* Allocate physically contiguous memory */
    uint32 frame_count = (size + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE;
    uint32 frame = memory_pmm_alloc_frames(frame_count);
    if (frame == 0) {
        return NULL;
    }
    
    *phys_addr = frame * MEMORY_PAGE_SIZE;
    
    /* Map to virtual memory */
    virt_addr = memory_heap_alloc_aligned(size, VBOX_PACKET_ALIGNMENT);
    if (!virt_addr) {
        memory_pmm_free_frames(frame, frame_count);
        return NULL;
    }
    
    /* Zero initialize packet */
    memory_set(virt_addr, 0, size);
    
    return virt_addr;
}

/*
 * Send a request to VMMDev
 */
static int vbox_send_request(void *packet, uint32_t phys_addr)
{
    if (!packet || !vbox_state.vmmdev_port) {
        return -1;
    }
    
    /* Write packet physical address to VMMDev port */
    outl(vbox_state.vmmdev_port, phys_addr);
    
    /* Check return code in packet header */
    struct vbox_request_header *header = (struct vbox_request_header*)packet;
    
    /* Give some time for request to complete */
    for (int i = 0; i < 1000; i++) {
        if (header->rc != 0) {
            break;
        }
        cpu_pause();
    }
    
    return header->rc;
}

/*
 * Find VirtualBox Guest PCI device
 */
static int vbox_find_pci_device(void)
{
    pci_device_t device;
    
    if (pci_find_by_vendor_device(VBOX_VENDOR_ID, VBOX_DEVICE_ID, &device)) {
        vbox_state.pci_found = true;
        vbox_state.pci_vendor = device.vendor_id;
        vbox_state.pci_device = device.device_id;
        
        debug_print("VBOX: Found VirtualBox Guest Device %04x:%04x\n", 
                    device.vendor_id, device.device_id);
        
        /* Get BAR addresses */
        vbox_state.mmio_base = device.bar[0] & ~0xF;
        vbox_state.vmmdev_port = device.bar[0] & ~0xF;
        
        if (device.bar[1] & 1) {
            /* IO space */
            vbox_state.vmmdev_port = device.bar[1] & ~0x3;
        } else {
            /* Memory space */
            uint32_t bar1 = device.bar[1];
            if (bar1 != 0) {
                uint32_t *vmmdev_mem = (uint32_t*)bar1;
                vbox_state.vmmdev_mem = vmmdev_mem;
            }
        }
        
        return 0;
    }
    
    debug_print("VBOX: VirtualBox Guest Device not found\n");
    return -1;
}

/*
 * Setup hardware resources
 */
static int vbox_setup_hardware_resources(void)
{
    /* Map MMIO region if needed */
    if (vbox_state.mmio_base) {
        /* Simple identity mapping for MMIO */
        memory_vmm_map_page(NULL, vbox_state.mmio_base, vbox_state.mmio_base, 
                          MEMORY_PAGE_FLAG_PRESENT | MEMORY_PAGE_FLAG_WRITABLE);
    }
    
    /* Map VMMDev memory region if needed */
    if (vbox_state.vmmdev_mem == NULL && vbox_state.mmio_base) {
        /* Use MMIO base as VMMDev memory if not separately mapped */
        vbox_state.vmmdev_mem = (uint32_t*)vbox_state.mmio_base;
    }
    
    debug_print("VBOX: MMIO base: 0x%08x, VMMDev port: 0x%08x\n",
                vbox_state.mmio_base, vbox_state.vmmdev_port);
    
    return 0;
}

/*
 * Initialize guest info
 */
static int vbox_init_guest_info(void)
{
    uint32_t packet_phys;
    struct vbox_guest_info *info;
    int ret;
    
    info = vbox_allocate_packet(&packet_phys, sizeof(struct vbox_guest_info));
    if (!info) {
        return -1;
    }
    
    /* Fill guest info packet */
    info->header.size = sizeof(struct vbox_guest_info);
    info->header.version = VBOX_REQUEST_HEADER_VERSION;
    info->header.request_type = VBOX_REQUEST_GUEST_INFO;
    info->header.rc = 0;
    info->header.reserved1 = 0;
    info->header.reserved2 = 0;
    info->version = VBOX_VMMDEV_VERSION;
    info->ostype = 0; /* Unknown 32-bit */
    
    ret = vbox_send_request(info, packet_phys);
    
    if (ret == 0) {
        debug_print("VBOX: Guest info initialized successfully\n");
    } else {
        debug_print("VBOX: Guest info initialization failed: %d\n", ret);
    }
    
    return ret;
}

/*
 * Set guest capabilities
 */
static int vbox_set_guest_capabilities(uint32_t caps)
{
    uint32_t packet_phys;
    struct vbox_guest_caps *guest_caps;
    int ret;
    
    guest_caps = vbox_allocate_packet(&packet_phys, sizeof(struct vbox_guest_caps));
    if (!guest_caps) {
        return -1;
    }
    
    /* Fill guest capabilities packet */
    guest_caps->header.size = sizeof(struct vbox_guest_caps);
    guest_caps->header.version = VBOX_REQUEST_HEADER_VERSION;
    guest_caps->header.request_type = VBOX_REQUEST_SET_GUEST_CAPS;
    guest_caps->header.rc = 0;
    guest_caps->header.reserved1 = 0;
    guest_caps->header.reserved2 = 0;
    guest_caps->caps = caps;
    
    ret = vbox_send_request(guest_caps, packet_phys);
    
    if (ret == 0) {
        vbox_state.guest_caps = caps;
        debug_print("VBOX: Guest capabilities set: 0x%08x\n", caps);
    } else {
        debug_print("VBOX: Failed to set guest capabilities: %d\n", ret);
    }
    
    return ret;
}

/*
 * Enable IRQ events
 */
static int vbox_enable_irq_events(void)
{
    if (vbox_state.vmmdev_mem) {
        /* Enable all events initially */
        vbox_state.vmmdev_mem[3] = 0xFFFFFFFF;
        vbox_state.event_mask = 0xFFFFFFFF;
        
        debug_print("VBOX: IRQ events enabled\n");
        return 0;
    }
    
    return -1;
}

/*
 * Interrupt handler for VirtualBox Guest Device
 */
static irq_return_t vbox_interrupt_handler(int vector, void *dev_id, struct interrupt_context *ctx)
{
    unsigned long flags;
    uint32_t events;
    
    (void)vector;
    (void)dev_id;
    (void)ctx;
    
    spin_lock_irqsave(&vbox_lock, flags);
    
    vbox_state.interrupt_count++;
    
    /* Check if there are pending events */
    if (vbox_state.vmmdev_mem) {
        events = vbox_state.vmmdev_mem[2];
        
        if (events == 0) {
            spin_unlock_irqrestore(&vbox_lock, flags);
            return IRQ_NONE;
        }
        
        /* Handle different event types */
        if (events & VBOX_EVENT_DISPLAY_CHANGE_REQUEST) {
            vbox_state.display_change_count++;
            
            /* Query display change information */
            uint32_t packet_phys;
            struct vbox_display_change *display_change;
            
            display_change = vbox_allocate_packet(&packet_phys, sizeof(struct vbox_display_change));
            if (display_change) {
                display_change->header.size = sizeof(struct vbox_display_change);
                display_change->header.version = VBOX_REQUEST_HEADER_VERSION;
                display_change->header.request_type = VBOX_REQUEST_GET_DISPLAY_CHANGE;
                display_change->header.rc = 0;
                display_change->header.reserved1 = 0;
                display_change->header.reserved2 = 0;
                display_change->xres = 0;
                display_change->yres = 0;
                display_change->bpp = 0;
                display_change->eventack = 1;
                
                if (vbox_send_request(display_change, packet_phys) == 0) {
                    vbox_state.current_display.xres = display_change->xres;
                    vbox_state.current_display.yres = display_change->yres;
                    vbox_state.current_display.bpp = display_change->bpp;
                    
                    if (g_display_callback) {
                        g_display_callback(&vbox_state.current_display);
                    }
                    
                    debug_print("VBOX: Display change: %dx%dx%d\n", 
                               display_change->xres, display_change->yres, display_change->bpp);
                }
            }
        }
        
        if (events & VBOX_EVENT_MOUSE_POSITION_CHANGED) {
            vbox_state.mouse_event_count++;
            
            /* Query mouse position */
            uint32_t packet_phys;
            struct vbox_mouse_absolute *mouse_pos;
            
            mouse_pos = vbox_allocate_packet(&packet_phys, sizeof(struct vbox_mouse_absolute));
            if (mouse_pos) {
                mouse_pos->header.size = sizeof(struct vbox_mouse_absolute);
                mouse_pos->header.version = VBOX_REQUEST_HEADER_VERSION;
                mouse_pos->header.request_type = VBOX_REQUEST_GET_MOUSE;
                mouse_pos->header.rc = 0;
                mouse_pos->header.reserved1 = 0;
                mouse_pos->header.reserved2 = 0;
                mouse_pos->features = 0;
                mouse_pos->x = 0;
                mouse_pos->y = 0;
                
                if (vbox_send_request(mouse_pos, packet_phys) == 0) {
                    /* Scale coordinates from 0-0xFFFF range to actual pixel coordinates */
                    if (vbox_state.current_display.xres > 0 && vbox_state.current_display.yres > 0) {
                        vbox_state.current_mouse.x = (mouse_pos->x * vbox_state.current_display.xres) / 0xFFFF;
                        vbox_state.current_mouse.y = (mouse_pos->y * vbox_state.current_display.yres) / 0xFFFF;
                        
                        if (g_mouse_callback) {
                            g_mouse_callback(&vbox_state.current_mouse);
                        }
                    }
                }
            }
        }
        
        /* Acknowledge events */
        vbox_acknowledge_events(events);
    }
    
    spin_unlock_irqrestore(&vbox_lock, flags);
    
    return IRQ_HANDLED;
}

/*
 * Acknowledge events to host
 */
void vbox_acknowledge_events(uint32_t events)
{
    uint32_t packet_phys;
    struct vbox_ack_events *ack;
    
    ack = vbox_allocate_packet(&packet_phys, sizeof(struct vbox_ack_events));
    if (!ack) {
        return;
    }
    
    ack->header.size = sizeof(struct vbox_ack_events);
    ack->header.version = VBOX_REQUEST_HEADER_VERSION;
    ack->header.request_type = VBOX_REQUEST_ACKNOWLEDGE_EVENTS;
    ack->header.rc = 0;
    ack->header.reserved1 = 0;
    ack->header.reserved2 = 0;
    ack->events = events;
    
    vbox_send_request(ack, packet_phys);
}

/*
 * Initialize VirtualBox Guest Additions
 */
int vbox_guest_init(void)
{
    unsigned long flags;
    int ret;
    
    debug_print("VBOX: Initializing VirtualBox Guest Additions\n");
    
    if (vbox_state.initialized) {
        return 0;
    }
    
    spin_lock_irqsave(&vbox_lock, flags);
    
    /* Find PCI device */
    ret = vbox_find_pci_device();
    if (ret != 0) {
        spin_unlock_irqrestore(&vbox_lock, flags);
        return ret;
    }
    
    /* Setup hardware resources */
    ret = vbox_setup_hardware_resources();
    if (ret != 0) {
        spin_unlock_irqrestore(&vbox_lock, flags);
        return ret;
    }
    
    /* Initialize guest info */
    ret = vbox_init_guest_info();
    if (ret != 0) {
        spin_unlock_irqrestore(&vbox_lock, flags);
        return ret;
    }
    
    /* Register interrupt handler */
    idt_register_handler(IRQ_VBOX_GUEST, vbox_interrupt_handler, "VBOX_GUEST");
    
    /* Enable IRQ */
    if (pic_is_available()) {
        pic_unmask_irq(IRQ_VBOX_GUEST);
    } else if (ioapic_is_available()) {
        ioapic_enable_irq(IRQ_VBOX_GUEST);
    }
    
    /* Enable IRQ events */
    vbox_enable_irq_events();
    
    vbox_state.initialized = true;
    
    spin_unlock_irqrestore(&vbox_lock, flags);
    
    debug_print("VBOX: VirtualBox Guest Additions initialized\n");
    return 0;
}

/*
 * Check if VirtualBox Guest Additions are available
 */
bool vbox_guest_is_available(void)
{
    return vbox_state.initialized && vbox_state.pci_found;
}

/*
 * Enable display auto-resize
 */
int vbox_enable_display_resize(void)
{
    uint32_t caps = VBOX_GUEST_CAPS_GRAPHICS;
    
    if (!vbox_guest_is_available()) {
        return -1;
    }
    
    int ret = vbox_set_guest_capabilities(caps);
    if (ret == 0) {
        vbox_state.display_resize_enabled = true;
        debug_print("VBOX: Display auto-resize enabled\n");
    }
    
    return ret;
}

/*
 * Disable display auto-resize
 */
int vbox_disable_display_resize(void)
{
    if (!vbox_guest_is_available()) {
        return -1;
    }
    
    int ret = vbox_set_guest_capabilities(vbox_state.guest_caps & ~VBOX_GUEST_CAPS_GRAPHICS);
    if (ret == 0) {
        vbox_state.display_resize_enabled = false;
        debug_print("VBOX: Display auto-resize disabled\n");
    }
    
    return ret;
}

/*
 * Enable mouse integration
 */
int vbox_enable_mouse_integration(void)
{
    uint32_t caps = VBOX_GUEST_CAPS_MOUSE;
    uint32_t packet_phys;
    struct vbox_mouse_absolute *mouse;
    int ret;
    
    if (!vbox_guest_is_available()) {
        return -1;
    }
    
    /* Set mouse capabilities */
    mouse = vbox_allocate_packet(&packet_phys, sizeof(struct vbox_mouse_absolute));
    if (!mouse) {
        return -1;
    }
    
    mouse->header.size = sizeof(struct vbox_mouse_absolute);
    mouse->header.version = VBOX_REQUEST_HEADER_VERSION;
    mouse->header.request_type = VBOX_REQUEST_SET_MOUSE;
    mouse->header.rc = 0;
    mouse->header.reserved1 = 0;
    mouse->header.reserved2 = 0;
    mouse->features = VBOX_MOUSE_FEATURE_GUEST_NEEDS_ABSOLUTE | VBOX_MOUSE_FEATURE_HOST_WANTS_ABSOLUTE;
    mouse->x = 0;
    mouse->y = 0;
    
    ret = vbox_send_request(mouse, packet_phys);
    
    if (ret == 0) {
        /* Enable mouse capability in guest capabilities */
        ret = vbox_set_guest_capabilities(vbox_state.guest_caps | caps);
        if (ret == 0) {
            vbox_state.mouse_absolute_enabled = true;
            vbox_state.mouse_integration_active = true;
            debug_print("VBOX: Mouse integration enabled\n");
        }
    }
    
    return ret;
}

/*
 * Disable mouse integration
 */
int vbox_disable_mouse_integration(void)
{
    if (!vbox_guest_is_available()) {
        return -1;
    }
    
    int ret = vbox_set_guest_capabilities(vbox_state.guest_caps & ~VBOX_GUEST_CAPS_MOUSE);
    if (ret == 0) {
        vbox_state.mouse_absolute_enabled = false;
        vbox_state.mouse_integration_active = false;
        debug_print("VBOX: Mouse integration disabled\n");
    }
    
    return ret;
}

/*
 * Set display change callback
 */
void vbox_set_display_change_callback(vbox_display_change_callback_t callback)
{
    unsigned long flags;
    
    spin_lock_irqsave(&vbox_lock, flags);
    g_display_callback = callback;
    spin_unlock_irqrestore(&vbox_lock, flags);
}

/*
 * Set mouse position callback
 */
void vbox_set_mouse_position_callback(vbox_mouse_position_callback_t callback)
{
    unsigned long flags;
    
    spin_lock_irqsave(&vbox_lock, flags);
    g_mouse_callback = callback;
    spin_unlock_irqrestore(&vbox_lock, flags);
}

/*
 * Get current display mode
 */
int vbox_get_current_display_mode(struct vbox_display_change_event *mode)
{
    if (!vbox_guest_is_available() || !mode) {
        return -1;
    }
    
    *mode = vbox_state.current_display;
    return 0;
}

/*
 * Check if mouse integration is active
 */
bool vbox_is_mouse_integration_active(void)
{
    return vbox_state.mouse_integration_active;
}

/*
 * Get statistics
 */
void vbox_get_statistics(struct vbox_guest_state *stats)
{
    if (!stats) {
        return;
    }
    
    unsigned long flags;
    spin_lock_irqsave(&vbox_lock, flags);
    *stats = vbox_state;
    spin_unlock_irqrestore(&vbox_lock, flags);
}

/*
 * Cleanup VirtualBox Guest Additions
 */
void vbox_guest_cleanup(void)
{
    if (!vbox_state.initialized) {
        return;
    }
    
    debug_print("VBOX: Cleaning up VirtualBox Guest Additions\n");
    
    /* Disable features */
    vbox_disable_display_resize();
    vbox_disable_mouse_integration();
    
    /* Disable interrupts */
    if (pic_is_available()) {
        pic_mask_irq(IRQ_VBOX_GUEST);
    } else if (ioapic_is_available()) {
        ioapic_disable_irq(IRQ_VBOX_GUEST);
    }
    
    memory_set((uint8_t*)&vbox_state, 0, sizeof(vbox_state));
}

/* -------------------------------------------------------------------------- *
 * Clipboard / mouse-set extensions used by vm_guest.c
 *
 * These reach into the host via the HGCM transport already used by the rest
 * of the driver. They return the VMMDev return code so callers can detect
 * host-side absence of the service without crashing.
 * -------------------------------------------------------------------------- */

#ifndef VBOX_HGCM_CLIPBOARD_CONNECT
#  define VBOX_HGCM_CLIPBOARD_CONNECT    0
#  define VBOX_HGCM_CLIPBOARD_DISCONNECT 1
#  define VBOX_HGCM_CLIPBOARD_WRITE_DATA 2
#  define VBOX_HGCM_CLIPBOARD_READ_DATA  3
#endif

/* VBox HGCM call needs a connection to the host service first. We borrow an
 * HGCM connect request against VBOX_HGCM_SERVICE_CLIPBOARD and cache the
 * client id on the global state. */
static int vbox_clipboard_connect(uint32_t *out_client_id)
{
    uint32_t packet_phys;
    struct vbox_hgcm_connect *conn;
    int ret;

    if (!out_client_id || !vbox_guest_is_available()) return -1;
    if (vbox_state.hgcm_client_id != 0) {
        *out_client_id = vbox_state.hgcm_client_id;
        return 0;
    }

    conn = vbox_allocate_packet(&packet_phys, sizeof(*conn));
    if (!conn) return -1;

    conn->header.size      = sizeof(*conn);
    conn->header.version   = VBOX_REQUEST_HEADER_VERSION;
    conn->header.request_type = VBOX_REQUEST_CONNECT;
    conn->header.rc        = 0;
    conn->location_type    = VBOX_HGCM_LOC_DEFAULT;
    /* location holds the service name; fill it in. */
    memory_set((uint8_t *)conn->location, 0, sizeof(conn->location));
    uint32 i = 0;
    const char *svc = VBOX_HGCM_SERVICE_CLIPBOARD;
    while (svc[i] && i < sizeof(conn->location) - 1) {
        conn->location[i] = svc[i];
        i++;
    }
    conn->client_id = 0;

    ret = vbox_send_request(conn, packet_phys);
    if (ret == 0) {
        vbox_state.hgcm_client_id = conn->client_id;
        *out_client_id = conn->client_id;
        debug_print("VBOX: clipboard HGCM client id = %u\n", conn->client_id);
        return 0;
    }
    debug_print("VBOX: clipboard connect failed (rc=%d)\n", ret);
    return ret;
}

/* Uniformly-named entry points for vm_guest.c. */
int vbox_clipboard_send(const void *buf, uint32_t len)
{
    uint32_t client_id;
    int ret;

    if (!buf || len == 0) return -1;
    if (!vbox_guest_is_available()) return -1;
    ret = vbox_clipboard_connect(&client_id);
    if (ret != 0) return ret;

    /* Pack a minimal HGCM call: WRITE_DATA with a flat buffer parameter. */
    struct {
        struct vbox_request_header header;
        uint32_t type;       /* HGCM call function number */
        uint32_t client_id;
        uint32_t function;
        uint32_t param_count;
        struct vbox_hgcm_param_buffer param;
    } __attribute__((packed)) *call;

    uint32_t packet_phys;
    call = vbox_allocate_packet(&packet_phys, sizeof(*call) + len);
    if (!call) return -1;

    call->header.size         = sizeof(*call) + len;
    call->header.version      = VBOX_REQUEST_HEADER_VERSION;
    call->header.request_type = VBOX_REQUEST_CALL_FUNCTION_32;
    call->header.rc           = 0;
    call->type                = 0;
    call->client_id           = client_id;
    call->function            = VBOX_HGCM_CLIPBOARD_WRITE_DATA;
    call->param_count         = 1;
    call->param.type          = VBOX_HGCM_PARAM_TYPE_LINEAR_ADDR;
    call->param.buffer_size   = len;
    call->param.buffer_addr   = packet_phys + sizeof(*call);

    uint8 *payload = (uint8 *)(call + 1);
    memcpy(payload, buf, len);

    ret = vbox_send_request(call, packet_phys);
    return ret;
}

int vbox_clipboard_recv(void *buf, uint32_t maxlen, uint32_t *out_len)
{
    uint32_t client_id;
    int ret;

    if (!buf || maxlen == 0) return -1;
    if (!vbox_guest_is_available()) return -1;
    ret = vbox_clipboard_connect(&client_id);
    if (ret != 0) return ret;

    /* READ_DATA returns format + size; we copy up to maxlen bytes. */
    struct {
        struct vbox_request_header header;
        uint32_t type;
        uint32_t client_id;
        uint32_t function;
        uint32_t param_count;
        struct vbox_hgcm_param_buffer param;
    } __attribute__((packed)) *call;
    uint32_t packet_phys;

    call = vbox_allocate_packet(&packet_phys, sizeof(*call) + maxlen);
    if (!call) return -1;

    call->header.size         = sizeof(*call) + maxlen;
    call->header.version      = VBOX_REQUEST_HEADER_VERSION;
    call->header.request_type = VBOX_REQUEST_CALL_FUNCTION_32;
    call->header.rc           = 0;
    call->type                = 0;
    call->client_id           = client_id;
    call->function            = VBOX_HGCM_CLIPBOARD_READ_DATA;
    call->param_count         = 1;
    call->param.type          = VBOX_HGCM_PARAM_TYPE_LINEAR_ADDR;
    call->param.buffer_size   = maxlen;
    call->param.buffer_addr   = packet_phys + sizeof(*call);

    ret = vbox_send_request(call, packet_phys);
    if (ret == 0) {
        uint32_t copied = call->param.buffer_size;
        if (copied > maxlen) copied = maxlen;
        uint8 *payload = (uint8 *)(call + 1);
        memcpy(buf, payload, copied);
        if (out_len) *out_len = copied;
    }
    return ret;
}

int vbox_mouse_set(int32_t x, int32_t y, int absolute)
{
    if (!vbox_guest_is_available()) return -1;

    uint32_t packet_phys;
    struct vbox_mouse_absolute *mouse;
    mouse = vbox_allocate_packet(&packet_phys, sizeof(*mouse));
    if (!mouse) return -1;

    mouse->header.size         = sizeof(*mouse);
    mouse->header.version      = VBOX_REQUEST_HEADER_VERSION;
    mouse->header.request_type = VBOX_REQUEST_SET_MOUSE;
    mouse->header.rc           = 0;
    mouse->features = absolute
        ? (VBOX_MOUSE_FEATURE_GUEST_NEEDS_ABSOLUTE | VBOX_MOUSE_FEATURE_HOST_WANTS_ABSOLUTE)
        : 0;
    mouse->x = x;
    mouse->y = y;

    return vbox_send_request(mouse, packet_phys);
}