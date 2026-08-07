#include "../include/graphics/parallel_graphics.h"
#include "../include/graphics/graphics_manager.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/smp.h"
#include "../include/thread.h"
#include "../include/spinlock.h"
#include "../include/debuglog.h"
#include "../include/mm.h"
#include "../include/libc/stdlib.h"

static parallel_graphics_state_t g_parallel_state = {0};

static void* worker_thread_func(void* arg) {
    parallel_worker_t* worker = (parallel_worker_t*)arg;
    parallel_graphics_state_t* state = &g_parallel_state;
    
    worker->idle = true;
    
    debuglog(DEBUG_INFO, "[ParallelGraphics] Worker %d started on CPU %d\n",
             worker->cpu_id, smp_get_cpu(0)->acpi_id);
    
    while (worker->running) {
        worker->idle = false;
        
        spinlock_acquire(&state->queue_lock);
        
        if (state->queue_head == state->queue_tail) {
            spinlock_release(&state->queue_lock);
            worker->idle = true;
            thread_yield();
            continue;
        }
        
        parallel_job_t* job = &state->job_queue[state->queue_head];
        state->queue_head = (state->queue_head + 1) % PARALLEL_MAX_QUEUED_JOBS;
        
        spinlock_release(&state->queue_lock);
        
        spinlock_acquire(&state->surface_lock);
        graphics_surface_t* target = state->target_surface;
        spinlock_release(&state->surface_lock);
        
        if (!target) {
            job->completed = true;
            atomic_dec(&job->ref_count);
            atomic_inc(&worker->jobs_processed);
            state->stats.jobs_per_second++;
            continue;
        }
        
        uint64_t start_time = 0;
        (void)start_time;
        
        switch (job->type) {
            case PARALLEL_JOB_TYPE_RECT_FILL: {
                parallel_rect_fill_job_t* rf = &job->fill;
                graphics_rect_t r = rf->rect;
                
                for (uint32_t y = 0; y < r.height; y++) {
                    uint32_t* row = (uint32_t*)((uint8_t*)target->pixels +
                        (r.y + y) * target->pitch + r.x * 4);
                    uint32_t row_width = r.width;
                    
                    for (uint32_t x = 0; x < row_width; x++) {
                        row[x] = *rf->pixel_value;
                    }
                }
                break;
            }
            
            case PARALLEL_JOB_TYPE_RECT_COPY: {
                parallel_rect_copy_job_t* rc = &job->copy;
                const uint8_t* src_row = rc->src + rc->rect.y * rc->src_pitch + rc->rect.x * rc->bytes_per_pixel;
                uint8_t* dst_row = rc->dst + rc->rect.y * rc->dst_pitch + rc->rect.x * rc->bytes_per_pixel;
                
                for (uint32_t y = 0; y < rc->rect.height; y++) {
                    memcpy(dst_row, src_row, rc->rect.width * rc->bytes_per_pixel);
                    src_row += rc->src_pitch;
                    dst_row += rc->dst_pitch;
                }
                break;
            }
            
            case PARALLEL_JOB_TYPE_LINE: {
                parallel_line_job_t* lj = &job->line;
                int32_t dx = abs(lj->x2 - lj->x1);
                int32_t dy = abs(lj->y2 - lj->y1);
                int32_t sx = (lj->x1 < lj->x2) ? 1 : -1;
                int32_t sy = (lj->y1 < lj->y2) ? 1 : -1;
                int32_t err = (dx > dy ? dx : -dy) / 2;
                
                int32_t x = lj->x1, y = lj->y1;
                while (true) {
                    if (x >= 0 && y >= 0 && x < (int32_t)target->width &&
                        y < (int32_t)target->height) {
                        uint32_t* pixel = (uint32_t*)((uint8_t*)target->pixels +
                            y * target->pitch + x * 4);
                        *pixel = (lj->color.a << 24) | (lj->color.r << 16) |
                                (lj->color.g << 8) | lj->color.b;
                    }
                    if (x == lj->x2 && y == lj->y2) break;
                    int32_t e2 = err;
                    if (e2 > -dx) { err -= dy; x += sx; }
                    if (e2 < dy) { err += dx; y += sy; }
                }
                break;
            }
            
            case PARALLEL_JOB_TYPE_CIRCLE: {
                parallel_circle_job_t* cj = &job->circle;
                int32_t r = cj->radius;
                int32_t r_sq = r * r;
                
                for (int32_t dy = -r; dy <= r; dy++) {
                    for (int32_t dx = -r; dx <= r; dx++) {
                        int32_t dist_sq = dx * dx + dy * dy;
                        if (dist_sq <= r_sq) {
                            int32_t px = cj->cx + dx;
                            int32_t py = cj->cy + dy;
                            if (px >= 0 && py >= 0 && px < (int32_t)target->width &&
                                py < (int32_t)target->height) {
                                uint32_t* pixel = (uint32_t*)((uint8_t*)target->pixels +
                                    py * target->pitch + px * 4);
                                *pixel = (cj->color.a << 24) | (cj->color.r << 16) |
                                        (cj->color.g << 8) | cj->color.b;
                            }
                        }
                    }
                }
                break;
            }
            
            case PARALLEL_JOB_TYPE_BLIT: {
                parallel_blit_job_t* bj = &job->blit;
                uint32_t bytes_per_pixel = bj->src->bpp / 8;
                
                for (uint32_t y = 0; y < bj->src_rect.height; y++) {
                    const uint8_t* src_row = (const uint8_t*)bj->src->pixels +
                        (bj->src_rect.y + y) * bj->src->pitch +
                        bj->src_rect.x * bytes_per_pixel;
                    uint8_t* dst_row = (uint8_t*)bj->dst->pixels +
                        (bj->dst_y + y) * bj->dst->pitch +
                        bj->dst_x * bytes_per_pixel;
                    memcpy(dst_row, src_row, bj->src_rect.width * bytes_per_pixel);
                }
                break;
            }
            
            case PARALLEL_JOB_TYPE_CUSTOM:
                if (job->custom_func) {
                    job->custom_func(job->custom_data);
                }
                break;
            
            default:
                break;
        }
        
        job->completed = true;
        atomic_dec(&job->ref_count);
        atomic_inc(&worker->jobs_processed);
        atomic_dec(&state->active_jobs);  /* Job done, decrease active count */
    }

    debuglog(DEBUG_INFO, "[ParallelGraphics] Worker %d shutting down\n", worker->cpu_id);
    return NULL;
}

