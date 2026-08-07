#ifndef IOCTL_H
#define IOCTL_H

#include "types.h"

/*
 * ioctl command encoding (Linux-compatible)
 *
 * The ioctl number is encoded in 32 bits:
 *   bits 31-30: direction (read/write)
 *   bits 29-16: size of argument
 *   bits 15-8:  type (magic number identifying driver)
 *   bits 7-0:   number (command within driver)
 */

#define _IOC_NRBITS     8
#define _IOC_TYPEBITS   8
#define _IOC_SIZEBITS   14
#define _IOC_DIRBITS    2

#define _IOC_NRMASK     ((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK   ((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK   ((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK    ((1 << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT  (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT   (_IOC_SIZESHIFT + _IOC_SIZEBITS)

// Direction bits
#define _IOC_NONE       0U
#define _IOC_WRITE      1U
#define _IOC_READ       2U

// Encode an ioctl command
#define _IOC(dir, type, nr, size) \
    (((dir) << _IOC_DIRSHIFT) | \
     ((type) << _IOC_TYPESHIFT) | \
     ((nr) << _IOC_NRSHIFT) | \
     ((size) << _IOC_SIZESHIFT))

// Macros for defining ioctl commands
#define _IO(type, nr)           _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, size)    _IOC(_IOC_READ, (type), (nr), sizeof(size))
#define _IOW(type, nr, size)    _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#define _IOWR(type, nr, size)   _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(size))

// Decode an ioctl command
#define _IOC_DIR(nr)    (((nr) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(nr)   (((nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr)     (((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(nr)   (((nr) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

/*
 * Generic input device ioctls (evdev-style)
 * Type: 'E' (0x45)
 */
#define EVIOCGVERSION   _IOR('E', 0x01, int)            // Get driver version
#define EVIOCGID        _IOR('E', 0x02, uint32)         // Get device ID struct
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, (len))  // Get device name
#define EVIOCGPHYS(len) _IOC(_IOC_READ, 'E', 0x07, (len))  // Get physical location
#define EVIOCGUNIQ(len) _IOC(_IOC_READ, 'E', 0x08, (len))  // Get unique identifier

/*
 * Keyboard-specific ioctls
 * Type: 'K' (0x4B)
 */
#define KDSETLED        _IOW('K', 0x32, int)            // Set LED state
#define KDGETLED        _IOR('K', 0x31, int)            // Get LED state
#define KDSETTYPEMATIC  _IOW('K', 0x33, uint32)         // Set typematic rate/delay
#define KDGETTYPEMATIC  _IOR('K', 0x34, uint32)         // Get typematic rate/delay
#define KDSETMODE       _IOW('K', 0x35, int)            // Set keyboard mode (raw/xlate)
#define KDGETMODE       _IOR('K', 0x36, int)            // Get keyboard mode
#define KDSETSCANCODE   _IOW('K', 0x37, int)            // Set scancode set (1, 2, 3)
#define KDGETSCANCODE   _IOR('K', 0x38, int)            // Get scancode set

// Keyboard modes for KDSETMODE/KDGETMODE
#define KBD_MODE_RAW        0   // Raw scancodes
#define KBD_MODE_XLATE      1   // Translated to keycodes
#define KBD_MODE_MEDIUMRAW  2   // Keycode with make/break
#define KBD_MODE_UNICODE    3   // Unicode characters

// LED bits for KDSETLED/KDGETLED
#define LED_SCROLLLOCK      0x01
#define LED_NUMLOCK         0x02
#define LED_CAPSLOCK        0x04

/*
 * Mouse-specific ioctls
 * Type: 'M' (0x4D)
 */
#define MOUSESETRES     _IOW('M', 0x01, int)            // Set resolution (0-3)
#define MOUSEGETRES     _IOR('M', 0x02, int)            // Get resolution
#define MOUSESETRATE    _IOW('M', 0x03, int)            // Set sample rate (10-200)
#define MOUSEGETRATE    _IOR('M', 0x04, int)            // Get sample rate
#define MOUSEGETSTATE   _IOR('M', 0x05, uint32)         // Get button/position state
#define MOUSESETSCALE   _IOW('M', 0x06, int)            // Set scaling (1:1 or 2:1)
#define MOUSEGETSCALE   _IOR('M', 0x07, int)            // Get scaling
#define MOUSEGETID      _IOR('M', 0x08, int)            // Get device ID (0, 3, or 4)

// Mouse resolution values for MOUSESETRES
#define MOUSE_RES_1_COUNT_MM    0   // 1 count per mm
#define MOUSE_RES_2_COUNT_MM    1   // 2 counts per mm
#define MOUSE_RES_4_COUNT_MM    2   // 4 counts per mm
#define MOUSE_RES_8_COUNT_MM    3   // 8 counts per mm

// Mouse scaling values for MOUSESETSCALE
#define MOUSE_SCALE_1_1     0   // 1:1 scaling
#define MOUSE_SCALE_2_1     1   // 2:1 scaling

/*
 * TTY ioctls
 * Type: 'T' (0x54)
 */
#define TIOCGWINSZ      _IOR('T', 0x01, uint32)         // Get window size
#define TIOCSWINSZ      _IOW('T', 0x02, uint32)         // Set window size
#define TIOCFLUSH       _IO('T', 0x03)                  // Flush buffers

/*
 * Generic file ioctls
 * Type: 'F' (0x46)
 */
#define FIONREAD        _IOR('F', 0x01, int)            // Get bytes available for read
#define FIONBIO         _IOW('F', 0x02, int)            // Set non-blocking I/O

/*
 * Error codes for ioctl
 */
#define IOCTL_SUCCESS       0
#define IOCTL_ENOTTY       -1   // Not a typewriter (invalid ioctl for device)
#define IOCTL_EINVAL       -2   // Invalid argument
#define IOCTL_EFAULT       -3   // Bad address
#define IOCTL_ENODEV       -4   // No such device
#define IOCTL_ENOSYS       -5   // Function not implemented

#endif /* IOCTL_H */
