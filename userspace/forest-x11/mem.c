#include "mem.h"
#include <unistd.h>
#include <string.h>

#define HEAP_SIZE (2 * 1024 * 1024)

typedef struct block {
    uint32_t size;
    int      used;
    struct block* next;
} block_t;

static uint8_t* g_heap = NULL;
static uint32_t g_heap_size = 0;
static block_t* g_free_list = NULL;
static uint32_t g_used = 0;

void x11_mem_init(uint32_t size) {
    if (size == 0) size = HEAP_SIZE;
    void* p = sbrk(size);
    if (p == (void*)-1) return;
    g_heap = (uint8_t*)p;
    g_heap_size = size;
    g_free_list = (block_t*)g_heap;
    g_free_list->size = g_heap_size - sizeof(block_t);
    g_free_list->used = 0;
    g_free_list->next = NULL;
    g_used = sizeof(block_t);
}

void* x11_mem_alloc(uint32_t size) {
    if (size == 0) return NULL;
    size = (size + 3) & ~3;
    block_t* blk = g_free_list;
    while (blk) {
        if (!blk->used && blk->size >= size) {
            if (blk->size >= size + sizeof(block_t) + 16) {
                block_t* new_blk = (block_t*)((uint8_t*)blk + sizeof(block_t) + size);
                new_blk->size = blk->size - size - sizeof(block_t);
                new_blk->used = 0;
                new_blk->next = blk->next;
                blk->next = new_blk;
                blk->size = size;
            }
            blk->used = 1;
            g_used += sizeof(block_t) + blk->size;
            return (uint8_t*)blk + sizeof(block_t);
        }
        blk = blk->next;
    }
    return NULL;
}

void x11_mem_free(void* ptr) {
    if (!ptr) return;
    block_t* blk = (block_t*)((uint8_t*)ptr - sizeof(block_t));
    if (blk->used) {
        g_used -= sizeof(block_t) + blk->size;
        blk->used = 0;
    }
    block_t* b = g_free_list;
    while (b) {
        if (!b->used && b->next && !b->next->used) {
            b->size += sizeof(block_t) + b->next->size;
            b->next = b->next->next;
            continue;
        }
        b = b->next;
    }
}

uint32_t x11_mem_used(void) { return g_used; }
uint32_t x11_mem_total(void) { return g_heap_size; }
