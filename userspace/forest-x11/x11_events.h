#ifndef X11_EVENTS_H
#define X11_EVENTS_H

#include <stdint.h>

#define X11_EVENT_EXPOSE           12
#define X11_EVENT_MAP_NOTIFY       19
#define X11_EVENT_UNMAP_NOTIFY     18
#define X11_EVENT_CONFIGURE_NOTIFY 22
#define X11_EVENT_DESTROY_NOTIFY   17
#define X11_EVENT_FOCUS_IN         9
#define X11_EVENT_FOCUS_OUT        10
#define X11_EVENT_ENTER_NOTIFY     7
#define X11_EVENT_LEAVE_NOTIFY     8
#define X11_EVENT_KEY_PRESS        2
#define X11_EVENT_KEY_RELEASE      3
#define X11_EVENT_BUTTON_PRESS     4
#define X11_EVENT_BUTTON_RELEASE   5
#define X11_EVENT_MOTION_NOTIFY    6
#define X11_EVENT_VISIBILITY_NOTIFY 15
#define X11_EVENT_PROPERTY_NOTIFY  28
#define X11_EVENT_SELECTION_CLEAR  31
#define X11_EVENT_SELECTION_REQUEST 30
#define X11_EVENT_SELECTION_NOTIFY 31
#define X11_EVENT_CLIENT_MESSAGE   33

void x11_events_init(void);
void x11_event_send(int client_fd, uint8_t type, const uint8_t* data, int len);
void x11_send_expose(int client_fd, uint32_t window, int x, int y, int w, int h);
void x11_send_map_notify(int client_fd, uint32_t window);
void x11_send_unmap_notify(int client_fd, uint32_t window);
void x11_send_configure_notify(int client_fd, uint32_t window,
                               int x, int y, int w, int h, int bw, uint32_t above);
void x11_send_destroy_notify(int client_fd, uint32_t event_window, uint32_t window);
void x11_send_focus_in(int client_fd, uint32_t window);
void x11_send_focus_out(int client_fd, uint32_t window);
void x11_send_enter_notify(int client_fd, uint32_t window, int x, int y);
void x11_send_leave_notify(int client_fd, uint32_t window, int x, int y);
void x11_send_key_event(int client_fd, uint8_t type, uint32_t window,
                        uint8_t keycode, uint32_t time_ms);
void x11_send_button_event(int client_fd, uint8_t type, uint32_t window,
                           uint8_t button, int x, int y, uint32_t time_ms);
void x11_send_motion_notify(int client_fd, uint32_t window,
                            int x, int y, uint32_t time_ms);
void x11_send_visibility_notify(int client_fd, uint32_t window, uint8_t state);
void x11_send_property_notify(int client_fd, uint32_t window, uint32_t atom,
                              uint8_t time);

#endif
