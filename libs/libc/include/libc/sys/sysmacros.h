#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#include <sys/types.h>

#define major(dev) ((unsigned int)(((dev) >> 8) & 0xFF))
#define minor(dev) ((unsigned int)((dev) & 0xFF))
#define makedev(maj, min) ((dev_t)(((maj) << 8) | (min)))

#endif
