#ifndef GPU_ACCEL_H
#define GPU_ACCEL_H

#include "graphics_types.h"

typedef enum {
    GPU_ACCEL_NONE = 0,
    GPU_ACCEL_SOFTWARE,
    GPU_ACCEL_VESA,
    GPU_ACCEL_VMWARE,
    GPU_ACCEL_BOCHS,
    GPU_ACCEL_INTEL,
    GPU_ACCEL_NVIDIA,
    GPU_ACCEL_AMD,
} gpu_accel_type_t;

typedef struct {
    gpu_accel_type_t type;
    bool available;
    bool hw_cursor;
    bool hw_blend;
    bool hw_scale;
    uint32_t vram_size;
    uint32_t max_texture_size;
    void (*fill_rect)(int x, int y, int w, int h, uint32_t color);
    void (*blit)(void* src, int sx, int sy, int sw, int sh, int dx, int dy);
    void (*blend)(void* src, int sx, int sy, int sw, int sh, int dx, int dy, uint8_t alpha);
    void (*set_cursor)(int x, int y, void* bitmap, uint32_t w, uint32_t h);
    void (*move_cursor)(int x, int y);
    void (*swap_buffers)(void);
} gpu_accel_context_t;

graphics_result_t gpu_accel_init(void);
gpu_accel_context_t* gpu_accel_get_context(void);
void gpu_accel_fill_rect(int x, int y, int w, int h, uint32_t color);
void gpu_accel_blit(void* src, int sx, int sy, int sw, int sh, int dx, int dy);
void gpu_accel_blend(void* src, int sx, int sy, int sw, int sh, int dx, int dy, uint8_t alpha);
void gpu_accel_set_cursor(int x, int y, void* bitmap, uint32_t w, uint32_t h);
void gpu_accel_move_cursor(int x, int y);
void gpu_accel_swap_buffers(void);

#endif // GPU_ACCEL_H
