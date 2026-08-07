#include "include/ps2_watchdog.h"
#include "include/ps2_keyboard.h"
#include "include/ps2_mouse.h"
#include "include/ps2_controller.h"
#include "include/timer.h"
#include "include/task.h"
#include "include/debuglog.h"

static bool g_watchdog_started = false;

/*
 * Check whether the PS/2 controller responds to a self-test command.
 * Returns true if the controller is responsive (self-test passed),
 * false if the command timed out or the wrong response was received.
 * NOTE: This is intentionally conservative — it only runs when the
 * keyboard and mouse drivers are already not present/ready, so it
 * will not disrupt an operational IRQ stream.
 */
static bool ps2_controller_is_responsive(void) {
    if (!ps2_controller_send_command(PS2_CMD_SELF_TEST)) {
        return false;
    }
    if (!ps2_controller_wait_output_ready()) {
        return false;
    }
    uint8_t response = ps2_controller_read_data();
    return (response == PS2_RESPONSE_CONTROLLER_SELF_TEST_PASSED);
}

static void ps2_watchdog_main(void) {
    bool last_keyboard_present = true;
    bool last_mouse_present = true;

    while (1) {
        /*
         * Sleep for 5 seconds between checks.  Use sleep_interruptible() so
         * the task is properly marked WAITING and the scheduler can run other
         * tasks during the delay — no busy-waiting, no CPU waste.
         */
        sleep_interruptible(5000);

        bool keyboard_present = ps2_keyboard_is_present();
        if (keyboard_present && !last_keyboard_present) {
            debuglog(DEBUG_INFO, "[PS2] Keyboard reconnected, reinitializing\n");
            if (ps2_keyboard_reinit() == 0) {
                debuglog(DEBUG_INFO, "[PS2] Keyboard reinitialized\n");
            } else {
                debuglog(DEBUG_WARN, "[PS2] Keyboard reinit failed\n");
            }
        }
        last_keyboard_present = keyboard_present;

        /* Only probe for mouse presence when the driver reports it as NOT ready.
         * ps2_mouse_is_present() sends a GET_DEVICE_ID command to the controller;
         * issuing that while the mouse is already in stream mode will corrupt the
         * in-flight packet stream and stall IRQ12 reception entirely.
         * When the mouse driver is up and running (ps2_mouse_is_ready() == true)
         * we simply assume it is still present and skip the disruptive probe. */
        bool mouse_present;
        if (ps2_mouse_is_ready()) {
            /* Already streaming — assume present, no command needed. */
            mouse_present = true;
        } else {
            mouse_present = ps2_mouse_is_present();
        }

        if (mouse_present && !last_mouse_present) {
            debuglog(DEBUG_INFO, "[PS2] Mouse reconnected, reinitializing\n");
            if (ps2_mouse_reinit() == 0) {
                (void)ps2_mouse_start_streaming();
                debuglog(DEBUG_INFO, "[PS2] Mouse reinitialized\n");
            } else {
                debuglog(DEBUG_WARN, "[PS2] Mouse reinit failed\n");
            }
        }
        last_mouse_present = mouse_present;

        /*
         * If both devices are absent, check whether the controller itself is
         * still responsive.  Only attempt a reset when neither device is
         * present — we must not disturb a running keyboard/mouse stream.
         */
        if (!keyboard_present && !mouse_present) {
            debuglog(DEBUG_INFO, "[PS2] Both devices absent, checking controller...\n");
            if (!ps2_controller_is_responsive()) {
                debuglog(DEBUG_WARN, "[PS2] Controller unresponsive, attempting reset\n");
                if (ps2_controller_init() == 0) {
                    debuglog(DEBUG_INFO, "[PS2] Controller reset OK\n");
                    /* Reset presence state so drivers are re-detected next
                     * iteration after the controller comes back up. */
                    last_keyboard_present = false;
                    last_mouse_present = false;
                } else {
                    debuglog(DEBUG_WARN, "[PS2] Controller reset failed\n");
                }
            }
        }
    }

}

void ps2_watchdog_start(void) {
    if (g_watchdog_started) {
        return;
    }
    g_watchdog_started = true;

    task_t* watchdog = task_create_kernel(ps2_watchdog_main, "ps2_watchdog", 4096);
    if (!watchdog) {
        debuglog(DEBUG_WARN, "[PS2] Failed to start hotplug watchdog\n");
    }
}
