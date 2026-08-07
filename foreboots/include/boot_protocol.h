/* =============================================================================
 * ForeB - Forest Bootloader
 * include/boot_protocol.h - Single source of truth for the boot handoff ABI (C)
 * =============================================================================
 * This header is the C-language mirror of the NASM layout in config.h. It is
 * shared by the native UEFI loader (uefi/bootx64.c) and any C consumer that
 * must produce or read the ForeB boot structures.
 *
 * CRITICAL: struct offsets here MUST stay byte-for-byte identical to config.h
 * and to the kernel's src/include/multiboot.h. Both firmware paths (BIOS
 * stage1/2/3 and native UEFI bootx64.c) hand the kernel an IDENTICAL machine
 * state:
 *
 *   - 32-bit protected mode, paging OFF, interrupts OFF (CLI)
 *   - Flat GDT: CS=0x08 (code, base 0 / limit 4 GiB), DS=ES=FS=GS=SS=0x10
 *   - EAX = MULTIBOOT1_MAGIC (0x2BADB002)
 *   - EBX = physical address of multiboot_info_t (ForeB uses 0x1800)
 *   - EIP = kernel ELF e_entry (e.g. 0x100000)
 *   - Both PICs fully masked (out 0x21,0xFF / out 0xA1,0xFF)
 *
 * All addresses stored in these structs are PHYSICAL. The 64-bit Forest kernel
 * performs its own long-mode transition after entry; the loader stays in
 * 32-bit PM regardless of ELF class.
 * =============================================================================
 */

#ifndef FOREB_BOOT_PROTOCOL_H
#define FOREB_BOOT_PROTOCOL_H

/* -----------------------------------------------------------------------------
 * Fixed-width integer types
 * -----------------------------------------------------------------------------
 * A freestanding UEFI loader may not link a full libc, but clang/gcc always
 * ship a freestanding <stdint.h>. Callers that define their own u8/u16/u32/u64
 * (e.g. a self-contained EFI app) may predefine FOREB_HAVE_STDINT_TYPES to
 * suppress these typedefs.
 */
#ifndef FOREB_HAVE_STDINT_TYPES
#include <stdint.h>
typedef uint8_t  foreb_u8;
typedef uint16_t foreb_u16;
typedef uint32_t foreb_u32;
typedef uint64_t foreb_u64;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Portable compile-time assertion (C11 _Static_assert is a keyword, no header) */
#ifndef FOREB_STATIC_ASSERT
#define FOREB_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

/* =============================================================================
 * Magic values  (mirror config.h lines 270-284)
 * =============================================================================
 */
#define FOREB_BOOT_INFO_MAGIC   0x464F5242u  /* "FORB" - UEFI loader MUST write */
#define FOREB_BOOT_INFO_VER     0x00020000u  /* v2.0 (major<<16 | minor)        */

#define MULTIBOOT1_MAGIC        0x2BADB002u  /* value placed in EAX at handoff  */
#define MULTIBOOT1_HEADER_MAGIC 0x1BADB002u  /* magic in the kernel's MB header */
#define MULTIBOOT2_MAGIC        0x36D76289u

/* =============================================================================
 * foreboots_boot_info.flags bits  (mirror config.h lines 360-372)
 * =============================================================================
 */
#define FOREB_BIF_MMAP             0x00000001u  /* mmap_count/mmap_addr valid    */
#define FOREB_BIF_FRAMEBUFFER      0x00000002u  /* framebuffer_* valid           */
#define FOREB_BIF_CMDLINE          0x00000004u  /* cmdline valid                 */
#define FOREB_BIF_LONG_MODE        0x00000008u  /* long mode supported           */
#define FOREB_BIF_CPUID            0x00000010u  /* CPUID present                 */
#define FOREB_BIF_PAE              0x00000020u  /* PAE supported                 */
#define FOREB_BIF_INITRD           0x00000040u  /* initrd_addr/initrd_size valid */
#define FOREB_BIF_NO_FB            0x00000080u  /* text-mode (nofb) boot         */
#define FOREB_BIF_SAFE             0x00000100u  /* safe-mode boot                */
#define FOREB_BIF_KERNEL_PRELOADED 0x00000200u  /* loader already staged PT_LOADs*/

/* =============================================================================
 * Multiboot1 info flags  (mirror config.h lines 278-284)
 * =============================================================================
 */
#define MB_FLAG_MEM            0x00000001u  /* mem_lower / mem_upper             */
#define MB_FLAG_BOOT_DEVICE    0x00000002u  /* boot_device                       */
#define MB_FLAG_CMDLINE        0x00000004u  /* cmdline                           */
#define MB_FLAG_MODS           0x00000008u  /* mods_count / mods_addr (initrd)   */
#define MB_FLAG_MMAP           0x00000040u  /* mmap_length / mmap_addr           */
#define MB_FLAG_BOOTLOADER     0x00000200u  /* boot_loader_name                  */
#define MB_FLAG_FRAMEBUFFER    0x00001000u  /* framebuffer_*                     */

