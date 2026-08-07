/**
 * Fern Graphics Hardware Register Definitions
 * 
 * This file contains hardware register definitions from official documentation:
 * - Bochs VBE Extensions specification
 * - VMware SVGA-II device specification (SVGA3D included)
 * - VGA standard registers
 * - Intel Graphics Programming Reference Manual
 */

#ifndef GRAPHICS_HW_REGS_H
#define GRAPHICS_HW_REGS_H

#include "../stdint.h"

/* ============================================================================
 * VGA Standard Registers
 * ============================================================================ */

/* VGA I/O Ports */
#define VGA_MISC_READ           0x3CC
#define VGA_MISC_WRITE          0x3C2
#define VGA_INPUT_STATUS0       0x3C2
#define VGA_INPUT_STATUS1_MONO  0x3BA
#define VGA_INPUT_STATUS1_COLOR 0x3DA

/* Attribute Controller */
#define VGA_AC_INDEX            0x3C0
#define VGA_AC_WRITE            0x3C0
#define VGA_AC_READ             0x3C1

/* Sequencer Registers */
#define VGA_SEQ_INDEX           0x3C4
#define VGA_SEQ_DATA            0x3C5
#define VGA_SEQ_RESET           0x00
#define VGA_SEQ_CLOCK_MODE      0x01
#define VGA_SEQ_PLANE_WRITE     0x02
#define VGA_SEQ_CHAR_MAP_SELECT 0x03
#define VGA_SEQ_MEMORY_MODE     0x04

/* Graphics Controller */
#define VGA_GC_INDEX            0x3CE
#define VGA_GC_DATA             0x3CF
#define VGA_GC_SET_RESET        0x00
#define VGA_GC_ENABLE_SET_RESET 0x01
#define VGA_GC_COLOR_COMPARE    0x02
#define VGA_GC_DATA_ROTATE      0x03
#define VGA_GC_READ_MAP_SELECT  0x04
#define VGA_GC_MODE             0x05
#define VGA_GC_MISC             0x06
#define VGA_GC_COLOR_DONT_CARE  0x07
#define VGA_GC_BIT_MASK         0x08

/* CRTC Registers */
#define VGA_CRTC_INDEX_MONO     0x3B4
#define VGA_CRTC_DATA_MONO      0x3B5
#define VGA_CRTC_INDEX_COLOR    0x3D4
#define VGA_CRTC_DATA_COLOR     0x3D5

/* CRTC Register Indices */
#define VGA_CRTC_H_TOTAL        0x00
#define VGA_CRTC_H_DISP_END     0x01
#define VGA_CRTC_H_BLANK_START  0x02
#define VGA_CRTC_H_BLANK_END    0x03
#define VGA_CRTC_H_SYNC_START   0x04
#define VGA_CRTC_H_SYNC_END     0x05
#define VGA_CRTC_V_TOTAL        0x06
#define VGA_CRTC_OVERFLOW       0x07
#define VGA_CRTC_PRESET_ROW     0x08
#define VGA_CRTC_MAX_SCAN_LINE  0x09
#define VGA_CRTC_CURSOR_START   0x0A
#define VGA_CRTC_CURSOR_END     0x0B
#define VGA_CRTC_START_ADDR_HI  0x0C
#define VGA_CRTC_START_ADDR_LO  0x0D
#define VGA_CRTC_CURSOR_LOC_HI  0x0E
#define VGA_CRTC_CURSOR_LOC_LO  0x0F
#define VGA_CRTC_V_SYNC_START   0x10
#define VGA_CRTC_V_SYNC_END     0x11
#define VGA_CRTC_V_DISP_END     0x12
#define VGA_CRTC_OFFSET         0x13
#define VGA_CRTC_UNDERLINE_LOC  0x14
#define VGA_CRTC_V_BLANK_START  0x15
#define VGA_CRTC_V_BLANK_END    0x16
#define VGA_CRTC_MODE_CONTROL   0x17
#define VGA_CRTC_LINE_COMPARE   0x18

/* DAC (Palette) Registers */
#define VGA_DAC_MASK            0x3C6
#define VGA_DAC_READ_INDEX      0x3C7
#define VGA_DAC_WRITE_INDEX     0x3C8
#define VGA_DAC_DATA            0x3C9

