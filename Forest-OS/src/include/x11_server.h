#ifndef X11_SERVER_H
#define X11_SERVER_H

#include "types.h"
#include "input_event.h"

/* ------------------------------------------------------------------ */
/*  X11 request opcodes                                                 */
/* ------------------------------------------------------------------ */
typedef enum {
    X11_REQ_CREATE_WINDOW        = 1,
    X11_REQ_CHANGE_WINDOW_ATTR   = 2,
    X11_REQ_GET_WINDOW_ATTR      = 3,
    X11_REQ_DESTROY_WINDOW       = 4,
    X11_REQ_MAP_WINDOW           = 8,
    X11_REQ_UNMAP_WINDOW         = 10,
    X11_REQ_CONFIGURE_WINDOW     = 12,
    X11_REQ_GET_GEOMETRY         = 14,
    X11_REQ_QUERY_TREE           = 15,
    X11_REQ_INTERN_ATOM          = 16,
    X11_REQ_GET_ATOM_NAME        = 17,
    X11_REQ_CHANGE_PROPERTY      = 18,
    X11_REQ_GET_PROPERTY         = 20,
    X11_REQ_SET_INPUT_FOCUS      = 42,
    X11_REQ_GET_INPUT_FOCUS      = 43,
    X11_REQ_QUERY_EXTENSION      = 98,
    X11_REQ_LIST_EXTENSIONS      = 99,
    X11_REQ_CREATE_PIXMAP        = 53,
    X11_REQ_FREE_PIXMAP          = 54,
    X11_REQ_CREATE_GC            = 55,
    X11_REQ_CHANGE_GC            = 56,
    X11_REQ_FREE_GC              = 60,
    X11_REQ_CLEAR_AREA           = 61,
    X11_REQ_COPY_AREA            = 62,
    X11_REQ_POLY_FILL_RECTANGLE  = 70,
    X11_REQ_PUT_IMAGE            = 72,
    X11_REQ_GET_IMAGE            = 73,
    X11_REQ_POLY_TEXT8           = 74,
    X11_REQ_IMAGE_TEXT8          = 76
} x11_request_opcode_t;

/* ------------------------------------------------------------------ */
/*  X11 event codes                                                     */
/* ------------------------------------------------------------------ */
#define X11_EVENT_KEY_PRESS        2
#define X11_EVENT_KEY_RELEASE      3
#define X11_EVENT_BUTTON_PRESS     4
#define X11_EVENT_BUTTON_RELEASE   5
#define X11_EVENT_MOTION_NOTIFY    6
#define X11_EVENT_EXPOSE           12
#define X11_EVENT_CLIENT_MESSAGE   33

/* X11 event mask bits */
#define X11_MASK_KEY_PRESS         0x00000001UL
#define X11_MASK_KEY_RELEASE       0x00000002UL
#define X11_MASK_BUTTON_PRESS      0x00000004UL
#define X11_MASK_BUTTON_RELEASE    0x00000008UL
#define X11_MASK_POINTER_MOTION    0x00000040UL
#define X11_MASK_EXPOSURE          0x00008000UL
#define X11_MASK_STRUCTURE_NOTIFY  0x00020000UL

/* ------------------------------------------------------------------ */
/*  Predefined X11 atoms                                               */
/* ------------------------------------------------------------------ */
#define X11_ATOM_NONE              0
#define X11_ATOM_STRING            31
#define X11_ATOM_WM_PROTOCOLS      1001
#define X11_ATOM_WM_DELETE_WINDOW  1002
#define X11_ATOM_NET_WM_NAME       1003
#define X11_ATOM_UTF8_STRING       1004

/* ------------------------------------------------------------------ */
/*  IPC channel (in-kernel ring buffer per client)                      */
/* ------------------------------------------------------------------ */
#define X11_IPC_BUF_SIZE           8192   /* bytes per direction */

typedef struct {
    uint8  data[X11_IPC_BUF_SIZE];
    uint32 head;   /* producer writes here */
    uint32 tail;   /* consumer reads here  */
} x11_ipc_ring_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/* Lifecycle */
void x11_server_init(void);
void x11_server_shutdown(void);
void x11_server_loop(void);
void x11_server_pump(uint32 max_events);

/*
 * Client channel access.
 * x11_client_connect() is called by the userspace shim / syscall layer
 * when a process opens /tmp/.X11-unix/X0.  It returns a client-id >= 0
 * or -1 on failure.
 */
int  x11_client_connect(void);
void x11_client_disconnect(int client_id);

/*
 * Feed raw bytes from the client into the server.
 * Called by the IPC / socket read path each time the client writes.
 * Returns 0 on success, -1 on error.
 */
int  x11_client_write(int client_id, const uint8 *data, uint32 len);

/*
 * Read bytes the server wants to send back to the client (replies/events).
 * Returns number of bytes copied into buf (may be 0 if no pending output).
 */
uint32 x11_client_read(int client_id, uint8 *buf, uint32 max_len);

/* Legacy direct-call API kept for compatibility */
int    x11_handle_connection(int client_fd);
int    x11_handle_request(int client_fd, const uint8 *request, uint32 length);

/* Window management */
uint32 x11_create_window(uint32 parent, int16 x, int16 y,
                         uint16 width, uint16 height, uint32 event_mask);
int    x11_map_window(uint32 window_id);
int    x11_unmap_window(uint32 window_id);
int    x11_destroy_window(uint32 window_id);
int    x11_configure_window(uint32 window_id, int16 x, int16 y,
                            uint16 width, uint16 height);
int    x11_get_window_info(uint32 window_id, int16 *x, int16 *y,
                           uint16 *width, uint16 *height, bool *mapped);

/* Event injection from device drivers */
void x11_send_expose(uint32 window_id);
void x11_dispatch_keyboard(uint8 keycode, bool pressed);
void x11_dispatch_pointer(int16 dx, int16 dy, uint8 buttons);

/* Input-mux callback – registers the X11 server as a consumer */
void x11_input_event_callback(const input_event_t *event, void *ctx);

#endif /* X11_SERVER_H */
