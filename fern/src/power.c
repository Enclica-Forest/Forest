#include "include/power.h"

#include "include/driver.h"
#include "include/interrupt.h"
#include "include/screen.h"
#include "include/sound.h"
#include "include/task.h"
#include "include/timer.h"
#include "include/debuglog.h"

static void power_log(const char* msg) {
    if (!msg) {
        return;
    }
    print("[POWER] ");
    print(msg);
    print("\n");
    debuglog_write("[POWER] ");
    debuglog_write(msg);
    debuglog_write("\n");
}

static void cleanup_subsystems(void) {
    power_log("Stopping scheduler and timer");
    timer_shutdown();

    power_log("Terminating user tasks");
    task_shutdown_all();

    power_log("Shutting down active drivers");
#ifdef ENABLE_AUDIO
    sound_shutdown();
#endif
    driver_shutdown_all();
}

bool power_request(power_action_t action) {
    power_log(action == POWER_ACTION_REBOOT ? "Reboot requested" : "Shutdown requested");
    cleanup_subsystems();
    irq_disable_safe();

    switch (action) {
        case POWER_ACTION_SHUTDOWN: power_shutdown(); break;
        case POWER_ACTION_REBOOT:   power_reboot();   break;
        case POWER_ACTION_SUSPEND:  power_suspend();  break;
        case POWER_ACTION_HALT:     power_halt();     break;
        default: break;
    }

    return false;
}