graphics_result_t parallel_graphics_init(graphics_surface_t* primary_surface) {
    if (g_parallel_state.initialized) {
        return GRAPHICS_SUCCESS;
    }
    
    debuglog(DEBUG_INFO, "Initializing parallel graphics subsystem...\n");
    
    memset(&g_parallel_state, 0, sizeof(g_parallel_state));
    
    g_parallel_state.target_surface = primary_surface;
    g_parallel_state.queue_head = 0;
    g_parallel_state.queue_tail = 0;
    
    atomic_set(&g_parallel_state.active_jobs, 0);
    atomic_set(&g_parallel_state.total_jobs, 0);
    
    spinlock_init(&g_parallel_state.queue_lock, "parallel_queue");
    spinlock_init(&g_parallel_state.surface_lock, "parallel_surface");
    
    g_parallel_state.config.enabled = true;
    g_parallel_state.config.min_job_pixels = 4096;
    g_parallel_state.config.preferred_block_size = 64;
    
    uint32 cpu_count = smp_get_cpu_count();
    uint32 num_workers = (cpu_count > 1) ? (cpu_count - 1) : 1;
    if (num_workers > PARALLEL_MAX_WORKERS) {
        num_workers = PARALLEL_MAX_WORKERS;
    }
    
    g_parallel_state.num_workers = num_workers;
    
    for (uint32 i = 0; i < num_workers; i++) {
        parallel_worker_t* worker = &g_parallel_state.workers[i];
        worker->cpu_id = i + 1;
        worker->running = true;
        worker->idle = true;
        atomic_set(&worker->jobs_processed, 0);
        spinlock_init(&worker->lock, "parallel_worker");
        
        worker->thread = thread_create("gfx_worker", worker_thread_func, worker);
        if (!worker->thread) {
            debuglog(DEBUG_ERROR, "Failed to create worker thread %d\n", i);
            g_parallel_state.num_workers = i;
            break;
        }
        
        thread_start(worker->thread);
    }
    
    g_parallel_state.initialized = true;
    
    debuglog(DEBUG_INFO, "Parallel graphics initialized with %d workers\n", 
             g_parallel_state.num_workers);
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t parallel_graphics_shutdown(void) {
    if (!g_parallel_state.initialized) {
        return GRAPHICS_SUCCESS;
    }
    
    debuglog(DEBUG_INFO, "Shutting down parallel graphics subsystem...\n");
    
    parallel_graphics_wait_idle();
    
    for (uint32 i = 0; i < g_parallel_state.num_workers; i++) {
        parallel_worker_t* worker = &g_parallel_state.workers[i];
        worker->running = false;
        if (worker->thread) {
            thread_join(worker->thread, NULL);
            thread_destroy(worker->thread);
            worker->thread = NULL;
        }
    }
    
    g_parallel_state.initialized = false;
    
    debuglog(DEBUG_INFO, "Parallel graphics shutdown complete\n");
    
    return GRAPHICS_SUCCESS;
}

bool parallel_graphics_is_initialized(void) {
    return g_parallel_state.initialized;
}

parallel_graphics_state_t* parallel_graphics_get_state(void) {
    return &g_parallel_state;
}

graphics_result_t parallel_graphics_set_target_surface(graphics_surface_t* surface) {
    spinlock_acquire(&g_parallel_state.surface_lock);
    g_parallel_state.target_surface = surface;
    spinlock_release(&g_parallel_state.surface_lock);
    return GRAPHICS_SUCCESS;
}

graphics_surface_t* parallel_graphics_get_target_surface(void) {
    spinlock_acquire(&g_parallel_state.surface_lock);
    graphics_surface_t* surface = g_parallel_state.target_surface;
    spinlock_release(&g_parallel_state.surface_lock);
    return surface;
}

static graphics_result_t enqueue_job(parallel_job_t* job) {
    spinlock_acquire(&g_parallel_state.queue_lock);
    
    uint32 next_tail = (g_parallel_state.queue_tail + 1) % PARALLEL_MAX_QUEUED_JOBS;
    if (next_tail == g_parallel_state.queue_head) {
        spinlock_release(&g_parallel_state.queue_lock);
        return GRAPHICS_ERROR_DEVICE_BUSY;
    }
    
    g_parallel_state.job_queue[g_parallel_state.queue_tail] = *job;
    g_parallel_state.queue_tail = next_tail;
    
    spinlock_release(&g_parallel_state.queue_lock);
    
    atomic_inc(&g_parallel_state.total_jobs);
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t parallel_graphics_schedule_rect_fill(const graphics_rect_t* rect,
                                                    graphics_color_t color) {
    parallel_job_t job = {0};
    job.type = PARALLEL_JOB_TYPE_RECT_FILL;
    job.fill.rect = *rect;
    job.fill.color = color;
    uint32_t pixel_value = (color.a << 24) | (color.r << 16) | (color.g << 8) | color.b;
    job.fill.pixel_value = kmalloc(sizeof(uint32_t));
    if (!job.fill.pixel_value) {
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }
    *job.fill.pixel_value = pixel_value;
    job.completed = false;
    atomic_set(&job.ref_count, 1);
    
    return enqueue_job(&job);
}

graphics_result_t parallel_graphics_schedule_rect_copy(const uint8_t* src, uint8_t* dst,
                                                    uint32_t src_pitch, uint32_t dst_pitch,
                                                    const graphics_rect_t* rect,
                                                    uint32_t bytes_per_pixel) {
    parallel_job_t job = {0};
    job.type = PARALLEL_JOB_TYPE_RECT_COPY;
    job.copy.src = src;
    job.copy.dst = dst;
    job.copy.src_pitch = src_pitch;
    job.copy.dst_pitch = dst_pitch;
    job.copy.rect = *rect;
    job.copy.bytes_per_pixel = bytes_per_pixel;
    job.completed = false;
    atomic_set(&job.ref_count, 1);
    
    return enqueue_job(&job);
}

graphics_result_t parallel_graphics_schedule_line(int32_t x1, int32_t y1,
                                                int32_t x2, int32_t y2,
                                                graphics_color_t color) {
    parallel_job_t job = {0};
    job.type = PARALLEL_JOB_TYPE_LINE;
    job.line.x1 = x1;
    job.line.y1 = y1;
    job.line.x2 = x2;
    job.line.y2 = y2;
    job.line.color = color;
    job.completed = false;
    atomic_set(&job.ref_count, 1);
    
    return enqueue_job(&job);
}

graphics_result_t parallel_graphics_schedule_circle(int32_t cx, int32_t cy,
                                                  int32_t radius,
                                                  graphics_color_t color,
                                                  bool filled, bool antialiased) {
    (void)antialiased;
    
    parallel_job_t job = {0};
    job.type = PARALLEL_JOB_TYPE_CIRCLE;
    job.circle.cx = cx;
    job.circle.cy = cy;
    job.circle.radius = radius;
    job.circle.color = color;
    job.circle.filled = filled;
    job.circle.antialiased = antialiased;
    job.completed = false;
    atomic_set(&job.ref_count, 1);
    
    return enqueue_job(&job);
}

graphics_result_t parallel_graphics_schedule_blit(const graphics_surface_t* src,
                                               graphics_surface_t* dst,
                                               const graphics_rect_t* src_rect,
                                               int32_t dst_x, int32_t dst_y) {
    parallel_job_t job = {0};
    job.type = PARALLEL_JOB_TYPE_BLIT;
    job.blit.src = src;
    job.blit.dst = dst;
    job.blit.src_rect = src_rect ? *src_rect : 
        (graphics_rect_t){0, 0, src->width, src->height};
    job.blit.dst_x = dst_x;
    job.blit.dst_y = dst_y;
    job.completed = false;
    atomic_set(&job.ref_count, 1);
    
    return enqueue_job(&job);
}

graphics_result_t parallel_graphics_flush(void) {
    return GRAPHICS_SUCCESS;
}

graphics_result_t parallel_graphics_wait_idle(void) {
    while (atomic_read(&g_parallel_state.active_jobs) > 0 ||
           g_parallel_state.queue_head != g_parallel_state.queue_tail) {
        thread_yield();
    }
    return GRAPHICS_SUCCESS;
}

graphics_result_t parallel_graphics_block_fill(uint8_t* dst, uint32_t pitch,
                                           const graphics_rect_t* rect,
                                           uint32_t pixel_value,
                                           uint32_t bytes_per_pixel) {
    if (!dst || !rect || bytes_per_pixel == 0) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    uint32_t start_y = rect->y;
    uint32_t end_y = rect->y + rect->height;
    uint32_t start_x = rect->x;
    uint32_t end_x = rect->x + rect->width;
    
    if (bytes_per_pixel == 4) {
        uint32_t* dst32 = (uint32_t*)dst;
        uint32_t pitch32 = pitch / 4;
        
        for (uint32_t y = start_y; y < end_y; y++) {
            for (uint32_t x = start_x; x < end_x; x++) {
                dst32[y * pitch32 + x] = pixel_value;
            }
        }
    } else if (bytes_per_pixel == 2) {
        uint16_t* dst16 = (uint16_t*)dst;
        uint32_t pitch16 = pitch / 2;
        uint16_t pixel16 = (uint16_t)pixel_value;
        
        for (uint32_t y = start_y; y < end_y; y++) {
            for (uint32_t x = start_x; x < end_x; x++) {
                dst16[y * pitch16 + x] = pixel16;
            }
        }
    } else {
        for (uint32_t y = start_y; y < end_y; y++) {
            uint8_t* row = dst + y * pitch + start_x * bytes_per_pixel;
            for (uint32_t x = 0; x < rect->width; x++) {
                memcpy(row + x * bytes_per_pixel, &pixel_value, bytes_per_pixel);
            }
        }
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t parallel_graphics_block_copy(const uint8_t* src, uint8_t* dst,
                                           uint32_t src_pitch, uint32_t dst_pitch,
                                           const graphics_rect_t* rect,
                                           uint32_t bytes_per_pixel) {
    if (!src || !dst || !rect || bytes_per_pixel == 0) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    uint32_t row_size = rect->width * bytes_per_pixel;
    
    for (uint32_t y = 0; y < rect->height; y++) {
        const uint8_t* src_row = src + (rect->y + y) * src_pitch + rect->x * bytes_per_pixel;
        uint8_t* dst_row = dst + (rect->y + y) * dst_pitch + rect->x * bytes_per_pixel;
        memcpy(dst_row, src_row, row_size);
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t parallel_graphics_get_config(uint32_t* min_job_pixels,
                                            uint32_t* preferred_block_size) {
    if (min_job_pixels) {
        *min_job_pixels = g_parallel_state.config.min_job_pixels;
    }
    if (preferred_block_size) {
        *preferred_block_size = g_parallel_state.config.preferred_block_size;
    }
    return GRAPHICS_SUCCESS;
}

graphics_result_t parallel_graphics_set_config(uint32_t min_job_pixels,
                                            uint32_t preferred_block_size) {
    g_parallel_state.config.min_job_pixels = min_job_pixels;
    g_parallel_state.config.preferred_block_size = preferred_block_size;
    return GRAPHICS_SUCCESS;
}

void parallel_graphics_get_stats(uint32_t* jobs_per_second,
                              uint32_t* avg_job_time_us,
                              uint32_t* cache_hits,
                              uint32_t* cache_misses) {
    if (jobs_per_second) {
        *jobs_per_second = g_parallel_state.stats.jobs_per_second;
    }
    if (avg_job_time_us) {
        *avg_job_time_us = g_parallel_state.stats.avg_job_time_us;
    }
    if (cache_hits) {
        *cache_hits = g_parallel_state.stats.cache_hits;
    }
    if (cache_misses) {
        *cache_misses = g_parallel_state.stats.cache_misses;
    }
}

void parallel_graphics_reset_stats(void) {
    memset(&g_parallel_state.stats, 0, sizeof(g_parallel_state.stats));
}
