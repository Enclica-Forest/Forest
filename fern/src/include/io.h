/**
 * @file io.h
 * @brief I/O Port Access Compatibility Header
 * 
 * Provides common inb/outb naming convention as aliases to
 * the kernel's inportb/outportb functions defined in system.h
 */

#ifndef IO_H
#define IO_H

#include "system.h"

// Byte-sized I/O port operations
static inline uint8_t inb(uint16_t port) {
    return inportb(port);
}

static inline void outb(uint16_t port, uint8_t data) {
    outportb(port, data);
}

// Word-sized I/O port operations
static inline uint16_t inw(uint16_t port) {
    return inportw(port);
}

static inline void outw(uint16_t port, uint16_t data) {
    outportw(port, data);
}

// Double-word I/O port operations
static inline uint32_t inl(uint16_t port) {
    return inportd(port);
}

static inline void outl(uint16_t port, uint32_t data) {
    outportd(port, data);
}

#endif // IO_H
