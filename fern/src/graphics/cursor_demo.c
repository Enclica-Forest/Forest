#include "../include/graphics/enhanced_cursor.h"
#include "../include/graphics/graphics_manager.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"
#include "../include/mm.h"

static enhanced_cursor_t* g_test_cursor = NULL;

void cursor_demo_init(void) {
    debuglog(DEBUG_INFO, "Initializing enhanced cursor demo...\n");
    
    enhanced_cursor_init();
    
    cursor_theme_t theme = {
        .primary = COLOR_WHITE,
        .secondary = COLOR_BLACK,
        .has_outline = true,
        .outline = COLOR_BLACK
    };
    
    enhanced_cursor_create(CURSOR_TYPE_ARROW, &theme, &g_test_cursor);
    if (g_test_cursor) {
        debuglog(DEBUG_INFO, "Created enhanced cursor\n");
    }
}

void cursor_demo_cycle_types(void) {
    static cursor_type_t types[] = {
        CURSOR_TYPE_ARROW,
        CURSOR_TYPE_MOVE,
        CURSOR_TYPE_HAND,
        CURSOR_TYPE_TEXT,
        CURSOR_TYPE_BUSY,
        CURSOR_TYPE_CROSSHAIR,
        CURSOR_TYPE_HELP,
        CURSOR_TYPE_RESIZE_NS,
        CURSOR_TYPE_RESIZE_EW,
        CURSOR_TYPE_RESIZE_NWSE,
        CURSOR_TYPE_RESIZE_NESW
    };
    
    static uint32_t current_type = 0;
    
    if (!g_test_cursor) return;
    
    current_type = (current_type + 1) % (sizeof(types) / sizeof(types[0]));
    
    enhanced_cursor_set_type(g_test_cursor, types[current_type]);
    
    const char* type_names[] = {
        "Arrow",
        "Move",
        "Hand",
        "Text",
        "Busy",
        "Crosshair",
        "Help",
        "Resize NS",
        "Resize EW",
        "Resize NWSE",
        "Resize NESW"
    };
    
    debuglog(DEBUG_INFO, "Cursor type changed to: %s\n", type_names[current_type]);
}

void cursor_demo_animate(void) {
    if (!g_test_cursor) return;
    
    enhanced_cursor_animate(g_test_cursor, true);
    debuglog(DEBUG_INFO, "Cursor animation enabled\n");
}

void cursor_demo_disable_shadow(void) {
    if (!g_test_cursor) return;
    
    g_test_cursor->shadow.enabled = false;
    debuglog(DEBUG_INFO, "Cursor shadow disabled\n");
}

void cursor_demo_enable_shadow(void) {
    if (!g_test_cursor) return;
    
    g_test_cursor->shadow.enabled = true;
    g_test_cursor->shadow.blur_radius = 5;
    g_test_cursor->shadow.alpha = 0.5f;
    debuglog(DEBUG_INFO, "Cursor shadow enabled with blur\n");
}

void cursor_demo_set_theme(const char* theme_name) {
    if (!g_test_cursor) return;
    
    cursor_theme_t theme;
    
    if (strcmp(theme_name, "dark") == 0) {
        theme.primary = COLOR_BLACK;
        theme.secondary = COLOR_WHITE;
        theme.outline = COLOR_WHITE;
        theme.has_outline = true;
        debuglog(DEBUG_INFO, "Set dark theme\n");
    } else if (strcmp(theme_name, "red") == 0) {
        theme.primary = COLOR_RED;
        theme.secondary = COLOR_DARK_GRAY;
        theme.outline = COLOR_BLACK;
        theme.has_outline = true;
        debuglog(DEBUG_INFO, "Set red theme\n");
    } else if (strcmp(theme_name, "green") == 0) {
        theme.primary = COLOR_GREEN;
        theme.secondary = COLOR_BLACK;
        theme.outline = COLOR_BLACK;
        theme.has_outline = true;
        debuglog(DEBUG_INFO, "Set green theme\n");
    } else if (strcmp(theme_name, "blue") == 0) {
        theme.primary = COLOR_BLUE;
        theme.secondary = COLOR_WHITE;
        theme.outline = COLOR_BLACK;
        theme.has_outline = true;
        debuglog(DEBUG_INFO, "Set blue theme\n");
    } else {
        theme.primary = COLOR_WHITE;
        theme.secondary = COLOR_BLACK;
        theme.outline = COLOR_BLACK;
        theme.has_outline = true;
        debuglog(DEBUG_INFO, "Set default theme\n");
    }
    
    enhanced_cursor_set_theme(g_test_cursor, &theme);
}

