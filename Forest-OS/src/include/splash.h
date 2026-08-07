#ifndef SPLASH_H
#define SPLASH_H

#include <stdbool.h>
#include <stdint.h>
#include "graphics/graphics_manager.h"

/* Aurora-style boot screen colors (modern blue gradient aesthetic) */
#define SPLASH_AERO_BG_TOP_R        0
#define SPLASH_AERO_BG_TOP_G        51
#define SPLASH_AERO_BG_TOP_B        102

#define SPLASH_AERO_BG_BOTTOM_R     0
#define SPLASH_AERO_BG_BOTTOM_G     30
#define SPLASH_AERO_BG_BOTTOM_B     60

#define SPLASH_AERO_ACCENT_R        255
#define SPLASH_AERO_ACCENT_G        255
#define SPLASH_AERO_ACCENT_B        255

#define SPLASH_AERO_ACCENT_SOFT_R   180
#define SPLASH_AERO_ACCENT_SOFT_G   200
#define SPLASH_AERO_ACCENT_SOFT_B   220

#define SPLASH_AERO_ACCENT_DARK_R   100
#define SPLASH_AERO_ACCENT_DARK_G   130
#define SPLASH_AERO_ACCENT_DARK_B   160

#define SPLASH_AERO_GLOW_R          135
#define SPLASH_AERO_GLOW_G          206
#define SPLASH_AERO_GLOW_B          250

// Animation parameters
#define SPLASH_ANIM_DOTS            5
#define SPLASH_ANIM_PERIOD          180
#define SPLASH_ANIM_DOT_SPACING     30
#define SPLASH_ANIM_FPS             30
#define SPLASH_ANIM_FRAME_SKIP      (1000 / SPLASH_ANIM_FPS)  // ms between frames

/* Splash screen states */
typedef enum {
    SPLASH_STATE_IDLE,
    SPLASH_STATE_INITIALIZING,
    SPLASH_STATE_RUNNING,
    SPLASH_STATE_FADING_OUT,
    SPLASH_STATE_DONE
} splash_state_t;

/* Splash screen configuration */
typedef struct {
    bool enabled;               // Whether splash is enabled
    bool use_quiet_mode;        // Use quiet/silent boot animation
    uint32_t fade_out_duration; // Duration of fade out in ms
} splash_config_t;

/* Overlay splash screen - saves underlying framebuffer for restoration */
typedef struct {
    uint8_t* backup;            // Saved framebuffer content
    uint32_t size;             // Size of backup
    uint32_t width;            // Framebuffer width
    uint32_t height;           // Framebuffer height
    uint32_t pitch;            // Framebuffer pitch
    uint32_t bpp;              // Bits per pixel
} splash_overlay_t;

/* Initialize the splash screen system
 * Must be called after graphics subsystem is initialized
 * Returns true on success, false on failure
 */
bool splash_init(const splash_config_t* config);

/* Start the splash screen animation in a background task
 * This spawns a separate kernel thread for the animation
 * Returns true on success, false on failure
 */
bool splash_start(void);

/* Actually spawn the animation kernel task, once the task scheduler is
 * ready (i.e. after tasks_init()). splash_start() defers to this when
 * called too early in boot for a kernel task to be safely scheduled;
 * kernel_main() also calls this directly right after tasks_init() to
 * pick up that deferred creation. Safe to call multiple times.
 */
void splash_start_animation_task(void);

/* Stop the splash screen animation
 * This will fade out the splash screen and terminate the animation task
 */
void splash_stop(void);

/* Draw the splash screen background (one-time render) */
void splash_draw_background(void);

/* Draw the animation frame (throbber/dots)
 * Called internally by the animation task
 */
void splash_draw_frame(uint32_t frame_number);

/* Check if the splash screen is currently running */
bool splash_is_running(void);

/* Get current splash state */
splash_state_t splash_get_state(void);

/* Set the boot progress (0-100)
 * Updates the progress bar if enabled
 */
void splash_set_progress(uint8_t progress_percent);

/* Update splash with a boot status message
 * This updates the progress and may show status text
 */
void splash_update_status(const char* status_message, bool success);

/* Cleanup splash resources */
void splash_cleanup(void);

/* Internal: Animation task entry point
 * This runs in a separate kernel thread
 */
void splash_animation_task(void);

void splash_record_start_ticks(void);
bool splash_should_timeout(void);

/* Force the splash to re-render and composite on top of current content.
 * Call this when the splash needs to be redrawn over changed underlying layers. */
void splash_rerender(void);

/* Migrate the splash from the early boot buffer to the render layer system.
 * Call this after rl_init() becomes available during graphics initialization. */
void splash_migrate_to_layer(void);

#endif // SPLASH_H
