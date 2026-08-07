#ifndef X11_MEM_H
#define X11_MEM_H

#include <stdint.h>

void x11_mem_init(uint32_t size);
void* x11_mem_alloc(uint32_t size);
void  x11_mem_free(void* ptr);
uint32_t x11_mem_used(void);
uint32_t x11_mem_total(void);

#endif