void cursor_demo_test_animation(void) {
    if (!g_test_cursor) return;
    
    uint64_t start_time = 0;
    uint32_t frame_count = 0;
    uint32_t total_frames = 100;
    
    debuglog(DEBUG_INFO, "Running cursor animation test (%d frames)...\n", total_frames);
    
    enhanced_cursor_set_type(g_test_cursor, CURSOR_TYPE_BUSY);
    enhanced_cursor_animate(g_test_cursor, true);
    
    for (uint32_t i = 0; i < total_frames; i++) {
        enhanced_cursor_update(g_test_cursor, start_time + i * 100);
        
        if (g_test_cursor->animation_frame != frame_count) {
            frame_count = g_test_cursor->animation_frame;
        }
    }
    
    debuglog(DEBUG_INFO, "Animation test complete. Total animation cycles: %d\n", frame_count);
}

void cursor_demo_test_shadow(void) {
    if (!g_test_cursor) return;
    
    debuglog(DEBUG_INFO, "Testing cursor shadow variations...\n");
    
    struct {
        int32_t blur_radius;
        float alpha;
        const char* name;
    } shadow_tests[] = {
        {0, 0.0f, "No shadow"},
        {1, 0.2f, "Light shadow"},
        {3, 0.4f, "Medium shadow"},
        {5, 0.6f, "Heavy shadow"},
        {8, 0.8f, "Very heavy shadow"}
    };
    
    for (uint32_t i = 0; i < sizeof(shadow_tests) / sizeof(shadow_tests[0]); i++) {
        g_test_cursor->shadow.enabled = true;
        g_test_cursor->shadow.blur_radius = shadow_tests[i].blur_radius;
        g_test_cursor->shadow.alpha = shadow_tests[i].alpha;
        
        debuglog(DEBUG_INFO, "  Testing: %s (blur=%d, alpha=%.1f)\n",
                 shadow_tests[i].name,
                 shadow_tests[i].blur_radius,
                 shadow_tests[i].alpha);
    }
    
    debuglog(DEBUG_INFO, "Shadow test complete\n");
}

void cursor_demo_test_all_types(void) {
    if (!g_test_cursor) return;
    
    debuglog(DEBUG_INFO, "Testing all cursor types...\n");
    
    cursor_type_t types[] = {
        CURSOR_TYPE_ARROW,
        CURSOR_TYPE_MOVE,
        CURSOR_TYPE_HAND,
        CURSOR_TYPE_TEXT,
        CURSOR_TYPE_BUSY,
        CURSOR_TYPE_CROSSHAIR,
        CURSOR_TYPE_HELP,
        CURSOR_TYPE_RESIZE_NS,
        CURSOR_TYPE_RESIZE_EW,
        CURSOR_TYPE_RESIZE_NWSE,
        CURSOR_TYPE_RESIZE_NESW
    };
    
    const char* type_names[] = {
        "Arrow",
        "Move",
        "Hand",
        "Text",
        "Busy",
        "Crosshair",
        "Help",
        "Resize NS",
        "Resize EW",
        "Resize NWSE",
        "Resize NESW"
    };
    
    for (uint32_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        graphics_result_t result = enhanced_cursor_set_type(g_test_cursor, types[i]);
        
        if (result == GRAPHICS_SUCCESS) {
            debuglog(DEBUG_INFO, "  ✓ Type %d: %s\n", i, type_names[i]);
        } else {
            debuglog(DEBUG_ERROR, "  ✗ Type %d: %s (error %d)\n",
                     i, type_names[i], result);
        }
    }
    
    debuglog(DEBUG_INFO, "All cursor types tested\n");
}

void cursor_demo_benchmark(void) {
    if (!g_test_cursor) return;
    
    debuglog(DEBUG_INFO, "Running cursor performance benchmark...\n");
    
    graphics_surface_t* test_surface;
    if (graphics_create_surface(800, 600, PIXEL_FORMAT_RGBA_8888, &test_surface) 
        != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_ERROR, "Failed to create test surface\n");
        return;
    }
    
    const uint32_t iterations = 1000;
    uint64_t start = 0;
    (void)start;
    
    start = 0;
    for (uint32_t i = 0; i < iterations; i++) {
        enhanced_cursor_draw(g_test_cursor, test_surface);
    }
    
    debuglog(DEBUG_INFO, "Benchmark complete: %d iterations\n", iterations);
    
    graphics_destroy_surface(test_surface);
}

