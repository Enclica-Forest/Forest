#include "x11_events.h"
#include "x11_protocol.h"
#include <unistd.h>
#include <string.h>

void x11_events_init(void) {}

void x11_event_send(int client_fd, uint8_t type, const uint8_t* data, int len) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = type;
    if (data && len > 1) {
        int c = len > 31 ? 31 : len;
        memcpy(ev + 1, data + 1, c - 1);
    }
    write(client_fd, ev, 32);
}

void x11_send_expose(int client_fd, uint32_t window, int x, int y, int w, int h) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_EXPOSE;
    x11_put32le(ev + 4, window);
    x11_put16le(ev + 8, (uint16_t)x);
    x11_put16le(ev + 10, (uint16_t)y);
    x11_put16le(ev + 12, (uint16_t)w);
    x11_put16le(ev + 14, (uint16_t)h);
    ev[16] = 0;
    write(client_fd, ev, 32);
}

void x11_send_map_notify(int client_fd, uint32_t window) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_MAP_NOTIFY;
    x11_put32le(ev + 4, window);
    x11_put32le(ev + 8, window);
    write(client_fd, ev, 32);
}

void x11_send_unmap_notify(int client_fd, uint32_t window) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_UNMAP_NOTIFY;
    x11_put32le(ev + 4, window);
    x11_put32le(ev + 8, window);
    write(client_fd, ev, 32);
}

void x11_send_configure_notify(int client_fd, uint32_t window,
                               int x, int y, int w, int h, int bw, uint32_t above) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_CONFIGURE_NOTIFY;
    x11_put32le(ev + 4, window);
    x11_put32le(ev + 8, window);
    x11_put16le(ev + 12, (uint16_t)x);
    x11_put16le(ev + 14, (uint16_t)y);
    x11_put16le(ev + 16, (uint16_t)w);
    x11_put16le(ev + 18, (uint16_t)h);
    x11_put16le(ev + 20, (uint16_t)bw);
    x11_put32le(ev + 24, above);
    write(client_fd, ev, 32);
}

void x11_send_destroy_notify(int client_fd, uint32_t event_window, uint32_t window) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_DESTROY_NOTIFY;
    x11_put32le(ev + 4, event_window);
    x11_put32le(ev + 8, window);
    write(client_fd, ev, 32);
}

void x11_send_focus_in(int client_fd, uint32_t window) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_FOCUS_IN;
    x11_put32le(ev + 4, window);
    ev[8] = 1;
    write(client_fd, ev, 32);
}

void x11_send_focus_out(int client_fd, uint32_t window) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_FOCUS_OUT;
    x11_put32le(ev + 4, window);
    ev[8] = 1;
    write(client_fd, ev, 32);
}

void x11_send_enter_notify(int client_fd, uint32_t window, int x, int y) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_ENTER_NOTIFY;
    x11_put32le(ev + 4, window);
    x11_put16le(ev + 8, (uint16_t)x);
    x11_put16le(ev + 10, (uint16_t)y);
    ev[12] = 1;
    write(client_fd, ev, 32);
}

void x11_send_leave_notify(int client_fd, uint32_t window, int x, int y) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_LEAVE_NOTIFY;
    x11_put32le(ev + 4, window);
    x11_put16le(ev + 8, (uint16_t)x);
    x11_put16le(ev + 10, (uint16_t)y);
    ev[12] = 1;
    write(client_fd, ev, 32);
}

void x11_send_key_event(int client_fd, uint8_t type, uint32_t window,
                        uint8_t keycode, uint32_t time_ms) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = type;
    x11_put32le(ev + 4, window);
    x11_put32le(ev + 8, time_ms);
    x11_put32le(ev + 12, window);
    ev[16] = 0;
    ev[17] = keycode;
    write(client_fd, ev, 32);
}

void x11_send_button_event(int client_fd, uint8_t type, uint32_t window,
                           uint8_t button, int x, int y, uint32_t time_ms) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = type;
    x11_put32le(ev + 4, window);
    x11_put32le(ev + 8, time_ms);
    x11_put32le(ev + 12, window);
    x11_put16le(ev + 16, (uint16_t)x);
    x11_put16le(ev + 18, (uint16_t)y);
    ev[20] = button;
    write(client_fd, ev, 32);
}

void x11_send_motion_notify(int client_fd, uint32_t window,
                            int x, int y, uint32_t time_ms) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_MOTION_NOTIFY;
    x11_put32le(ev + 4, window);
    x11_put32le(ev + 8, time_ms);
    x11_put32le(ev + 12, window);
    x11_put16le(ev + 16, (uint16_t)x);
    x11_put16le(ev + 18, (uint16_t)y);
    ev[20] = 0;
    write(client_fd, ev, 32);
}

void x11_send_visibility_notify(int client_fd, uint32_t window, uint8_t state) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_VISIBILITY_NOTIFY;
    x11_put32le(ev + 4, window);
    ev[8] = state;
    write(client_fd, ev, 32);
}

void x11_send_property_notify(int client_fd, uint32_t window, uint32_t atom, uint8_t time) {
    uint8_t ev[32];
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_PROPERTY_NOTIFY;
    x11_put32le(ev + 4, window);
    x11_put32le(ev + 8, atom);
    x11_put32le(ev + 12, time);
    ev[16] = 0;
    write(client_fd, ev, 32);
}
