; =============================================================================
; ForeB - Forest Bootloader
; config.h (NASM include) - Boot Configuration Constants & Struct Layouts
; =============================================================================
; Included by stage1.asm, stage2.asm, stage3.asm via %include.
;
; Every tunable below is wrapped in %ifndef so the main build can override it
; on the NASM command line with -D, e.g.:
;     nasm -f bin -DFOREBO_DEFAULT_WIDTH=1280 -DFOREBO_DEFAULT_HEIGHT=720 ...
;
; All addresses are PHYSICAL (real-mode / flat PM) unless noted.
; =============================================================================

%ifndef FOREB_CONFIG_H
%define FOREB_CONFIG_H

; -----------------------------------------------------------------------------
; ForeB version
; -----------------------------------------------------------------------------
%ifndef FOREB_VERSION_MAJOR
%define FOREB_VERSION_MAJOR    2
%endif
%ifndef FOREB_VERSION_MINOR
%define FOREB_VERSION_MINOR    0
%endif

; -----------------------------------------------------------------------------
; Disk Layout (LBA sectors, 512 bytes each)
; -----------------------------------------------------------------------------
;   Sector  0          : Stage 1 (MBR, 512 bytes + partition table + 0xAA55)
;   Sectors 1..16      : Stage 2  (16 sectors = 8192 bytes, real-mode GUI/BIOS)
;   Sectors 17..32     : Stage 3  (16 sectors = 8192 bytes, PM/long-mode trampoline)
;   Sector  48+        : Kernel ELF image (raw file, parsed by stage3)
;   Sector  (48+K)..   : Optional initrd (if FOREB_INITRD_START_SECTOR != 0)
%ifndef STAGE2_START_SECTOR
%define STAGE2_START_SECTOR    1
%endif
%ifndef STAGE2_SECTOR_COUNT
%define STAGE2_SECTOR_COUNT    16
%endif
%ifndef STAGE2_LOAD_SEG
%define STAGE2_LOAD_SEG        0x0800      ; physical 0x8000
%endif
%ifndef STAGE2_LOAD_OFF
%define STAGE2_LOAD_OFF        0x0000
%endif

%ifndef STAGE3_START_SECTOR
%define STAGE3_START_SECTOR    17
%endif
%ifndef STAGE3_SECTOR_COUNT
%define STAGE3_SECTOR_COUNT    16
%endif
%ifndef STAGE3_LOAD_SEG
%define STAGE3_LOAD_SEG        0x0500      ; physical 0x5000
%endif
%ifndef STAGE3_LOAD_OFF
%define STAGE3_LOAD_OFF        0x0000
%endif
%define STAGE3_LOAD_PHYS       (STAGE3_LOAD_SEG * 16 + STAGE3_LOAD_OFF)

%ifndef KERNEL_START_SECTOR
%define KERNEL_START_SECTOR    48
%endif
%ifndef KERNEL_LOAD_SEG
%define KERNEL_LOAD_SEG        0x1000      ; physical 0x10000 (ELF file buffer)
%endif
%ifndef KERNEL_LOAD_OFF
%define KERNEL_LOAD_OFF        0x0000
%endif
%define KERNEL_LOAD_PHYS       (KERNEL_LOAD_SEG * 16 + KERNEL_LOAD_OFF)
%ifndef KERNEL_MAX_SECTORS
%define KERNEL_MAX_SECTORS     512         ; (legacy) old whole-file load cap
%endif
%ifndef KERNEL_MAX_BYTES
%define KERNEL_MAX_BYTES       (KERNEL_MAX_SECTORS * 512)
%endif

; ---------------------------------------------------------------------------
; Streaming kernel loader (stage2) — loads arbitrarily large kernels of any
; size directly to each PT_LOAD segment's physical address, GRUB-style, with
; no whole-file buffer. BIOS INT 13h can only DMA to real-mode-addressable
; low memory (<1 MiB), so segments are read into a low bounce buffer and then
; copied up to their (possibly >1 MiB) destinations via unreal mode (FS flat).
;
;   KERNEL_HDR_BUF      : low buffer holding the ELF header + program headers
;   KERNEL_HDR_SECTORS  : how many leading kernel sectors to slurp for headers
;   KERNEL_BOUNCE_BUF   : low buffer that each segment is streamed through
;   KERNEL_BOUNCE_SECTORS: sectors per INT 13h chunk (<=63 for a single call)
; Both buffers live in free conventional RAM (0x10000..0x27FFF, below 640 KiB)
; and never overlap stage2 (0x8000) / stage3 (0x5000) / boot_info (0x1000).
; ---------------------------------------------------------------------------
%ifndef KERNEL_HDR_BUF
%define KERNEL_HDR_BUF         0x00010000
%endif
%ifndef KERNEL_HDR_SECTORS
%define KERNEL_HDR_SECTORS     32          ; 16 KiB of ELF header + phdr table
%endif
%ifndef KERNEL_BOUNCE_BUF
%define KERNEL_BOUNCE_BUF      0x00020000
%endif
%ifndef KERNEL_BOUNCE_SECTORS
%define KERNEL_BOUNCE_SECTORS  63          ; 31.5 KiB per disk read
%endif

