#ifndef ARCH_POWER_H
#define ARCH_POWER_H

#include "arch.h"
#include <stdbool.h>

bool power_shutdown(void);
bool power_reboot(void);
bool power_suspend(void);
bool power_halt(void);

#endif
