#include "x11_protocol.h"
#include <unistd.h>
#include <string.h>

int x11_read_request(int fd, uint8_t* buf, int max, x11_request_t* req) {
    if (max < 4) return -1;
    int n = read(fd, buf, 4);
    if (n <= 0) return n;
    if (n < 4) return -1;

    req->opcode = buf[0];
    req->data = buf[1];
    req->length = x11_u16le(buf + 2);

    int total = req->length * 4;
    if (total > max) total = max;
    if (total > 4) {
        int got = read(fd, buf + 4, total - 4);
        if (got < 0) return -1;
    }
    req->body_len = total > 4 ? total - 4 : 0;
    if (req->body_len > sizeof(req->body))
        req->body_len = sizeof(req->body);
    if (req->body_len > 0)
        memcpy(req->body, buf + 4, req->body_len);
    return total;
}

void x11_write_reply(int fd, const uint8_t* data, int len) {
    if (len > 0) write(fd, data, len);
}

void x11_write_event(int fd, const x11_event_t* ev) {
    write(fd, ev->data, 32);
}

void x11_reply_init(x11_reply_t* r, uint8_t data_byte, uint16_t seq) {
    memset(r->data, 0, 32);
    r->data[0] = 1;
    r->data[1] = data_byte;
    x11_put16le(r->data + 2, seq);
}

void x11_reply_put8(x11_reply_t* r, int* off, uint8_t v) {
    if (*off < 32) { r->data[*off] = v; (*off)++; }
}

void x11_reply_put16(x11_reply_t* r, int* off, uint16_t v) {
    if (*off + 2 <= 32) { x11_put16le(r->data + *off, v); *off += 2; }
}

void x11_reply_put32(x11_reply_t* r, int* off, uint32_t v) {
    if (*off + 4 <= 32) { x11_put32le(r->data + *off, v); *off += 4; }
}

void x11_event_init(x11_event_t* ev, uint8_t type) {
    memset(ev->data, 0, 32);
    ev->data[0] = type;
}

void x11_event_put8(x11_event_t* ev, int off, uint8_t v) {
    if (off >= 0 && off < 32) ev->data[off] = v;
}

void x11_event_put16(x11_event_t* ev, int off, uint16_t v) {
    if (off >= 0 && off + 2 <= 32) x11_put16le(ev->data + off, v);
}

void x11_event_put32(x11_event_t* ev, int off, uint32_t v) {
    if (off >= 0 && off + 4 <= 32) x11_put32le(ev->data + off, v);
}