/* VGA Memory */
#define VGA_TEXT_FRAMEBUFFER    0xB8000
#define VGA_GFX_FRAMEBUFFER     0xA0000
#define VGA_TEXT_COLS           80
#define VGA_TEXT_ROWS           25
#define VGA_TEXT_SIZE           (VGA_TEXT_COLS * VGA_TEXT_ROWS * 2)

/* VGA Text Mode Attributes */
#define VGA_ATTR_BLACK          0x00
#define VGA_ATTR_BLUE           0x01
#define VGA_ATTR_GREEN          0x02
#define VGA_ATTR_CYAN           0x03
#define VGA_ATTR_RED            0x04
#define VGA_ATTR_MAGENTA        0x05
#define VGA_ATTR_BROWN          0x06
#define VGA_ATTR_LIGHT_GRAY     0x07
#define VGA_ATTR_DARK_GRAY      0x08
#define VGA_ATTR_LIGHT_BLUE     0x09
#define VGA_ATTR_LIGHT_GREEN    0x0A
#define VGA_ATTR_LIGHT_CYAN     0x0B
#define VGA_ATTR_LIGHT_RED      0x0C
#define VGA_ATTR_LIGHT_MAGENTA  0x0D
#define VGA_ATTR_YELLOW         0x0E
#define VGA_ATTR_WHITE          0x0F

/* VGA attribute byte: fg (bits 0-3) | bg (bits 4-6).  Bit 7 is the blink
 * bit and must not be set by the background value.  Mask bg to 3 bits. */
#define VGA_MAKE_ATTR(fg, bg)   ((((bg) & 0x07) << 4) | ((fg) & 0x0F))

/* ============================================================================
 * Bochs VBE Extensions (BGA) Registers
 * From official Bochs VBE specification
 * ============================================================================ */

/* Bochs/QEMU VGA PCI IDs */
#define BGA_PCI_VENDOR_BOCHS    0x1234
#define BGA_PCI_DEVICE_BGA      0x1111
#define BGA_PCI_VENDOR_QEMU     0x1B36  /* QEMU QXL */
#define BGA_PCI_DEVICE_QXL      0x0100

/* VBE Dispi Ports (index/data pair) */
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF

/* Alternate Bochs ports */
#define VBE_DISPI_IOPORT_INDEX_ALT  0xFF80  /* MMIO offset */
#define VBE_DISPI_IOPORT_DATA_ALT   0xFF81  /* MMIO offset */

/* VBE Dispi Index Registers */
#define VBE_DISPI_INDEX_ID              0x0
#define VBE_DISPI_INDEX_XRES            0x1
#define VBE_DISPI_INDEX_YRES            0x2
#define VBE_DISPI_INDEX_BPP             0x3
#define VBE_DISPI_INDEX_ENABLE          0x4
#define VBE_DISPI_INDEX_BANK            0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH      0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT     0x7
#define VBE_DISPI_INDEX_X_OFFSET        0x8
#define VBE_DISPI_INDEX_Y_OFFSET        0x9
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0xa  /* Added in newer versions */

/* VBE Dispi ID Versions */
#define VBE_DISPI_ID0           0xB0C0
#define VBE_DISPI_ID1           0xB0C1
#define VBE_DISPI_ID2           0xB0C2
#define VBE_DISPI_ID3           0xB0C3
#define VBE_DISPI_ID4           0xB0C4
#define VBE_DISPI_ID5           0xB0C5

/* VBE Dispi Enable Register Bits */
#define VBE_DISPI_DISABLED      0x00
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_GETCAPS       0x02
#define VBE_DISPI_8BIT_DAC      0x20
#define VBE_DISPI_LFB_ENABLED   0x40
#define VBE_DISPI_NOCLEARMEM    0x80

/* BGA Default Framebuffer Locations */
#define BGA_LFB_PHYSICAL_ADDRESS    0xE0000000  /* Common default */
#define BGA_LFB_PHYSICAL_ADDRESS_2  0xFD000000  /* Alternative */

