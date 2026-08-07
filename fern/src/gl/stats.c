#include "stats.h"

gl_stats_t g_gl_stats;

/*
 * Read a timestamp. In a kernel environment this would be rdtsc
 * or a platform timer. The counter is opaque -- only deltas matter.
 */
static inline uint64_t read_ticks(void)
{
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    static uint64_t counter = 0;
    return ++counter;
#endif
}

void gl_stats_begin_frame(void)
{
    g_gl_stats.frame_start_ticks = read_ticks();
    g_gl_stats.triangle_count   = 0;
    g_gl_stats.pixel_count      = 0;
    g_gl_stats.depth_test_pass  = 0;
    g_gl_stats.depth_test_fail  = 0;
    g_gl_stats.block_count      = 0;
    g_gl_stats.block_skip       = 0;
}

void gl_stats_end_frame(void)
{
    g_gl_stats.frame_end_ticks  = read_ticks();
    g_gl_stats.frame_time_ticks = g_gl_stats.frame_end_ticks - g_gl_stats.frame_start_ticks;
}

void gl_stats_reset(void)
{
    g_gl_stats.frame_time_ticks = 0;
    g_gl_stats.triangle_count   = 0;
    g_gl_stats.pixel_count      = 0;
    g_gl_stats.depth_test_pass  = 0;
    g_gl_stats.depth_test_fail  = 0;
    g_gl_stats.block_count      = 0;
    g_gl_stats.block_skip       = 0;
}

void gl_stats_record_triangle(void)
{
    g_gl_stats.triangle_count++;
}

void gl_stats_record_pixels(int count)
{
    g_gl_stats.pixel_count += (uint32_t)count;
}

void gl_stats_record_depth(int pass, int fail)
{
    g_gl_stats.depth_test_pass += (uint32_t)pass;
    g_gl_stats.depth_test_fail += (uint32_t)fail;
}

void gl_stats_record_block(int skipped)
{
    g_gl_stats.block_count++;
    g_gl_stats.block_skip += (uint32_t)skipped;
}