/* =============================================================================
 * E820 / mmap memory types  (mirror config.h lines 287-291)
 * =============================================================================
 */
#define FOREB_E820_USABLE        1u
#define FOREB_E820_RESERVED      2u
#define FOREB_E820_ACPI_RECLAIM  3u
#define FOREB_E820_ACPI_NVS      4u
#define FOREB_E820_BAD           5u

/* =============================================================================
 * framebuffer_type enum  (shared by both structs; UEFI GOP is always RGB)
 * =============================================================================
 */
enum foreb_framebuffer_type {
    FOREB_FB_INDEXED  = 0,  /* 8bpp palette-indexed                            */
    FOREB_FB_RGB      = 1,  /* >=16bpp direct color (UEFI GOP => this)         */
    FOREB_FB_EGA_TEXT = 2   /* VGA/EGA text console (nofb path)                */
};

/* =============================================================================
 * Fixed physical layout the kernel expects (BIOS ForeB layout the UEFI loader
 * must reproduce). Mirror config.h lines 166-211.
 * =============================================================================
 */
#define FOREB_BOOT_INFO_ADDRESS   0x00001000u  /* foreboots_boot_info (152 B)   */
#define FOREB_MMAP_ADDRESS        0x00001100u  /* foreboots_mmap_entry[32]      */
#define FOREB_MB_MMAP_ADDRESS     0x00001400u  /* multiboot1 mmap array         */
#define FOREB_MULTIBOOT_INFO_ADDR 0x00001800u  /* multiboot_info_t (EBX target) */
#define FOREB_MMAP_MAX            32u          /* max stored E820 entries       */
#define FOREB_KERNEL_LOAD_PHYS    0x00010000u  /* raw ELF file buffer           */
#define FOREB_INITRD_LOAD_PHYS    0x00040000u  /* initrd load address           */

/* Legacy selected-menu-entry indices. RETIRED: the canonical per-entry boot
 * method now lives in `enum forebo_entry_type` (include/forebo_cfg.h), whose
 * FOREB_ENTRY_* members supersede these. These four macros were unused anywhere
 * in the tree and collided with that enum (FOREB_ENTRY_REBOOT), so they are
 * removed. Legacy names kept here in a comment for git-archaeology only:
 *   FOREB_ENTRY_DEFAULT=0, FOREB_ENTRY_NOFB=1, FOREB_ENTRY_SAFE=2,
 *   FOREB_ENTRY_REBOOT=3  ->  see enum forebo_entry_type. */

/* ELF constants (mirror config.h lines 449-454) */
#define FOREB_ELFCLASS32   1u
#define FOREB_ELFCLASS64   2u
#define FOREB_ELFDATA2LSB  1u
#define FOREB_EM_386       3u
#define FOREB_EM_X86_64    62u
#define FOREB_PT_LOAD      1u

/* =============================================================================
 * struct foreboots_mmap_entry  (24 bytes, 64-bit-safe E820 entry)
 * Mirror config.h struc foreboots_mmap_entry (lines 377-382).
 * =============================================================================
 */
#if defined(__GNUC__) || defined(__clang__)
#define FOREB_PACKED __attribute__((packed))
#else
#define FOREB_PACKED
#endif

struct FOREB_PACKED foreboots_mmap_entry {
    foreb_u64 base;    /* offset  0: 64-bit base address                       */
    foreb_u64 length;  /* offset  8: 64-bit length                            */
    foreb_u32 type;    /* offset 16: FOREB_E820_* type                        */
    foreb_u32 acpi;    /* offset 20: ACPI 3.0 extended attributes             */
};

FOREB_STATIC_ASSERT(sizeof(struct foreboots_mmap_entry) == 24,
                    "foreboots_mmap_entry must be 24 bytes");

/* =============================================================================
 * struct foreboots_boot_info  (152 bytes, ForeB's rich boot info @ 0x1000)
 * Mirror config.h struc foreboots_boot_info (lines 319-357).
 * Offsets annotated below MUST match the contract exactly.
 * =============================================================================
 */