/* BGA Limits */
#define BGA_MAX_XRES            16000  /* Max X resolution in BGA v5 */
#define BGA_MAX_YRES            12000  /* Max Y resolution */
#define BGA_MAX_BPP             32

/* BGA Bank Size */
#define BGA_BANK_SIZE           65536  /* 64 KB per bank */

/* ============================================================================
 * VMware SVGA-II Registers
 * From VMware SVGA Device Interface Specification
 * ============================================================================ */

/* VMware SVGA PCI IDs */
#define VMWARE_PCI_VENDOR       0x15AD
#define VMWARE_SVGA_PCI_DEVICE  0x0405
#define VMWARE_SVGA_PCI_DEVICE_OLD  0x0710

/* VMware SVGA I/O Ports (relative to PCI BAR 0) */
#define SVGA_INDEX_PORT         0x00
#define SVGA_VALUE_PORT         0x01
#define SVGA_BIOS_PORT          0x02
#define SVGA_IRQSTATUS_PORT     0x08

/* SVGA Register Indices */
typedef enum {
    SVGA_REG_ID = 0,
    SVGA_REG_ENABLE = 1,
    SVGA_REG_WIDTH = 2,
    SVGA_REG_HEIGHT = 3,
    SVGA_REG_MAX_WIDTH = 4,
    SVGA_REG_MAX_HEIGHT = 5,
    SVGA_REG_DEPTH = 6,
    SVGA_REG_BITS_PER_PIXEL = 7,
    SVGA_REG_PSEUDOCOLOR = 8,
    SVGA_REG_RED_MASK = 9,
    SVGA_REG_GREEN_MASK = 10,
    SVGA_REG_BLUE_MASK = 11,
    SVGA_REG_BYTES_PER_LINE = 12,
    SVGA_REG_FB_START = 13,
    SVGA_REG_FB_OFFSET = 14,
    SVGA_REG_VRAM_SIZE = 15,
    SVGA_REG_FB_SIZE = 16,
    
    /* Extended capabilities and FIFO */
    SVGA_REG_CAPABILITIES = 17,
    SVGA_REG_MEM_START = 18,
    SVGA_REG_MEM_SIZE = 19,
    SVGA_REG_CONFIG_DONE = 20,
    SVGA_REG_SYNC = 21,
    SVGA_REG_BUSY = 22,
    SVGA_REG_GUEST_ID = 23,
    SVGA_REG_CURSOR_ID = 24,
    SVGA_REG_CURSOR_X = 25,
    SVGA_REG_CURSOR_Y = 26,
    SVGA_REG_CURSOR_ON = 27,
    SVGA_REG_HOST_BITS_PER_PIXEL = 28,
    SVGA_REG_SCRATCH_SIZE = 29,
    SVGA_REG_MEM_REGS = 30,
    SVGA_REG_NUM_DISPLAYS = 31,
    SVGA_REG_PITCHLOCK = 32,
    SVGA_REG_IRQMASK = 33,
    
    /* SVGA 3D and Display registers */
    SVGA_REG_NUM_GUEST_DISPLAYS = 34,
    SVGA_REG_DISPLAY_ID = 35,
    SVGA_REG_DISPLAY_IS_PRIMARY = 36,
    SVGA_REG_DISPLAY_POSITION_X = 37,
    SVGA_REG_DISPLAY_POSITION_Y = 38,
    SVGA_REG_DISPLAY_WIDTH = 39,
    SVGA_REG_DISPLAY_HEIGHT = 40,
    SVGA_REG_GMR_ID = 41,
    SVGA_REG_GMR_DESCRIPTOR = 42,
    SVGA_REG_GMR_MAX_IDS = 43,
    SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH = 44,
    SVGA_REG_TRACES = 45,
    SVGA_REG_GMRS_MAX_PAGES = 46,
    SVGA_REG_MEMORY_SIZE = 47,
    SVGA_REG_COMMAND_LOW = 48,
    SVGA_REG_COMMAND_HIGH = 49,
    SVGA_REG_MAX_PRIMARY_BOUNDING_BOX_MEM = 50,
    SVGA_REG_SUGGESTED_GBOBJECT_MEM_SIZE_KB = 51,
    SVGA_REG_DEV_CAP = 52,
    SVGA_REG_CMD_PREPEND_LOW = 53,
    SVGA_REG_CMD_PREPEND_HIGH = 54,
    SVGA_REG_SCREENTARGET_MAX_WIDTH = 55,
    SVGA_REG_SCREENTARGET_MAX_HEIGHT = 56,
    SVGA_REG_MOB_MAX_SIZE = 57,
    SVGA_REG_TOP = 58,  /* Number of valid SVGA_REG_* */
} svga_reg_t;

