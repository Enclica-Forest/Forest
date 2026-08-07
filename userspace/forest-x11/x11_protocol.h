#ifndef X11_PROTOCOL_H
#define X11_PROTOCOL_H

#include <stdint.h>

#define X11_MAX_REQUEST_SIZE 65536

static inline uint16_t x11_u16le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t x11_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int32_t x11_i32le(const uint8_t* p) {
    return (int32_t)x11_u32le(p);
}

static inline void x11_put16le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static inline void x11_put32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void x11_put16be(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static inline void x11_put32be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

typedef struct {
    uint8_t  opcode;
    uint8_t  data;
    uint16_t length;
    uint8_t  body[256];
    uint32_t body_len;
} x11_request_t;

typedef struct {
    uint8_t  data[32];
} x11_event_t;

typedef struct {
    uint8_t  data[32];
} x11_reply_t;

int  x11_read_request(int fd, uint8_t* buf, int max, x11_request_t* req);
void x11_write_reply(int fd, const uint8_t* data, int len);
void x11_write_event(int fd, const x11_event_t* ev);

void x11_reply_init(x11_reply_t* r, uint8_t data_byte, uint16_t seq);
void x11_reply_put8(x11_reply_t* r, int* off, uint8_t v);
void x11_reply_put16(x11_reply_t* r, int* off, uint16_t v);
void x11_reply_put32(x11_reply_t* r, int* off, uint32_t v);

void x11_event_init(x11_event_t* ev, uint8_t type);
void x11_event_put8(x11_event_t* ev, int off, uint8_t v);
void x11_event_put16(x11_event_t* ev, int off, uint16_t v);
void x11_event_put32(x11_event_t* ev, int off, uint32_t v);

#endif
