#ifndef GL_STATS_H
#define GL_STATS_H

#include <stdint.h>

typedef struct {
    uint64_t frame_start_ticks;
    uint64_t frame_end_ticks;
    uint64_t frame_time_ticks;
    uint32_t triangle_count;
    uint32_t pixel_count;
    uint32_t depth_test_pass;
    uint32_t depth_test_fail;
    uint32_t block_count;
    uint32_t block_skip;
} gl_stats_t;

extern gl_stats_t g_gl_stats;

void gl_stats_begin_frame(void);
void gl_stats_end_frame(void);
void gl_stats_reset(void);
void gl_stats_record_triangle(void);
void gl_stats_record_pixels(int count);
void gl_stats_record_depth(int pass, int fail);
void gl_stats_record_block(int skipped);

#endif /* GL_STATS_H */