/* SVGA ID Values */
#define SVGA_ID_0               0x900000UL
#define SVGA_ID_1               0x900001UL
#define SVGA_ID_2               0x900002UL
#define SVGA_ID_INVALID         0xFFFFFFFFUL

/* SVGA Capabilities */
#define SVGA_CAP_NONE               0x00000000
#define SVGA_CAP_RECT_COPY          0x00000002
#define SVGA_CAP_CURSOR             0x00000020
#define SVGA_CAP_CURSOR_BYPASS      0x00000040
#define SVGA_CAP_CURSOR_BYPASS_2    0x00000080
#define SVGA_CAP_8BIT_EMULATION     0x00000100
#define SVGA_CAP_ALPHA_CURSOR       0x00000200
#define SVGA_CAP_3D                 0x00004000
#define SVGA_CAP_EXTENDED_FIFO      0x00008000
#define SVGA_CAP_MULTIMON           0x00010000
#define SVGA_CAP_PITCHLOCK          0x00020000
#define SVGA_CAP_IRQMASK            0x00040000
#define SVGA_CAP_DISPLAY_TOPOLOGY   0x00080000
#define SVGA_CAP_GMR                0x00100000
#define SVGA_CAP_TRACES             0x00200000
#define SVGA_CAP_GMR2               0x00400000
#define SVGA_CAP_SCREEN_OBJECT_2    0x00800000
#define SVGA_CAP_COMMAND_BUFFERS    0x01000000
#define SVGA_CAP_DEAD1              0x02000000
#define SVGA_CAP_CMD_BUFFERS_2      0x04000000
#define SVGA_CAP_GBOBJECTS          0x08000000
#define SVGA_CAP_DX                 0x10000000
#define SVGA_CAP_HP_CMD_QUEUE       0x20000000
#define SVGA_CAP_NO_BB_RESTRICTION  0x40000000
#define SVGA_CAP_CAP2_REGISTER      0x80000000

/* SVGA FIFO Registers (offsets into FIFO memory) */
typedef enum {
    SVGA_FIFO_MIN = 0,
    SVGA_FIFO_MAX = 1,
    SVGA_FIFO_NEXT_CMD = 2,
    SVGA_FIFO_STOP = 3,
    
    /* Extended FIFO (when SVGA_CAP_EXTENDED_FIFO) */
    SVGA_FIFO_CAPABILITIES = 4,
    SVGA_FIFO_FLAGS = 5,
    SVGA_FIFO_FENCE = 6,
    SVGA_FIFO_3D_HWVERSION = 7,
    SVGA_FIFO_PITCHLOCK = 8,
    SVGA_FIFO_CURSOR_ON = 9,
    SVGA_FIFO_CURSOR_X = 10,
    SVGA_FIFO_CURSOR_Y = 11,
    SVGA_FIFO_CURSOR_COUNT = 12,
    SVGA_FIFO_CURSOR_LAST_UPDATED = 13,
    SVGA_FIFO_RESERVED = 14,
    SVGA_FIFO_CURSOR_SCREEN_ID = 15,
    SVGA_FIFO_DEAD = 16,
    SVGA_FIFO_3D_HWVERSION_REVISED = 17,
    SVGA_FIFO_3D_CAPS = 32,
    SVGA_FIFO_3D_CAPS_LAST = 32 + 255,
    SVGA_FIFO_GUEST_3D_HWVERSION = SVGA_FIFO_3D_CAPS_LAST + 1,
    SVGA_FIFO_FENCE_GOAL = SVGA_FIFO_GUEST_3D_HWVERSION + 1,
    SVGA_FIFO_BUSY = SVGA_FIFO_FENCE_GOAL + 1,
    SVGA_FIFO_NUM_REGS = SVGA_FIFO_BUSY + 1,
} svga_fifo_reg_t;