void cursor_demo_cleanup(void) {
    if (g_test_cursor) {
        enhanced_cursor_destroy(g_test_cursor);
        g_test_cursor = NULL;
    }
    
    enhanced_cursor_shutdown();
    debuglog(DEBUG_INFO, "Cursor demo cleaned up\n");
}

void cursor_demo_custom_cursor(void) {
    if (!g_test_cursor) return;
    
    debuglog(DEBUG_INFO, "Creating custom cursor...\n");
    
    static const uint8_t custom_bitmap[] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,1,2,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,1,2,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,1,2,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,1,2,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,1,2,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,1,2,2,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    };
    
    graphics_result_t result = enhanced_cursor_set_custom_bitmap(
        g_test_cursor,
        custom_bitmap,
        CURSOR_WIDTH,
        CURSOR_HEIGHT,
        12,
        12
    );
    
    if (result == GRAPHICS_SUCCESS) {
        debuglog(DEBUG_INFO, "Custom cursor created successfully\n");
    } else {
        debuglog(DEBUG_ERROR, "Failed to create custom cursor: %d\n", result);
    }
}

void cursor_demo_print_status(void) {
    if (!g_test_cursor) {
        debuglog(DEBUG_INFO, "No cursor created\n");
        return;
    }
    
    debuglog(DEBUG_INFO, "Cursor Status:\n");
    debuglog(DEBUG_INFO, "  Type: %d\n", g_test_cursor->type);
    debuglog(DEBUG_INFO, "  State: %d\n", g_test_cursor->state);
    debuglog(DEBUG_INFO, "  Position: (%d, %d)\n", g_test_cursor->x, g_test_cursor->y);
    debuglog(DEBUG_INFO, "  Visible: %s\n", g_test_cursor->visible ? "Yes" : "No");
    debuglog(DEBUG_INFO, "  Animated: %s\n", g_test_cursor->animated ? "Yes" : "No");
    debuglog(DEBUG_INFO, "  Frame: %u\n", g_test_cursor->animation_frame);
    debuglog(DEBUG_INFO, "  Hotspot: (%d, %d)\n", g_test_cursor->hotspot_x, g_test_cursor->hotspot_y);
    debuglog(DEBUG_INFO, "  Shadow enabled: %s\n", g_test_cursor->shadow.enabled ? "Yes" : "No");
    debuglog(DEBUG_INFO, "  Primary color: RGBA(%u,%u,%u,%u)\n",
             g_test_cursor->primary_color.r,
             g_test_cursor->primary_color.g,
             g_test_cursor->primary_color.b,
             g_test_cursor->primary_color.a);
    debuglog(DEBUG_INFO, "  Secondary color: RGBA(%u,%u,%u,%u)\n",
             g_test_cursor->secondary_color.r,
             g_test_cursor->secondary_color.g,
             g_test_cursor->secondary_color.b,
             g_test_cursor->secondary_color.a);
}

void cursor_demo_menu(void) {
    debuglog(DEBUG_INFO, "\n=== Enhanced Cursor Demo Menu ===\n");
    debuglog(DEBUG_INFO, "1. Cycle cursor types\n");
    debuglog(DEBUG_INFO, "2. Enable animation\n");
    debuglog(DEBUG_INFO, "3. Test all types\n");
    debuglog(DEBUG_INFO, "4. Test animation\n");
    debuglog(DEBUG_INFO, "5. Test shadow variations\n");
    debuglog(DEBUG_INFO, "6. Create custom cursor\n");
    debuglog(DEBUG_INFO, "7. Set theme (dark/red/green/blue/default)\n");
    debuglog(DEBUG_INFO, "8. Enable shadow\n");
    debuglog(DEBUG_INFO, "9. Disable shadow\n");
    debuglog(DEBUG_INFO, "0. Print status\n");
    debuglog(DEBUG_INFO, "q. Quit\n");
    debuglog(DEBUG_INFO, "=====================================\n");
}