; Optional initrd. Set to 0 to disable. If non-zero, stage2 loads it to
; INITRD_LOAD_PHYS and stage3 registers it as multiboot module 0.
%ifndef FOREB_INITRD_START_SECTOR
%define FOREB_INITRD_START_SECTOR 0
%endif
%ifndef INITRD_LOAD_SEG
%define INITRD_LOAD_SEG        0x4000      ; physical 0x40000
%endif
%ifndef INITRD_LOAD_OFF
%define INITRD_LOAD_OFF        0x0000
%endif
%define INITRD_LOAD_PHYS       (INITRD_LOAD_SEG * 16 + INITRD_LOAD_OFF)
%ifndef INITRD_MAX_SECTORS
%define INITRD_MAX_SECTORS     512
%endif

; GRUB chain-load sector (legacy fallback, 0 = disabled)
%ifndef GRUB_CHAINLOAD_SECTOR
%define GRUB_CHAINLOAD_SECTOR  0
%endif

; -----------------------------------------------------------------------------
; Boot menu tunables
; -----------------------------------------------------------------------------
%ifndef FOREB_DEFAULT_TIMEOUT
%define FOREB_DEFAULT_TIMEOUT  5           ; seconds before auto-boot
%endif
%ifndef FOREB_DEFAULT_ENTRY
%define FOREB_DEFAULT_ENTRY    0           ; 0-based default selection
%endif

; Boot menu entries (indices)
%define ENTRY_DEFAULT          0           ; Forest OS (default, best VBE)
%define ENTRY_NOFB             1           ; Forest OS (text mode, no framebuffer)
%define ENTRY_SAFE             2           ; Forest OS (safe mode)
%define ENTRY_REBOOT           3           ; Reboot
%define BOOT_ENTRY_COUNT       4

; -----------------------------------------------------------------------------
; VBE / video tunables
; -----------------------------------------------------------------------------
; Preferred mode. stage2 enumerates all VBE modes and picks the best match for
; (FOREB_DEFAULT_WIDTH x FOREB_DEFAULT_HEIGHT x FOREB_DEFAULT_BPP), then walks
; the fallback chain below if unavailable, finally falling back to text 03h.
%ifndef FOREB_DEFAULT_WIDTH
%define FOREB_DEFAULT_WIDTH    1920
%endif
%ifndef FOREB_DEFAULT_HEIGHT
%define FOREB_DEFAULT_HEIGHT   1080
%endif
%ifndef FOREB_DEFAULT_BPP
%define FOREB_DEFAULT_BPP      32
%endif

; Text mode fallback (VGA INT 10h mode)
%define VGA_TEXT_MODE          0x0003

; Framebuffer-mode fallback chain. stage2 first tries the preferred
; FOREB_DEFAULT_* resolution; if the BIOS/VBE cannot provide it, it walks down
; to this smaller, near-universal linear mode before finally dropping to text
; 03h. These are additive tunables (a %ifndef override lets the build pick a
; different fallback); they do not change existing mode-selection logic.
%ifndef FOREB_FB_FALLBACK_WIDTH
%define FOREB_FB_FALLBACK_WIDTH   1024
%endif
%ifndef FOREB_FB_FALLBACK_HEIGHT
%define FOREB_FB_FALLBACK_HEIGHT  768
%endif
%ifndef FOREB_FB_FALLBACK_BPP
%define FOREB_FB_FALLBACK_BPP     32
%endif
; Last-resort linear mode before text: 640x480, allowed BPP floor.
%ifndef FOREB_FB_MIN_WIDTH
%define FOREB_FB_MIN_WIDTH        640
%endif
%ifndef FOREB_FB_MIN_HEIGHT
%define FOREB_FB_MIN_HEIGHT       480
%endif
%ifndef FOREB_FB_MIN_BPP
%define FOREB_FB_MIN_BPP          16
%endif