/* SVGA FIFO Command IDs */
typedef enum {
    SVGA_CMD_INVALID_CMD = 0,
    SVGA_CMD_UPDATE = 1,
    SVGA_CMD_RECT_COPY = 3,
    SVGA_CMD_RECT_ROP_COPY = 14,
    SVGA_CMD_DEFINE_CURSOR = 19,
    SVGA_CMD_DEFINE_ALPHA_CURSOR = 22,
    SVGA_CMD_UPDATE_VERBOSE = 25,
    SVGA_CMD_FRONT_ROP_FILL = 29,
    SVGA_CMD_FENCE = 30,
    SVGA_CMD_ESCAPE = 33,
    SVGA_CMD_DEFINE_SCREEN = 34,
    SVGA_CMD_DESTROY_SCREEN = 35,
    SVGA_CMD_DEFINE_GMRFB = 36,
    SVGA_CMD_BLIT_GMRFB_TO_SCREEN = 37,
    SVGA_CMD_BLIT_SCREEN_TO_GMRFB = 38,
    SVGA_CMD_ANNOTATION_FILL = 39,
    SVGA_CMD_ANNOTATION_COPY = 40,
    SVGA_CMD_DEFINE_GMR2 = 41,
    SVGA_CMD_REMAP_GMR2 = 42,
    SVGA_CMD_DEAD = 43,
    SVGA_CMD_DEAD_2 = 44,
    SVGA_CMD_NOP = 45,
    SVGA_CMD_NOP_ERROR = 46,
    SVGA_CMD_MAX = 47,
} svga_cmd_t;

/* SVGA IRQ Flags */
#define SVGA_IRQFLAG_ANY_FENCE      0x1
#define SVGA_IRQFLAG_FIFO_PROGRESS  0x2
#define SVGA_IRQFLAG_FENCE_GOAL     0x4

/* ============================================================================
 * VirtualBox VBVA Registers
 * ============================================================================ */

#define VBOX_PCI_VENDOR         0x80EE
#define VBOX_PCI_DEVICE_VESA    0xBEEF
#define VBOX_PCI_DEVICE_VGA     0xCAFE

/* VirtualBox uses BGA-compatible interface plus HGSMI extensions */
/* The framebuffer is typically at PCI BAR 0 */

/* ============================================================================
 * Intel HD Graphics Registers (Basic subset for mode setting)
 * From Intel Open Source Graphics Drivers
 * ============================================================================ */

/* Intel PCI Vendor ID */
#define INTEL_PCI_VENDOR        0x8086

/* Intel device IDs (sample - there are hundreds) */
#define INTEL_PCI_DEVICE_HD2000     0x0102
#define INTEL_PCI_DEVICE_HD3000     0x0116
#define INTEL_PCI_DEVICE_HD4000     0x0162
#define INTEL_PCI_DEVICE_HD5000     0x0412
#define INTEL_PCI_DEVICE_SKYLAKE    0x1912
#define INTEL_PCI_DEVICE_KABYLAKE   0x5912

/* MMIO Register offsets */
#define INTEL_MMIO_SIZE         0x200000  /* 2 MB */

/* VGA Registers (offset from MMIO base) */
#define INTEL_VGA_CNTRL         0x71400

/* Display Engine Registers */
#define INTEL_DSPARB            0x70030  /* Display Arbitration Control */
#define INTEL_DSPFW1            0x70034  /* Display FIFO Watermark 1 */
#define INTEL_DSPFW2            0x70038
#define INTEL_DSPFW3            0x7003C

/* Pipe A */
#define INTEL_HTOTAL_A          0x60000
#define INTEL_HBLANK_A          0x60004
#define INTEL_HSYNC_A           0x60008
#define INTEL_VTOTAL_A          0x6000C
#define INTEL_VBLANK_A          0x60010
#define INTEL_VSYNC_A           0x60014
#define INTEL_PIPEASRC          0x6001C
#define INTEL_BCLRPAT_A         0x60020
#define INTEL_VSYNCSHIFT_A      0x60028