struct FOREB_PACKED foreboots_boot_info {
    foreb_u32 magic;                /*   0: FOREB_BOOT_INFO_MAGIC ("FORB")     */
    foreb_u32 version;              /*   4: FOREB_BOOT_INFO_VER                */
    foreb_u32 flags;                /*   8: FOREB_BIF_* bitmask                */
    foreb_u32 boot_disk;            /*  12: BIOS boot drive (DL); 0 on UEFI    */
    foreb_u32 cmdline;              /*  16: phys addr of cmdline string        */
    foreb_u32 boot_loader_name;     /*  20: phys addr of loader name string    */
    foreb_u32 mem_lower;            /*  24: KiB usable RAM below 1 MiB (640)   */
    foreb_u32 mem_upper;            /*  28: KiB usable RAM above 1 MiB         */
    foreb_u32 mmap_count;           /*  32: # foreboots_mmap_entry records     */
    foreb_u32 mmap_addr;            /*  36: phys addr of mmap array (0x1100)   */
    /* Framebuffer (valid when flags & FOREB_BIF_FRAMEBUFFER) */
    foreb_u64 framebuffer_addr;     /*  40: 64-bit phys LFB base (0 if text)   */
    foreb_u32 framebuffer_pitch;    /*  48: bytes per scanline                 */
    foreb_u32 framebuffer_width;    /*  52: width in pixels                    */
    foreb_u32 framebuffer_height;   /*  56: height in pixels                   */
    foreb_u32 framebuffer_bpp;      /*  60: bits per pixel (32 for GOP)        */
    foreb_u32 framebuffer_type;     /*  64: enum foreb_framebuffer_type        */
    foreb_u16 vbe_mode;             /*  68: VBE mode number (0 on UEFI)        */
    foreb_u16 vbe_pad;              /*  70: padding; 0                         */
    /* CPU capability detection */
    foreb_u32 cpuid_available;      /*  72: 1 if CPUID present                 */
    foreb_u32 long_mode_available;  /*  76: 1 if long mode supported           */
    foreb_u32 pae_available;        /*  80: 1 if PAE supported                 */
    /* Kernel image info */
    foreb_u32 kernel_load_addr;     /*  84: phys addr of raw ELF file buffer   */
    foreb_u32 kernel_size;          /*  88: bytes of kernel image loaded       */
    foreb_u32 kernel_entry;         /*  92: ELF e_entry phys addr (MUST set)   */
    foreb_u32 kernel_is64bit;       /*  96: 1 if ELF64, 0 if ELF32             */
    /* Initrd */
    foreb_u32 initrd_addr;          /* 100: phys addr of initrd (0 if none)    */
    foreb_u32 initrd_size;          /* 104: initrd size in bytes               */
    /* Selected boot entry + mode flags */
    foreb_u32 boot_entry;           /* 108: FOREB_ENTRY_* selected in menu     */
    foreb_u32 no_framebuffer;       /* 112: 1 if text-mode (nofb) requested    */
    foreb_u32 safe_mode;            /* 116: 1 if safe-mode requested           */
    /* Reserved for growth (zeroed) */
    foreb_u32 reserved[8];          /* 120..151: 32 bytes reserved             */
};

FOREB_STATIC_ASSERT(sizeof(struct foreboots_boot_info) == 152,
                    "foreboots_boot_info must be 152 bytes");
/* Spot-check a few load-bearing offsets so any accidental repacking is caught. */
FOREB_STATIC_ASSERT(__builtin_offsetof(struct foreboots_boot_info, framebuffer_addr) == 40,
                    "framebuffer_addr must be at offset 40");
FOREB_STATIC_ASSERT(__builtin_offsetof(struct foreboots_boot_info, kernel_entry) == 92,
                    "kernel_entry must be at offset 92");
FOREB_STATIC_ASSERT(__builtin_offsetof(struct foreboots_boot_info, reserved) == 120,
                    "reserved must be at offset 120");

/* =============================================================================
 * struct mb_mmap_entry  (24 bytes; .size field = 20)
 * Mirror config.h struc mb_mmap_entry (lines 387-394).
 *
 * NOTE the multiboot walking convention: mmap_addr points at the .size field;
 * the kernel iterates  p += ((mb_mmap_entry*)p)->size + 4.
 * =============================================================================
 */
struct FOREB_PACKED mb_mmap_entry {
    foreb_u32 size;      /*  0: = sizeof(entry) - 4 = 20                       */
    foreb_u32 addr_low;  /*  4                                                 */
    foreb_u32 addr_high; /*  8                                                 */
    foreb_u32 len_low;   /* 12                                                 */
    foreb_u32 len_high;  /* 16                                                 */
    foreb_u32 type;      /* 20: E820 type                                     */
};

FOREB_STATIC_ASSERT(sizeof(struct mb_mmap_entry) == 24,
                    "mb_mmap_entry must be 24 bytes");

/* =============================================================================
 * struct mb_module  (16 bytes; multiboot module, used for initrd)
 * Mirror config.h struc mb_module (lines 434-439).
 * =============================================================================
 */
