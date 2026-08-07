#ifndef PARALLEL_GRAPHICS_H
#define PARALLEL_GRAPHICS_H

#include "graphics_types.h"
#include "../smp.h"
#include "../atomic.h"
#include "../spinlock.h"

#define PARALLEL_MAX_WORKERS (SMP_MAX_CPUS - 1)
#define PARALLEL_MAX_QUEUED_JOBS 256

typedef enum {
    PARALLEL_JOB_TYPE_RECT_FILL,
    PARALLEL_JOB_TYPE_RECT_COPY,
    PARALLEL_JOB_TYPE_LINE,
    PARALLEL_JOB_TYPE_CIRCLE,
    PARALLEL_JOB_TYPE_BLIT,
    PARALLEL_JOB_TYPE_CUSTOM
} parallel_job_type_t;

typedef struct parallel_rect_fill_job {
    graphics_rect_t rect;
    graphics_color_t color;
    uint32_t* pixel_value;
} parallel_rect_fill_job_t;

typedef struct parallel_rect_copy_job {
    const uint8_t* src;
    uint8_t* dst;
    uint32_t src_pitch;
    uint32_t dst_pitch;
    graphics_rect_t rect;
    uint32_t bytes_per_pixel;
} parallel_rect_copy_job_t;

typedef struct parallel_line_job {
    int32_t x1, y1, x2, y2;
    graphics_color_t color;
} parallel_line_job_t;

typedef struct parallel_circle_job {
    int32_t cx, cy, radius;
    graphics_color_t color;
    bool filled;
    bool antialiased;
} parallel_circle_job_t;

typedef struct parallel_blit_job {
    const graphics_surface_t* src;
    graphics_surface_t* dst;
    graphics_rect_t src_rect;
    int32_t dst_x, dst_y;
} parallel_blit_job_t;

typedef struct parallel_job {
    parallel_job_type_t type;
    union {
        parallel_rect_fill_job_t fill;
        parallel_rect_copy_job_t copy;
        parallel_line_job_t line;
        parallel_circle_job_t circle;
        parallel_blit_job_t blit;
    };
    void (*custom_func)(void* data);
    void* custom_data;
    volatile bool completed;
    atomic32_t ref_count;
} parallel_job_t;

typedef struct parallel_worker {
    uint32 cpu_id;
    struct thread* thread;
    volatile bool running;
    volatile bool idle;
    atomic32_t jobs_processed;
    spinlock_t lock;
} parallel_worker_t;

typedef struct {
    bool initialized;
    uint32 num_workers;
    parallel_worker_t workers[PARALLEL_MAX_WORKERS];
    
    parallel_job_t job_queue[PARALLEL_MAX_QUEUED_JOBS];
    uint32 queue_head;
    uint32 queue_tail;
    spinlock_t queue_lock;
    
    atomic32_t active_jobs;
    atomic32_t total_jobs;
    
    graphics_surface_t* target_surface;
    spinlock_t surface_lock;
    
    struct {
        uint32 jobs_per_second;
        uint32 avg_job_time_us;
        uint32 cache_hits;
        uint32 cache_misses;
    } stats;
    
    struct {
        bool enabled;
        uint32 min_job_pixels;
        uint32 preferred_block_size;
    } config;
} parallel_graphics_state_t;

parallel_graphics_state_t* parallel_graphics_get_state(void);

graphics_result_t parallel_graphics_init(graphics_surface_t* primary_surface);
graphics_result_t parallel_graphics_shutdown(void);
bool parallel_graphics_is_initialized(void);

graphics_result_t parallel_graphics_set_target_surface(graphics_surface_t* surface);
graphics_surface_t* parallel_graphics_get_target_surface(void);

graphics_result_t parallel_graphics_schedule_rect_fill(const graphics_rect_t* rect,
                                                    graphics_color_t color);
graphics_result_t parallel_graphics_schedule_rect_copy(const uint8_t* src, uint8_t* dst,
                                                    uint32_t src_pitch, uint32_t dst_pitch,
                                                    const graphics_rect_t* rect,
                                                    uint32_t bytes_per_pixel);
graphics_result_t parallel_graphics_schedule_line(int32_t x1, int32_t y1,
                                                int32_t x2, int32_t y2,
                                                graphics_color_t color);
graphics_result_t parallel_graphics_schedule_circle(int32_t cx, int32_t cy,
                                                  int32_t radius,
                                                  graphics_color_t color,
                                                  bool filled, bool antialiased);
graphics_result_t parallel_graphics_schedule_blit(const graphics_surface_t* src,
                                               graphics_surface_t* dst,
                                               const graphics_rect_t* src_rect,
                                               int32_t dst_x, int32_t dst_y);

graphics_result_t parallel_graphics_flush(void);
graphics_result_t parallel_graphics_wait_idle(void);

graphics_result_t parallel_graphics_optimize_for_rects(const graphics_rect_t* rects,
                                                     uint32_t count,
                                                     graphics_color_t color);

graphics_result_t parallel_graphics_block_fill(uint8_t* dst, uint32_t pitch,
                                           const graphics_rect_t* rect,
                                           uint32_t pixel_value,
                                           uint32_t bytes_per_pixel);

graphics_result_t parallel_graphics_block_copy(const uint8_t* src, uint8_t* dst,
                                           uint32_t src_pitch, uint32_t dst_pitch,
                                           const graphics_rect_t* rect,
                                           uint32_t bytes_per_pixel);

graphics_result_t parallel_graphics_get_config(uint32_t* min_job_pixels,
                                            uint32_t* preferred_block_size);
graphics_result_t parallel_graphics_set_config(uint32_t min_job_pixels,
                                            uint32_t preferred_block_size);

void parallel_graphics_get_stats(uint32_t* jobs_per_second,
                              uint32_t* avg_job_time_us,
                              uint32_t* cache_hits,
                              uint32_t* cache_misses);
void parallel_graphics_reset_stats(void);

static inline bool parallel_graphics_should_parallelize(uint32_t pixel_count) {
    if (!parallel_graphics_is_initialized()) {
        return false;
    }
    parallel_graphics_state_t* state = parallel_graphics_get_state();
    return pixel_count >= state->config.min_job_pixels;
}

#endif // PARALLEL_GRAPHICS_H