/* Pipe A Control */
#define INTEL_PIPEACONF         0x70008

/* Pipe B */
#define INTEL_HTOTAL_B          0x61000
#define INTEL_HBLANK_B          0x61004
#define INTEL_HSYNC_B           0x61008
#define INTEL_VTOTAL_B          0x6100C
#define INTEL_VBLANK_B          0x61010
#define INTEL_VSYNC_B           0x61014
#define INTEL_PIPEBSRC          0x6101C
#define INTEL_BCLRPAT_B         0x61020
#define INTEL_VSYNCSHIFT_B      0x61028

/* Pipe B Control */
#define INTEL_PIPEBCONF         0x71008

/* Plane A */
#define INTEL_DSPACNTR          0x70180  /* Display Plane A Control */
#define INTEL_DSPALINOFF        0x70184  /* Linear Offset */
#define INTEL_DSPASTRIDE        0x70188  /* Stride */
#define INTEL_DSPAPOS           0x7018C  /* Position */
#define INTEL_DSPASIZE          0x70190  /* Size */
#define INTEL_DSPASURF          0x7019C  /* Surface Base Address */
#define INTEL_DSPATILEOFF       0x701A4  /* Tile Offset */

/* Plane B */
#define INTEL_DSPBCNTR          0x71180
#define INTEL_DSPBLINOFF        0x71184
#define INTEL_DSPBSTRIDE        0x71188
#define INTEL_DSPBPOS           0x7118C
#define INTEL_DSPBSIZE          0x71190
#define INTEL_DSPBSURF          0x7119C
#define INTEL_DSPBTILEOFF       0x711A4

/* Display Plane Control Bits */
#define INTEL_DSPCNTR_ENABLE    (1 << 31)
#define INTEL_DSPCNTR_FORMAT_MASK   (0xF << 26)
#define INTEL_DSPCNTR_FORMAT_BGRX8888   (0x6 << 26)
#define INTEL_DSPCNTR_FORMAT_RGBX8888   (0x8 << 26)
#define INTEL_DSPCNTR_TILED     (1 << 10)

/* Pipe Control Bits */
#define INTEL_PIPE_ENABLE       (1 << 31)
#define INTEL_PIPE_STATE        (1 << 30)

/* GMBUS (I2C for EDID) Registers */
#define INTEL_GMBUS0            0xC5100  /* Clock/Port Select */
#define INTEL_GMBUS1            0xC5104  /* Command/Status */
#define INTEL_GMBUS2            0xC5108  /* Status */
#define INTEL_GMBUS3            0xC510C  /* Data Buffer */
#define INTEL_GMBUS4            0xC5110  /* Interrupt Mask */

/* GMBUS Port Select Values */
#define INTEL_GMBUS_PORT_SSC    0x01
#define INTEL_GMBUS_PORT_VGADDC 0x02
#define INTEL_GMBUS_PORT_PANEL  0x03
#define INTEL_GMBUS_PORT_DPCTRL 0x04
#define INTEL_GMBUS_PORT_DPIDX  0x05

/* GMBUS Command Bits */
#define INTEL_GMBUS_SW_RDY      (1 << 30)
#define INTEL_GMBUS_SW_CLR_INT  (1 << 31)
#define INTEL_GMBUS_CYCLE_INDEX (1 << 28)
#define INTEL_GMBUS_CYCLE_STOP  (1 << 27)
#define INTEL_GMBUS_BYTE_COUNT(n)  (((n) & 0x1FF) << 16)
#define INTEL_GMBUS_SLAVE_ADDR(a)  ((a) << 1)
#define INTEL_GMBUS_READ        (1 << 0)
#define INTEL_GMBUS_WRITE       (0 << 0)

/* GMBUS Status Bits */
#define INTEL_GMBUS_HW_RDY      (1 << 11)
#define INTEL_GMBUS_HW_WAIT     (1 << 14)
#define INTEL_GMBUS_NAK         (1 << 10)

/* ============================================================================
 * AMD/ATI Registers (Basic subset)
 * ============================================================================ */