struct FOREB_PACKED mb_module {
    foreb_u32 mod_start;  /*  0: phys start                                    */
    foreb_u32 mod_end;    /*  4: phys end                                      */
    foreb_u32 string;     /*  8: phys ptr to string (0 ok)                     */
    foreb_u32 reserved;   /* 12: 0                                             */
};

FOREB_STATIC_ASSERT(sizeof(struct mb_module) == 16,
                    "mb_module must be 16 bytes");

/* =============================================================================
 * struct multiboot_info  (112 bytes; passed to the kernel in EBX @ 0x1800)
 * Mirror config.h struc mb_info (lines 400-429) and the kernel's
 * src/include/multiboot.h multiboot_info_t. Field offsets annotated below.
 *
 * IMPORTANT: framebuffer_bpp/type are single BYTES here (offsets 108/109),
 * unlike foreboots_boot_info where they are u32.
 * =============================================================================
 */
struct FOREB_PACKED multiboot_info {
    foreb_u32 flags;              /*   0: MB_FLAG_* bitmask                    */
    foreb_u32 mem_lower;          /*   4: KiB below 1 MiB (640)                */
    foreb_u32 mem_upper;          /*   8: KiB above 1 MiB                      */
    foreb_u32 boot_device;        /*  12: BIOS boot device (0 on UEFI)        */
    foreb_u32 cmdline;            /*  16: phys ptr to cmdline string          */
    foreb_u32 mods_count;         /*  20: 1 if initrd else 0                   */
    foreb_u32 mods_addr;          /*  24: phys ptr to mb_module[] else 0      */
    foreb_u8  syms[16];           /*  28: zeroed (unused)                      */
    foreb_u32 mmap_length;        /*  44: total bytes of mmap array           */
    foreb_u32 mmap_addr;          /*  48: phys ptr to mb_mmap_entry[] (0x1400)*/
    foreb_u32 drives_length;      /*  52: 0                                    */
    foreb_u32 drives_addr;        /*  56: 0                                    */
    foreb_u32 config_table;       /*  60: 0                                    */
    foreb_u32 boot_loader_name;   /*  64: phys ptr to name string             */
    foreb_u32 apm_table;          /*  68: 0                                    */
    foreb_u32 vbe_control_info;   /*  72: 0 on UEFI                            */
    foreb_u32 vbe_mode_info;      /*  76: 0 on UEFI                            */
    foreb_u16 vbe_mode;           /*  80: 0 on UEFI                            */
    foreb_u16 vbe_interface_seg;  /*  82: 0                                    */
    foreb_u16 vbe_interface_off;  /*  84: 0                                    */
    foreb_u16 vbe_interface_len;  /*  86: 0                                    */
    foreb_u64 framebuffer_addr;   /*  88: 64-bit phys LFB base                 */
    foreb_u32 framebuffer_pitch;  /*  96: bytes per scanline                   */
    foreb_u32 framebuffer_width;  /* 100: width in pixels                      */
    foreb_u32 framebuffer_height; /* 104: height in pixels                     */
    foreb_u8  framebuffer_bpp;    /* 108: bits per pixel (u8!)                 */
    foreb_u8  framebuffer_type;   /* 109: enum foreb_framebuffer_type (u8!)    */
    foreb_u16 framebuffer_color_info; /* 110: RGB field positions/masks or 0   */
};

FOREB_STATIC_ASSERT(sizeof(struct multiboot_info) == 112,
                    "multiboot_info must be 112 bytes");
FOREB_STATIC_ASSERT(__builtin_offsetof(struct multiboot_info, mmap_addr) == 48,
                    "multiboot_info.mmap_addr must be at offset 48");
FOREB_STATIC_ASSERT(__builtin_offsetof(struct multiboot_info, boot_loader_name) == 64,
                    "multiboot_info.boot_loader_name must be at offset 64");
FOREB_STATIC_ASSERT(__builtin_offsetof(struct multiboot_info, framebuffer_addr) == 88,
                    "multiboot_info.framebuffer_addr must be at offset 88");
FOREB_STATIC_ASSERT(__builtin_offsetof(struct multiboot_info, framebuffer_bpp) == 108,
                    "multiboot_info.framebuffer_bpp must be at offset 108");

/* Convenience aliases matching the recon/contract wording. */
typedef struct foreboots_boot_info  foreboots_boot_info_t;
typedef struct foreboots_mmap_entry foreboots_mmap_entry_t;
typedef struct multiboot_info       multiboot_info_t;
typedef struct mb_mmap_entry        mb_mmap_entry_t;
typedef struct mb_module            mb_module_t;

#ifdef __cplusplus
}
#endif

#endif /* FOREB_BOOT_PROTOCOL_H */