; -----------------------------------------------------------------------------
; Serial debug (COM1) - proof-of-progress banners on the BIOS boot path
; -----------------------------------------------------------------------------
; When FOREB_SERIAL_DEBUG is non-zero, stage2/stage3 emit a short banner over
; COM1 (8N1) so `qemu ... -serial stdio` shows the loader is making progress.
; Default ON; override with -DFOREB_SERIAL_DEBUG=0 to silence.
%ifndef FOREB_SERIAL_DEBUG
%define FOREB_SERIAL_DEBUG     1
%endif
%ifndef FOREB_COM1_BASE
%define FOREB_COM1_BASE        0x3F8       ; COM1 I/O port base
%endif

; -----------------------------------------------------------------------------
; Boot info struct address (ForeB's own rich structure)
; -----------------------------------------------------------------------------
%ifndef BOOT_INFO_ADDRESS
%define BOOT_INFO_ADDRESS      0x00001000  ; foreboots_boot_info
%endif
%ifndef FOREB_MMAP_ADDRESS
%define FOREB_MMAP_ADDRESS     0x00001100  ; foreboots_mmap_entry[FOREB_MMAP_MAX]
%endif
%ifndef MB_MMAP_ADDRESS
%define MB_MMAP_ADDRESS        0x00001400  ; multiboot1 mmap entries
%endif
%ifndef MULTIBOOT_INFO_ADDR
%define MULTIBOOT_INFO_ADDR    0x00001800  ; multiboot_info_t (kernel handoff)
%endif
%ifndef FOREB_MMAP_MAX
%define FOREB_MMAP_MAX         32          ; max E820 entries stored
%endif

; E820 scratch buffer (one entry, 32 bytes) for INT 15h E820 calls
%ifndef E820_SCRATCH
%define E820_SCRATCH           0x00000520
%endif

; -----------------------------------------------------------------------------
; Low-memory buffer layout
; -----------------------------------------------------------------------------
%ifndef DAP_BUF
%define DAP_BUF                0x00000500  ; Disk Address Packet (16 bytes)
%endif
; LBA base offset for El Torito no-emulation CD boot.  Stage1 fills this
; dword with (boot_image_lba * 4) when a valid boot info table is detected,
; 0 for hard disk boot.  Stage2's disk_load adds it to every LBA read.
%ifndef LBA_BASE_OFFSET
%define LBA_BASE_OFFSET        0x00000510  ; dword: 512-byte sector offset (0=HDD)
%endif
; Raw El Torito boot image LBA (2048-byte sectors) as patched by xorriso into
; the boot-info-table.  After stage1's relocation this lives in the stage1
; copy at 0x600 (offset 12); stage2's load_kernel probes it as a fallback read
; base for firmwares that expose the whole CD as the boot drive instead of the
; El Torito boot image.  Zero on hard-disk boot (header unpatched).
%ifndef CD_BOOT_LBA
%define CD_BOOT_LBA            0x0000060C
%endif
; El Torito boot info table field offsets (relative to boot image start)
%define BTI_PVD_LBA            8           ; dword: PVD LBA in 2048-byte sectors
%define BTI_BOOT_IMAGE_LBA     12          ; dword: boot image LBA (2048-byte)
%define ISO9660_PVD_LBA        16          ; PVD is always at sector 16
%ifndef VBEINFO_OFF
%define VBEINFO_OFF            0x00002000  ; VBE controller info (512 bytes)
%endif
%ifndef VBEMODEINFO_OFF
%define VBEMODEINFO_OFF        0x00002200  ; VBE mode info (256 bytes)
%endif
%ifndef PALETTE_BUF
%define PALETTE_BUF            0x00002400  ; DAC palette (768 bytes)
%endif

; Stack for stage1 (grows down from 0x7C00)
%ifndef STACK_TOP
%define STACK_TOP              0x7C00
%endif

; -----------------------------------------------------------------------------
; Menu colors (8bpp palette indices; reprogrammed by stage2)
; -----------------------------------------------------------------------------
%define FOREB_BG               16
%define FOREB_PANEL            17
%define FOREB_BORDER           18
%define FOREB_SELECT           19
%define FOREB_TITLE            20
%define FOREB_TEXT             21
%define FOREB_DIM              22
%define FOREB_TIMER            23
%define FOREB_WHITE            24
%define FOREB_SHADOW           25
%define FOREB_TREE1            26
%define FOREB_TREE2            27
%define FOREB_TREE3            28

; Text-mode colors (VGA attribute, low nibble fg, high nibble bg)
%define TEXT_ATTR_GREEN        0x0A
%define TEXT_ATTR_LGREEN       0x0B
%define TEXT_ATTR_WHITE        0x0F
%define TEXT_ATTR_HIGHLIGHT    0x30        ; black on cyan

; -----------------------------------------------------------------------------
; UI layout (pixels, designed for >= 800x600; clamped at runtime)
; -----------------------------------------------------------------------------
%define MENU_X                 160
%define MENU_Y                 160
%define MENU_W                 480
%define MENU_H                 300
%define TITLE_X                200
%define TITLE_Y                60
%define TIMER_X                340
%define TIMER_Y                480
%define ENTRY_Y_START          240
%define ENTRY_HEIGHT           32
%define LOGO_X                 340
%define LOGO_Y                 80

; -----------------------------------------------------------------------------
; CPU / architecture detection
; -----------------------------------------------------------------------------
; If non-zero, stage3 will set up long mode itself and jump to a 64-bit entry.
; The current Forest OS kernel enters in 32-bit PM and does its own long-mode
; transition (see src/boot64.asm), so this defaults to 0 (disabled).
%ifndef FOREB_FORCE_LONG_MODE
%define FOREB_FORCE_LONG_MODE  0
%endif

; -----------------------------------------------------------------------------
; Magic values
; -----------------------------------------------------------------------------
%define FOREB_BOOT_INFO_MAGIC  0x464F5242  ; "FORB"
%define FOREB_BOOT_INFO_VER    0x00020000  ; v2.0

%define MULTIBOOT1_MAGIC       0x2BADB002  ; passed to kernel in EAX
%define MULTIBOOT1_HEADER_MAGIC 0x1BADB002
%define MULTIBOOT2_MAGIC       0x36d76289

; Multiboot1 info flags
%define MB_FLAG_MEM            0x00000001
%define MB_FLAG_BOOT_DEVICE    0x00000002
%define MB_FLAG_CMDLINE        0x00000004
%define MB_FLAG_MODS           0x00000008
%define MB_FLAG_MMAP           0x00000040
%define MB_FLAG_BOOTLOADER     0x00000200
%define MB_FLAG_FRAMEBUFFER    0x00001000

; E820 memory types
%define E820_USABLE            1
%define E820_RESERVED          2
%define E820_ACPI_RECLAIM      3
%define E820_ACPI_NVS          4
%define E820_BAD               5

; VBE MemoryModel values
%define VBE_MODEL_TEXT         0
%define VBE_MODEL_CGA          1
%define VBE_MODEL_HERCULES     2
%define VBE_MODEL_PLANAR       3
%define VBE_MODEL_PACKED       4
%define VBE_MODEL_DIRECT       6
%define VBE_MODEL_YUV          7

; VBE ModeAttributes bits
%define VBE_ATTR_SUPPORTED     0x0001
%define VBE_ATTR_LFB           0x0080
%define VBE_ATTR_GRAPHICS      0x0010

%define BIOS_VBE_LINEAR        0x4000

; Keyboard scan codes
%define KEY_UP                 0x48
%define KEY_DOWN               0x50
%define KEY_ENTER              0x1C
%define KEY_ESCAPE             0x01

; =============================================================================
; Structure: foreboots_boot_info  (ForeB's rich boot info, at BOOT_INFO_ADDRESS)
; Documented C layout in foreboots/README.md. Must stay in sync.
; =============================================================================
struc foreboots_boot_info
    .magic              resd 1      ; FOREB_BOOT_INFO_MAGIC ("FORB")
    .version            resd 1      ; FOREB_BOOT_INFO_VER
    .flags              resd 1      ; FOREB_BIF_* below
    .boot_disk          resd 1      ; BIOS boot drive number (DL)
    .cmdline            resd 1      ; physical addr of cmdline string
    .boot_loader_name   resd 1      ; physical addr of bootloader name string
    .mem_lower          resd 1      ; KiB of usable memory below 1 MiB
    .mem_upper          resd 1      ; KiB of usable memory above 1 MiB
    .mmap_count         resd 1      ; number of valid entries at mmap_addr
    .mmap_addr          resd 1      ; physical addr of foreboots_mmap_entry[]
    ; Framebuffer (filled when flags & FOREB_BIF_FRAMEBUFFER)
    .framebuffer_addr   resq 1      ; 64-bit physical LFB address (0 if text)
    .framebuffer_pitch  resd 1      ; bytes per scanline
    .framebuffer_width  resd 1
    .framebuffer_height resd 1
    .framebuffer_bpp    resd 1
    .framebuffer_type   resd 1      ; 0=indexed, 1=RGB, 2=EGA text
    .vbe_mode           resw 1      ; VBE mode number set (0 if text)
    .vbe_pad            resw 1
    ; CPU capability detection
    .cpuid_available    resd 1
    .long_mode_available resd 1
    .pae_available      resd 1
    ; Kernel image info (filled by stage2, used by stage3)
    .kernel_load_addr   resd 1      ; physical addr of raw ELF file buffer
    .kernel_size        resd 1      ; bytes loaded
    .kernel_entry       resd 1      ; ELF e_entry (physical, set by stage3)
    .kernel_is64bit     resd 1      ; 1 if ELF64, 0 if ELF32 (set by stage3)
    ; Initrd
    .initrd_addr        resd 1      ; physical addr (0 if none)
    .initrd_size        resd 1
    ; Selected boot entry index + mode flags
    .boot_entry         resd 1      ; ENTRY_* selected in menu
    .no_framebuffer     resd 1      ; 1 if text-mode boot requested
    .safe_mode          resd 1      ; 1 if safe-mode boot requested
    ; Reserved for growth
    .reserved           resd 8
endstruc

; foreboots_boot_info.flags bits
%define FOREB_BIF_MMAP         0x00000001
%define FOREB_BIF_FRAMEBUFFER  0x00000002
%define FOREB_BIF_CMDLINE      0x00000004
%define FOREB_BIF_LONG_MODE    0x00000008
%define FOREB_BIF_CPUID        0x00000010
%define FOREB_BIF_PAE          0x00000020
%define FOREB_BIF_INITRD       0x00000040
%define FOREB_BIF_NO_FB        0x00000080
%define FOREB_BIF_SAFE         0x00000100
; Set by stage2 when it has already streamed every PT_LOAD segment to its
; physical address (arbitrary-size kernel loader). stage3 then skips its own
; in-memory ELF copy and jumps straight to boot_info.kernel_entry.
%define FOREB_BIF_KERNEL_PRELOADED 0x00000200

; =============================================================================
; Structure: foreboots_mmap_entry (ForeB's 64-bit-safe E820 entry, 24 bytes)
; =============================================================================
struc foreboots_mmap_entry
    .base               resq 1      ; 64-bit base address
    .length             resq 1      ; 64-bit length
    .type               resd 1      ; E820 type (1=usable, 2=reserved, ...)
    .acpi               resd 1      ; ACPI 3.0 extended attributes
endstruc

; =============================================================================
; Structure: multiboot1 mmap entry (24 bytes, size field = 20)
; =============================================================================
struc mb_mmap_entry
    .size               resd 1      ; = sizeof(entry) - 4 = 20
    .addr_low           resd 1
    .addr_high          resd 1
    .len_low            resd 1
    .len_high           resd 1
    .type               resd 1
endstruc

; =============================================================================
; Structure: multiboot1 info (passed to kernel in EBX) - matches kernel's
; src/include/multiboot.h multiboot_info_t
; =============================================================================
struc mb_info
    .flags              resd 1
    .mem_lower          resd 1
    .mem_upper          resd 1
    .boot_device        resd 1
    .cmdline            resd 1
    .mods_count         resd 1
    .mods_addr          resd 1
    .syms               resb 16
    .mmap_length        resd 1
    .mmap_addr          resd 1
    .drives_length      resd 1
    .drives_addr        resd 1
    .config_table       resd 1
    .boot_loader_name   resd 1
    .apm_table          resd 1
    .vbe_control_info   resd 1
    .vbe_mode_info      resd 1
    .vbe_mode           resw 1
    .vbe_interface_seg  resw 1
    .vbe_interface_off  resw 1
    .vbe_interface_len  resw 1
    .framebuffer_addr   resq 1      ; 64-bit
    .framebuffer_pitch  resd 1
    .framebuffer_width  resd 1
    .framebuffer_height resd 1
    .framebuffer_bpp    resb 1
    .framebuffer_type   resb 1
    .framebuffer_color_info resw 1
endstruc

; =============================================================================
; Structure: multiboot1 module entry (for initrd)
; =============================================================================
struc mb_module
    .mod_start          resd 1
    .mod_end            resd 1
    .string             resd 1
    .reserved           resd 1
endstruc

; Computed struct sizes (NASM struc does not auto-define a *_size symbol)
%define foreboots_boot_info_size     152
%define foreboots_mmap_entry_size    24
%define mb_mmap_entry_size           24
%define mb_info_size                 112
%define mb_module_size               16

; ELF constants (used by stage3)
%define ELFCLASS32          1
%define ELFCLASS64          2
%define ELFDATA2LSB         1
%define EM_386              3
%define EM_X86_64           62
%define PT_LOAD             1

%endif ; FOREB_CONFIG_H