/* AMD PCI Vendor IDs */
#define AMD_PCI_VENDOR          0x1002
#define ATI_PCI_VENDOR          0x1002

/* MMIO Registers for older Radeon */
#define RADEON_CRTC_GEN_CNTL        0x0050
#define RADEON_CRTC_EXT_CNTL        0x0054
#define RADEON_CRTC_H_TOTAL_DISP    0x0200
#define RADEON_CRTC_H_SYNC_STRT_WID 0x0204
#define RADEON_CRTC_V_TOTAL_DISP    0x0208
#define RADEON_CRTC_V_SYNC_STRT_WID 0x020C
#define RADEON_CRTC_OFFSET          0x0224
#define RADEON_CRTC_OFFSET_CNTL     0x0228
#define RADEON_CRTC_PITCH           0x022C

/* CRTC Control Bits */
#define RADEON_CRTC_EN              (1 << 0)
#define RADEON_CRTC_DISPLAY_DIS     (1 << 22)
#define RADEON_CRTC_VSYNC_DIS       (1 << 26)
#define RADEON_CRTC_HSYNC_DIS       (1 << 27)

/* Display Controller Registers */
#define RADEON_DAC_CNTL             0x0058
#define RADEON_DAC_MASK             0x00F0
#define RADEON_DAC_W_INDEX          0x00F8
#define RADEON_DAC_DATA             0x00FC

/* Surface Registers */
#define RADEON_SURFACE0_INFO        0x0B00
#define RADEON_SURFACE0_LOWER_BOUND 0x0B04
#define RADEON_SURFACE0_UPPER_BOUND 0x0B08
#define RADEON_SURFACE1_INFO        0x0B10
#define RADEON_SURFACE2_INFO        0x0B20

/* Memory Controller */
#define RADEON_MC_FB_LOCATION       0x0148
#define RADEON_MC_AGP_LOCATION      0x014C

/* ============================================================================
 * NVIDIA Registers (Basic subset for framebuffer)
 * From Nouveau project documentation
 * ============================================================================ */

/* NVIDIA PCI Vendor ID */
#define NVIDIA_PCI_VENDOR       0x10DE

/* PRAMIN - Instance Memory */
#define NV_PRAMIN               0x00700000

/* PFIFO - Command Submission */
#define NV_PFIFO                0x00002000

/* PGRAPH - 2D/3D Engine */
#define NV_PGRAPH               0x00400000

/* PFB - Framebuffer Controller */
#define NV_PFB                  0x00100000
#define NV_PFB_CFG0             0x00100200
#define NV_PFB_CSTATUS          0x0010020C

/* PCRTC - Display Controller */
#define NV_PCRTC                0x00600000
#define NV_PCRTC_INTR_0         0x00600100
#define NV_PCRTC_INTR_EN_0      0x00600140
#define NV_PCRTC_START          0x00600800
#define NV_PCRTC_CONFIG         0x00600804

/* PRAMDAC - RAMDAC Control */
#define NV_PRAMDAC              0x00680000
#define NV_PRAMDAC_VPLL_COEFF   0x00680508
#define NV_PRAMDAC_PLL_COEFF_SELECT 0x0068050C
#define NV_PRAMDAC_GENERAL_CONTROL  0x00680600

/* Helper macros */
#define NV_RD32(base, reg)      (*(volatile uint32_t*)((base) + (reg)))
#define NV_WR32(base, reg, val) (*(volatile uint32_t*)((base) + (reg)) = (val))

/* ============================================================================
 * Port I/O Helper Functions
 * ============================================================================ */

static inline void gfx_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void gfx_outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void gfx_outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t gfx_inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint16_t gfx_inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint32_t gfx_inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* MMIO helpers */
static inline uint32_t gfx_mmio_read32(volatile void* addr) {
    return *(volatile uint32_t*)addr;
}

static inline void gfx_mmio_write32(volatile void* addr, uint32_t val) {
    *(volatile uint32_t*)addr = val;
    __asm__ volatile("" ::: "memory");  /* Memory barrier */
}

