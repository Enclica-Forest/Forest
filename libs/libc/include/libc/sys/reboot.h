#ifndef _SYS_REBOOT_H
#define _SYS_REBOOT_H

#define RB_POWER_OFF 0x4321FEDB
#define RB_AUTOBOOT  0x01234567
#define RB_HALT_SYSTEM 0xCDEF0123
#define RB_ENABLE_CAD 0x1234567

int reboot(int cmd);

#endif