static inline uint16_t gfx_mmio_read16(volatile void* addr) {
    return *(volatile uint16_t*)addr;
}

static inline void gfx_mmio_write16(volatile void* addr, uint16_t val) {
    *(volatile uint16_t*)addr = val;
    __asm__ volatile("" ::: "memory");
}

/* ============================================================================
 * VBE/VESA Helper Macros
 * ============================================================================ */

/* Bochs/QEMU VBE register access */
#define BGA_WRITE_REG(index, value) do { \
    gfx_outw(VBE_DISPI_IOPORT_INDEX, (index)); \
    gfx_outw(VBE_DISPI_IOPORT_DATA, (value)); \
} while(0)

#define BGA_READ_REG(index) ({ \
    gfx_outw(VBE_DISPI_IOPORT_INDEX, (index)); \
    gfx_inw(VBE_DISPI_IOPORT_DATA); \
})

/* VMware SVGA register access */
#define SVGA_WRITE_REG(dev, index, value) do { \
    gfx_outl((dev)->io_base + SVGA_INDEX_PORT, (index)); \
    gfx_outl((dev)->io_base + SVGA_VALUE_PORT, (value)); \
} while(0)

#define SVGA_READ_REG(dev, index) ({ \
    gfx_outl((dev)->io_base + SVGA_INDEX_PORT, (index)); \
    gfx_inl((dev)->io_base + SVGA_VALUE_PORT); \
})

/* VGA register access helpers */
#define VGA_WRITE_CRTC(index, value) do { \
    gfx_outb(VGA_CRTC_INDEX_COLOR, (index)); \
    gfx_outb(VGA_CRTC_DATA_COLOR, (value)); \
} while(0)

#define VGA_READ_CRTC(index) ({ \
    gfx_outb(VGA_CRTC_INDEX_COLOR, (index)); \
    gfx_inb(VGA_CRTC_DATA_COLOR); \
})

#define VGA_WRITE_SEQ(index, value) do { \
    gfx_outb(VGA_SEQ_INDEX, (index)); \
    gfx_outb(VGA_SEQ_DATA, (value)); \
} while(0)

#define VGA_READ_SEQ(index) ({ \
    gfx_outb(VGA_SEQ_INDEX, (index)); \
    gfx_inb(VGA_SEQ_DATA); \
})

#define VGA_WRITE_GC(index, value) do { \
    gfx_outb(VGA_GC_INDEX, (index)); \
    gfx_outb(VGA_GC_DATA, (value)); \
} while(0)

#define VGA_READ_GC(index) ({ \
    gfx_outb(VGA_GC_INDEX, (index)); \
    gfx_inb(VGA_GC_DATA); \
})

/* ============================================================================
 * PCI Compatibility Wrappers
 * Maps simplified pci_read_config/pci_write_config to Fern PCI API
 * ============================================================================ */

#include "../pci.h"

/* Simplified PCI read - assumes segment 0, register in bytes */
#define pci_read_config(bus, slot, func, reg) \
    pci_config_read32(0, (bus), (slot), (func), (reg))

/* Simplified PCI write - assumes segment 0, register in bytes */
#define pci_write_config(bus, slot, func, reg, value) \
    pci_config_write32(0, (bus), (slot), (func), (reg), (value))

/* ============================================================================
 * Memory Mapping Compatibility
 * For graphics framebuffers, GRUB/Multiboot sets up identity mapping during boot.
 * The VMM in vmm_init() explicitly identity-maps the multiboot framebuffer
 * (see vmm.c lines 480-516) before paging is enabled.
 * ============================================================================ */

/**
 * Map physical memory to virtual address space.
 * 
 * IMPORTANT: This works because:
 * 1. vmm_init() identity-maps the framebuffer at boot time (phys == virt)
 * 2. GRUB provides framebuffer info in multiboot tags
 * 3. The kernel processes this before enabling paging
 * 
 * The (size) parameter is unused since identity mapping is pre-established.
 * For memory regions NOT identity-mapped at boot, use vmm_identity_map_range().
 */
#define map_physical_memory(phys_addr, size) ((void*)(uintptr_t)(phys_addr))

#endif /* GRAPHICS_HW_REGS_H */
