# ForeB BIOS Boot

ForeB boots Forest OS on BIOS/CSM firmware through a three-stage pipeline. Each
stage is a self-contained NASM flat binary, assembled with `[BITS 16]` (stages
1-2) or `[BITS 32]` (stage 3). The pipeline starts from the BIOS loading the
MBR and ends with a Multiboot1-compatible handoff to the kernel in 32-bit
protected mode — identical to what GRUB delivers.

```
 BIOS loads MBR     Stage 1 loads stage2     Stage 2 loads kernel + initrd
  0x7C00  ───────>  0x8000  ─────────────>  0x100000 (PT_LOAD targets)
  (512 B)           (8 KiB)                  + builds multiboot_info @ 0x1800
                      │
                      └──> Stage 3 @ 0x5000  (8 KiB) parses ELF, copies segments
                           enters 32-bit PM, jumps to kernel entry
```

## 1. The 3-Stage BIOS Boot Process

| Stage   | Source file     | CPU mode       | Size    | Load address      | Role                                      |
|---------|-----------------|----------------|---------|--------------------|-------------------------------------------|
| Stage 1 | `stage1.asm`    | 16-bit real    | 512 B   | `0x7C00` → `0x600` | MBR: relocate, LBA-probe, load stage2    |
| Stage 2 | `stage2.asm`    | 16-bit real    | 8 KiB   | `0x8000`           | A20, E820, VBE, menu, disk I/O, build multiboot, enter PM |
| Stage 3 | `stage3.asm`    | 32-bit PM      | 8 KiB   | `0x5000`           | ELF parsing, copy PT_LOAD segments, Multiboot1 handoff |

### Disk layout (LBA sectors, 512 bytes each)

| Sectors      | Content                                  |
|--------------|------------------------------------------|
| 0            | Stage 1 (MBR + partition table + `0xAA55`) |
| 1..16        | Stage 2 (8 KiB)                          |
| 17..32       | Stage 3 (8 KiB)                          |
| 48+          | Kernel ELF image                         |
| Past kernel  | Optional initrd / multiboot module       |

All sector offsets are configurable via `config.h` and overridable with NASM
`-D` flags at build time.

---

## 2. Stage 1 — The MBR (512 bytes)

Source: `foreboots/bios/stage1.asm`

Stage 1 is a standard MBR loaded by the BIOS at `0x7C00`. It occupies exactly
512 bytes including the partition table and `0xAA55` boot signature. On El Torito
CD boot (no-emulation), the BIOS pre-loads the entire boot blob (48 sectors) at
`0x7C00`; on HDD/USB, only sector 0 is loaded.

### Execution flow

1. **Normalize CS:IP** — `jmp 0x0000:_normalize_cs` ensures `CS=0`.
2. **Set up segments** — `DS=ES=SS=0`, `SP=STACK_TOP` (0x7C00).
3. **Save boot drive** — `DL` (BIOS boot drive number) is saved to `boot_drive`.
4. **Relocate** — Copy 512 bytes from `0x7C00` to `0x0600` (standard MBR
   relocation to make room for stage2 at `0x7E00` on CD).
5. **Try HDD path** — Build a Disk Address Packet (DAP) at `0x0500`, issue
   `INT 13h AH=42h` (LBA extensions extended read) to load stage2 (16 sectors)
   directly to `0x8000` and stage3 (16 sectors) to `0x5000`. If the read
   succeeds and the stage2 magic at `0x8002` is `0xFEB1`, proceed.
6. **Fall back to CD blob path** — If LBA read failed (e.g. El Torito
   no-emulation where the BIOS pre-loaded the whole blob), copy stage3 from
   `0x9E00` to `0x5000` and stage2 from `0x7E00` to `0x8000` (backward copy
   for overlap safety). Verify stage2 magic.
7. **Jump to stage2** — `jmp 0x0800:0x0000` with `DL` = boot drive.

### MBR layout

```
Offset  Size  Contents
0x0000    3   JMP short + NOP
0x0003   61   Zeros (xorriso patches bytes 8-63 on CD)
0x0040   64   Disk signature (0x464F5245 = "FOREB"), reserved, partition table (4x16B)
0x00BE   16   Partition 1 entry (active, LBA base 63, size 0x100000 sectors)
0x00CE   16   Partition 2 (empty)
0x00DE   16   Partition 3 (empty)
0x00EE   16   Partition 4 (empty)
0x01FE    2   Boot signature 0xAA55
```

### Error handling

If stage2 magic verification fails, `error_flash` is called: it sets text mode
`03h`, then alternates the screen between red and white backgrounds every 0.5
seconds while printing the error message at the center. This loops forever — the
system cannot recover without a valid stage2.

---

## 3. Stage 2 — Extended Loader (8 KiB)

Source: `foreboots/bios/stage2.asm`

Stage 2 runs at physical `0x8000` (`CS=0x0800, ORG=0`). It is the largest and
most complex stage, responsible for hardware detection, memory map collection,
VBE mode selection, the boot menu, kernel loading, and the final protected-mode
transition.

### Segment convention

Throughout stage 2:

| Register | Value   | Purpose                                         |
|----------|---------|-------------------------------------------------|
| `DS`     | `0x0800`| Stage 2 code/data (screen_*, strings, variables)|
| `ES`     | `0x0000`| Low memory (boot_info, mmap, multiboot_info, DAP)|
| `SS`     | `0x7000`| Real-mode stack (SP=`0xFFFE`, physical `0x7FFFE`)|

Functions that need `DS=0` (E820 collection, CPUID, multiboot build) switch DS
temporarily.

### Initialization sequence

The entry point `stage2_main` runs these steps in order:

#### 1. Initialize boot info (`init_boot_info`)

Zeroes the `foreboots_boot_info` struct at `0x1000` and fills in static fields:
magic (`0x464F5242` = "FORB"), version (v2.0), boot disk, kernel load address
(`0x10000`), and bootloader name string.

#### 2. Enable A20 gate (`enable_a20`)

Three methods tried in sequence, with a wrap-test after each:

1. **Fast A20** — Port `0x92`: set bit 1, clear bit 0 (reset toggle).
2. **Keyboard controller** — Port `0x64`/`0x60`: command `0xD1` (write output
   port), set bit 1.
3. **BIOS INT 15h** — `AX=0x2401`.

The wrap test writes `0x55` to `0x0000:0x0500`, then reads `0xFFFF:0x0510`. If
A20 is off, the addresses wrap to the same byte and the values match (ZF=1).

If all three methods fail, a warning is printed but boot continues — the kernel
may still work for low-memory operations.

#### 3. Detect CPU capabilities (`detect_long_mode` macro)

Uses CPUID to detect:
- `cpuid_available` — CPUID is usable (flags bit `FOREB_BIF_CPUID`)
- `pae_available` — PAE is supported (flags bit `FOREB_BIF_PAE`)
- `long_mode_available` — Long mode (64-bit) is supported (flags bit
  `FOREB_BIF_LONG_MODE`)

#### 4. Collect E820 memory map (`e820_collect` macro)

Calls `INT 15h AX=E820h` in a loop, storing up to 32 entries (24 bytes each)
at `FOREB_MMAP_ADDRESS` (`0x1100`). Each entry contains:

```nasm
struc foreboots_mmap_entry
    .base    resq 1    ;  0: 64-bit physical base address
    .length  resq 1    ;  8: 64-bit region length
    .type    resd 1    ; 16: E820 type (1=usable, 2=reserved, 3=ACPI reclaim, ...)
    .acpi    resd 1    ; 20: ACPI 3.0 extended attributes
endstruc               ; 24 bytes total
```

`compute_mem_totals` then sums all usable regions above 1 MiB to produce
`mem_upper` (in KiB). `mem_lower` is fixed at 640.

#### 5. VBE mode selection and graphical menu

Stage 2 attempts to set up a VESA VBE mode for a graphical boot menu:

1. **Enumerate VBE modes** — `INT 10h AX=4F00h` to get the VBE controller
   info, then walk the mode list.
2. **Match preferences** — The menu preference table searches for:
   `800x600x8` → `640x480x8`. Must have a linear framebuffer (LFB) and
   packed/direct color model.
3. **Set mode** — `INT 10h AX=4F02h` with the LFB bit set.
4. **Program DAC palette** — 13 forest-theme colors at DAC registers 16-28.

If VBE is unavailable, the menu falls back to a text-mode display.

The menu renders a forest-themed graphical UI using **16-bit protected-mode
excursions** (`run_in_pm`). LFB stores (`fs:[phys]`) don't work from real or
unreal mode on QEMU, so each repaint briefly enters 16-bit PM with:

| Register | Selector | Base       | Purpose                            |
|----------|----------|------------|-------------------------------------|
| `CS`     | `0x18`   | `0x8000`   | 16-bit code (ORG-relative access)  |
| `DS/ES`  | `0x20`   | `0x8000`   | 16-bit data (variable access)      |
| `FS`     | `0x10`   | `0x0000`   | Flat 4 GiB (LFB at physical addr)  |
| `SS`     | `0x28`   | `0x70000`  | Same physical stack as real mode   |

After drawing, control returns to real mode for keyboard/timer polling via
`INT 16h`/`INT 1Ah`.

The menu supports four boot entries:

| Index | Entry                      | Cmdline  | Video mode           |
|-------|----------------------------|----------|----------------------|
| 0     | Forest OS (default)        | `""`     | 32bpp VBE chain      |
| 1     | Forest OS (no framebuffer) | `nofb`   | VGA text `03h`       |
| 2     | Forest OS (safe mode)      | `safe`   | 32bpp VBE chain      |
| 3     | Reboot                     | —        | `INT 19h`            |

Navigation: **Up/Down** arrows to move, **Enter** to select, **Esc** to reset
the auto-boot countdown. After selection, `launch_boot_entry` applies the
chosen mode: for "no framebuffer" it clears all framebuffer fields and sets
`framebuffer_type=2` (EGA text); for the default and safe entries it
re-selects the kernel's 32bpp VBE mode from the kernel preference chain:

```
1920x1080x32 → 1280x720x32 → 1024x768x32 → 800x600x32 → 640x480x32 → VGA text 03h
```

The loading screen stays visible while the kernel streams in; `vesa_teardown`
restores text mode `03h` and clears framebuffer info **after** the kernel is
loaded but **before** the Multiboot handoff, so the handoff is byte-identical
to the text-mode path.

---

## 4. Stage 3 — ELF Loader (8 KiB)

Source: `foreboots/bios/stage3.asm`

Stage 3 runs at physical `0x5000` in 32-bit protected mode with flat segments
(`DS=ES=SS=0x10`, base 0, limit 4 GiB). Stage 2 enters PM and jumps here with:

- `ESI` = `&foreboots_boot_info` (at `0x1000`)
- `EDI` = `&multiboot_info_t` (at `0x1800`)

### ELF parsing

Stage 3 checks the magic at `kernel_load_addr` (`0x10000`):

```
0x7F 'E' 'L' 'F'  →  check EI_CLASS byte
  ELFCLASS32 (1)  →  elf32 path
  ELFCLASS64 (2)  →  elf64 path
  anything else   →  not_elf (flat binary copy to 0x100000)
```

#### ELF32 path

1. Read `e_phnum` (program header count), `e_phentsize`, `e_phoff`.
2. Walk each program header. For `PT_LOAD` segments:
   - Copy `p_filesz` bytes from `elfbase + p_offset` to `p_paddr`.
   - Zero-fill BSS: `p_memsz - p_filesz` bytes at `p_paddr + p_filesz`.
3. Record `e_entry` as the kernel entry point.

#### ELF64 path

Identical logic using `Elf64_Ehdr`/`Elf64_Phdr` layout (low 32 bits of 64-bit
fields; Forest OS kernel lives below 4 GiB).

#### Flat binary fallback

If the file is not an ELF, stage 3 copies `kernel_size` bytes from
`kernel_load_addr` to `0x100000` and sets entry to `0x100000`.

### Pre-loaded kernel shortcut

If stage 2 already streamed the kernel directly to its PT_LOAD target addresses
(the "arbitrary-size loader" path), `FOREB_BIF_KERNEL_PRELOADED` is set in
`boot_info.flags` and stage 3 skips ELF parsing entirely — it reads
`kernel_entry` from `boot_info` and jumps straight to it.

### Kernel entry (Multiboot1 handoff)

```nasm
mov  al, 0xFF
out  0xA1, al          ; mask all slave PIC IRQs (8..15)
out  0x21, al          ; mask all master PIC IRQs (0..7)
mov  eax, MULTIBOOT1_MAGIC   ; 0x2BADB002
mov  ebx, edi          ; EBX = &multiboot_info_t (0x1800)
jmp  [ebp + foreboots_boot_info.kernel_entry]
```

The kernel is entered in 32-bit protected mode with:
- `EAX` = `0x2BADB002` (Multiboot1 magic)
- `EBX` = physical address of `multiboot_info_t` (`0x1800`)
- `EIP` = kernel ELF entry (e.g. `0x100000`)
- Interrupts off (CLI)
- Paging off
- PICs fully masked

This matches exactly what GRUB delivers, so the kernel cannot distinguish
ForeB from GRUB.

### Optional long-mode trampoline

When `FOREB_FORCE_LONG_MODE=1` is set at build time, and the CPU supports long
mode, and the kernel is ELF64, stage 3 builds identity-mapped page tables for
the low 2 MiB:

```
PML4 @ 0x10000  →  PDPT @ 0x11000  →  PD @ 0x12000  →  2 MiB page @ 0x00000
```

Then enables PAE + EFER.LME + paging, loads a 64-bit GDT, and far-jumps to the
kernel entry in 64-bit mode. This is **off by default** — the current kernel
performs its own long-mode transition after 32-bit PM entry.

---

## 5. Memory Layout During BIOS Boot

### Low memory map

```
Address     Size    Contents
──────────  ──────  ───────────────────────────────────────────────
0x0400      256 B   BIOS IVT (interrupt vector table)
0x0500       32 B   DAP scratch buffer (Disk Address Packet)
0x0520       32 B   E820 scratch entry
0x0600      512 B   Stage 1 relocated MBR
0x1000      152 B   foreboots_boot_info (v2.0, 152 bytes)
0x1100      768 B   ForeB E820 mmap array (32 × 24 bytes)
0x1400      768 B   Multiboot1 mmap array (32 × 24 bytes)
0x1800      112 B   multiboot_info_t (kernel handoff, EBX target)
0x2000      512 B   VBE controller info buffer
0x2200      256 B   VBE mode info buffer
0x2400      768 B   DAC palette buffer (768 bytes)
0x5000     8192 B   Stage 3 (8 KiB)
0x8000     8192 B   Stage 2 (8 KiB)
0xA000     1024 B   Stage 2 header buffer (kernel ELF header parse)
0xC000     4096 B   Stage 2 bounce buffer (kernel streaming)
0x10000   256 KiB   Kernel ELF file buffer (up to KERNEL_MAX_SECTORS × 512)
0x40000       —     Initrd load address (INITRD_LOAD_PHYS)
0x50000       —     Initrd max size boundary
0x70000       —     Stack (SS=0x7000, SP=0xFFFE, physical 0x7FFFE)
```

### High memory targets

```
Address     Contents
──────────  ────────────────────────────────────────────────
0x100000    Kernel PT_LOAD segment destinations (1 MiB+)
            Entry point for the kernel (e.g. 0x100000)
```

The kernel ELF is either:
- **Buffered** at `0x10000` (up to 256 KiB), then stage 3 copies segments to
  their `p_paddr` targets, or
- **Streamed** directly to each `p_paddr` by stage 2's `load_kernel` via
  unreal mode (no size limit — each segment is read in ≤63-sector chunks to a
  bounce buffer at `0xC000` and copied up to the target via `fs:[phys]`).

---

## 6. Disk I/O (INT 13h Extensions)

Stage 1 and stage 2 read disk via INT 13h **LBA extensions** (`AH=42h`). A
Disk Address Packet (DAP) is built at physical `0x0500`:

```nasm
DAP @ 0x0500:
  byte [0x00]  0x10      ; DAP size (16 bytes)
  byte [0x01]  0         ; reserved
  word [0x02]  count     ; sector count
  word [0x04]  offset    ; destination offset
  word [0x06]  segment   ; destination segment
  dword [0x08] lba_low   ; LBA low 32 bits
  dword [0x0C] 0         ; LBA high 32 bits
```

### Read function (`disk_load` / `foreb_read`)

- **Input**: `EAX` = starting LBA, `ECX` = sector count, `ESI` = physical
  destination.
- Reads are chunked to **63 sectors per call** to respect BIOS limits.
- Adds `LBA_BASE_OFFSET` (set by stage 1; 0 for HDD, non-zero for El Torito
  CD) to the LBA before reading.
- Returns `CF=0` on success, `CF=1` on error.

### Unreliable CHS fallback

Stage 2's `load_kernel` uses a dedicated `foreb_read` function that performs
**no CHS fallback** — a direct DAP read only. The CHS fallback in
`lba_read_one` can corrupt the sector count on some BIOSes, so the kernel
loader avoids it. If LBA is not supported, the read fails and `str_kernerr` is
printed.

### Unreal mode for high-memory copies

Kernel segments may be loaded above 1 MiB (e.g. `0x100000`). Since real mode
can only address up to 1 MiB, stage 2 uses **unreal mode**:

1. Briefly enter protected mode (`CR0.PE=1`).
2. Load `FS` with a flat 4 GiB data selector (selector `0x10`).
3. Return to real mode (`CR0.PE=0`). FS descriptor cache retains the 4 GiB
   limit.
4. `mov [fs:edi], al` now reaches any physical address up to 4 GiB.

Stage 2's `load_kernel` disables interrupts (`CLI`) for the entire load
sequence because a real-mode IRQ handler touching FS would wipe the unreal
descriptor cache. After each `foreb_read` call, `foreb_enable_unreal` is
re-invoked to rebuild the FS descriptor (INT 13h can clobber FS).

---

## 7. The Boot Protocol Between ForeB and the Kernel

ForeB implements the **Multiboot1 specification** (GRUB-compatible). The kernel
receives:

```nasm
EAX = 0x2BADB002      ; MULTIBOOT1_MAGIC
EBX = 0x1800          ; &multiboot_info_t
EIP = <e_entry>       ; kernel entry point (e.g. 0x100000)
```

CPU state at entry: 32-bit protected mode, paging off, interrupts off, flat
segments (CS=0x08, DS=ES=FS=GS=SS=0x10), both PICs masked.

### `multiboot_info_t` at `0x1800`

```c
struct multiboot_info {          // 112 bytes, byte-compatible with GRUB
    uint32_t flags;              //  0: MB_FLAG_* bitmask
    uint32_t mem_lower;          //  4: 640 (KiB below 1 MiB)
    uint32_t mem_upper;          //  8: KiB above 1 MiB (from E820)
    uint32_t boot_device;        // 12: 0
    uint32_t cmdline;            // 16: phys ptr to cmdline string
    uint32_t mods_count;         // 20: 1 if initrd else 0
    uint32_t mods_addr;          // 24: phys ptr to mb_module[]
    uint8_t  syms[16];           // 28: zeroed
    uint32_t mmap_length;        // 44: total bytes of mmap array
    uint32_t mmap_addr;          // 48: phys ptr to mb_mmap_entry[] (0x1400)
    uint32_t drives_length;      // 52: 0
    uint32_t drives_addr;        // 56: 0
    uint32_t config_table;       // 60: 0
    uint32_t boot_loader_name;   // 64: phys ptr to "ForeB Forest Bootloader v2.0"
    uint32_t apm_table;          // 68: 0
    uint32_t vbe_control_info;   // 72: 0
    uint32_t vbe_mode_info;      // 76: 0
    uint16_t vbe_mode;           // 80: VBE mode number (0 if text)
    uint16_t vbe_interface_seg;  // 82: 0
    uint16_t vbe_interface_off;  // 84: 0
    uint16_t vbe_interface_len;  // 86: 0
    uint64_t framebuffer_addr;   // 88: 64-bit physical LFB base
    uint32_t framebuffer_pitch;  // 96: bytes per scanline
    uint32_t framebuffer_width;  // 100: width in pixels
    uint32_t framebuffer_height; // 104: height in pixels
    uint8_t  framebuffer_bpp;    // 108: bits per pixel (u8!)
    uint8_t  framebuffer_type;   // 109: 0=indexed, 1=RGB, 2=EGA text
    uint16_t framebuffer_color_info; // 110: RGB field info or 0
};
```

**Important**: `framebuffer_bpp` and `framebuffer_type` are single **bytes**
at offsets 108-109, unlike `foreboots_boot_info` where they are `uint32_t`.

### Multiboot1 flags set by ForeB

| Flag              | Value    | Fields                                        |
|-------------------|----------|-----------------------------------------------|
| `MB_FLAG_MEM`     | `0x001`  | `mem_lower` (640), `mem_upper`               |
| `MB_FLAG_CMDLINE` | `0x004`  | `cmdline` (physical pointer)                  |
| `MB_FLAG_MODS`    | `0x008`  | `mods_count`=1, `mods_addr` (initrd)         |
| `MB_FLAG_MMAP`    | `0x040`  | `mmap_addr`, `mmap_length`                   |
| `MB_FLAG_BOOTLOADER` | `0x200`| `boot_loader_name` (physical pointer)       |
| `MB_FLAG_FRAMEBUFFER`| `0x1000`| `framebuffer_addr` (64-bit), pitch, w, h, bpp, type |

### `foreboots_boot_info` at `0x1000`

ForeB also builds a richer, ForeB-native struct (152 bytes). This is the
bootloader's internal representation and an extension point for future use.

```c
struct foreboots_boot_info {     // 152 bytes
    uint32_t magic;              //  0: 0x464F5242 ("FORB")
    uint32_t version;            //  4: 0x00020000 (v2.0)
    uint32_t flags;              //  8: FOREB_BIF_* bitmask
    uint32_t boot_disk;          // 12: BIOS boot drive (DL)
    uint32_t cmdline;            // 16: physical addr of cmdline
    uint32_t boot_loader_name;   // 20: physical addr of loader name
    uint32_t mem_lower;          // 24: 640 KiB
    uint32_t mem_upper;          // 28: KiB above 1 MiB
    uint32_t mmap_count;         // 32: number of E820 entries
    uint32_t mmap_addr;          // 36: 0x1100
    uint64_t framebuffer_addr;   // 40: 64-bit LFB base
    uint32_t framebuffer_pitch;  // 48
    uint32_t framebuffer_width;  // 52
    uint32_t framebuffer_height; // 56
    uint32_t framebuffer_bpp;    // 60 (u32, not u8)
    uint32_t framebuffer_type;   // 64 (u32, not u8)
    uint16_t vbe_mode;           // 68
    uint16_t vbe_pad;            // 70
    uint32_t cpuid_available;    // 72
    uint32_t long_mode_available;// 76
    uint32_t pae_available;      // 80
    uint32_t kernel_load_addr;   // 84: 0x10000
    uint32_t kernel_size;        // 88
    uint32_t kernel_entry;       // 92: set by stage3
    uint32_t kernel_is64bit;     // 96: 1=ELF64, 0=ELF32
    uint32_t initrd_addr;        // 100
    uint32_t initrd_size;        // 104
    uint32_t boot_entry;         // 108: selected menu entry
    uint32_t no_framebuffer;     // 112
    uint32_t safe_mode;          // 116
    uint32_t reserved[8];        // 120..151
};
```

---

## 8. How the Kernel ELF Is Loaded into Memory

ForeB supports two loading strategies, both achieving the same result:

### Strategy A: Buffered (stage 2 + stage 3)

1. Stage 2 reads the entire kernel ELF into a 256 KiB buffer at `0x10000`.
2. Stage 3 parses the ELF header and program headers at `0x10000`.
3. For each `PT_LOAD` segment, stage 3 copies `p_filesz` bytes from
   `0x10000 + p_offset` to `p_paddr` and zero-fills `p_memsz - p_filesz`.

This is limited to kernels ≤ 256 KiB (the buffer size).

### Strategy B: Streaming (stage 2 only, arbitrary size)

1. Stage 2 reads the ELF header into a small header buffer at `0xA000`.
2. For each `PT_LOAD` segment, stage 2 calls `stream_segment` which:
   - Reads from disk (LBA = `KERNEL_START_SECTOR + file_offset/512`) into a
     4 KiB bounce buffer at `0xC000`.
   - Copies from the bounce buffer to the segment's `p_paddr` (possibly above
     1 MiB) via unreal mode (`fs:[phys]`).
   - Zero-fills BSS (`p_memsz - p_filesz`) via `zero_flat`.
3. Sets `FOREB_BIF_KERNEL_PRELOADED` so stage 3 skips ELF parsing.

This handles kernels of **any size** — there is no 256 KiB limit.

### ELF64 handling

Both strategies use the low 32 bits of 64-bit ELF fields (`p_offset`,
`p_paddr`, `p_filesz`, `p_memsz`, `e_entry`). Forest OS kernels are linked
below 4 GiB, so the upper 32 bits are always zero.

---

## 9. How the Initrd Is Loaded

The initrd is an optional multiboot module (typically a `tar` or `cpio` archive)
loaded from a fixed disk sector.

### Configuration

- `FOREB_INITRD_START_SECTOR` in `config.h` — the disk sector where the initrd
  starts. Defaults to `0` (disabled); the Makefile computes a safe sector just
  past the kernel for `make image`/`make iso` builds.
- `INITRD_LOAD_PHYS` = `0x40000` — physical address where the initrd is loaded.

### Loading (`load_initrd`)

```nasm
%if FOREB_INITRD_START_SECTOR == 0
    ret                          ; no initrd configured
%else
    mov  eax, FOREB_INITRD_START_SECTOR
    mov  ecx, INITRD_MAX_SECTORS
    mov  esi, INITRD_LOAD_PHYS
    call disk_load               ; INT 13h LBA read
    jc   .li_done                ; non-fatal on error
    ; register in boot_info
    mov  [boot_info.initrd_addr], INITRD_LOAD_PHYS
    mov  [boot_info.initrd_size], INITRD_MAX_SECTORS * 512
    or   [boot_info.flags], FOREB_BIF_INITRD
%endif
```

### Multiboot module registration

In `build_multiboot_info`, if `FOREB_BIF_INITRD` is set:

```c
mb_module.mod_start = initrd_addr;
mb_module.mod_end   = initrd_addr + initrd_size;
mb_module.string    = 0;          // no cmdline for BIOS initrd
mb_info.mods_count  = 1;
mb_info.mods_addr   = &mb_module_slot;  // at MB_MMAP_ADDRESS + 32*24
```

The BIOS path supports exactly **one** module. The UEFI path supports multiple
modules via `forebo.cfg`'s `module=`/`module2=` entries.

---

## 10. Protected Mode Setup

### GDT used for the final transition

Stage 2 builds a flat 32-bit GDT (`gdt_pm_desc`) for the jump to stage 3:

| Selector | Base    | Limit     | Access | Description                  |
|----------|---------|-----------|--------|------------------------------|
| `0x00`   | 0       | 0         | —      | Null descriptor              |
| `0x08`   | 0       | 4 GiB     | 0x9A   | 32-bit flat code (D=1, G=1) |
| `0x10`   | 0       | 4 GiB     | 0x92   | 32-bit flat data (D=1, G=1) |
| `0x18`   | 0x8000  | 64 KiB    | 0x9A   | 16-bit code (PM draw)       |
| `0x20`   | 0x8000  | 64 KiB    | 0x92   | 16-bit data (PM draw)       |
| `0x28`   | 0x70000 | 64 KiB    | 0x92   | 16-bit data (stack, base 0x70000) |

### Transition sequence (`enter_pm_and_jump_stage3`)

1. `CLI` — disable interrupts.
2. `LGDT [gdt_pm_desc]` — load the flat GDT.
3. Enable A20 (fast method, port `0x92`).
4. `CR0.PE = 1` — enable protected mode.
5. Far jump to `.pm32` (selector `0x08`): enters 32-bit PM with flat code.
6. Load all segment registers with `0x10` (flat data), `SS:ESP = 0x10:0x90000`.
7. Load `ESI = 0x1000` (boot_info), `EDI = 0x1800` (multiboot_info).
8. Absolute indirect jump to `STAGE3_LOAD_PHYS` (`0x5000`).

Stage 3 then parses the ELF and performs the Multiboot1 handoff to the kernel
as described in section 4.

---

## 11. Error Handling and Fallback Behavior

ForeB follows a **best-effort, non-fatal** error philosophy: the boot process
continues even if individual steps fail, unless the failure is unrecoverable.

### Stage 1 errors

| Condition                  | Behavior                                    |
|----------------------------|---------------------------------------------|
| LBA read fails             | Falls back to CD blob path (El Torito)     |
| Stage 2 magic mismatch     | `error_flash`: flashing red/white screen, beep, message, infinite loop |

### Stage 2 errors

| Condition                  | Behavior                                    |
|----------------------------|---------------------------------------------|
| A20 enable fails           | Warning printed; boot continues             |
| VBE mode not found         | Falls back to text-mode menu                |
| Stage 3 load fails         | Error printed; boot continues (stage 3 may already be in memory from CD blob) |
| Kernel load fails          | Error printed; boot continues (stage 3 will report) |
| Initrd load fails          | Non-fatal; `FOREB_BIF_INITRD` not set, no module registered |
| No kernel loaded           | Text menu shows "WARNING: No kernel loaded!" warning |

### Stage 3 errors

| Condition                  | Behavior                                    |
|----------------------------|---------------------------------------------|
| Not an ELF                 | Treats as flat binary, copies to `0x100000` |
| Kernel already pre-loaded  | Skips ELF parsing, jumps to recorded entry  |
| Long mode unavailable      | Falls back to 32-bit PM handoff (default)   |

### Error display

Stage 1 uses a distinctive **flashing red/white screen** with a centered error
message and PC speaker beep — visible even if the display adapter is unusual.
Stage 2 uses `bios_print` (INT 10h teletype) for error messages. All errors
are also sent to **serial (COM1)** when `FOREB_SERIAL_DEBUG` is enabled.

### Non-fatal philosophy

Most disk-read failures are non-fatal because:
- Stage 3 may already be in memory (CD blob path pre-loads everything).
- The kernel may be partially loaded and still boot.
- The initrd is optional.

Only the stage 2 magic verification in stage 1 is truly fatal — if stage 2 is
corrupted or missing, boot cannot proceed.

---

## Summary

ForeB's BIOS boot pipeline is a carefully sized three-stage system that
delivers a GRUB-compatible Multiboot1 handoff from 512 bytes of MBR code. The
key design decisions:

- **Size discipline**: Each stage stays within its budget (512 B / 8 KiB / 8
  KiB), enforced by build-time size assertions.
- **Dual media support**: The same stages boot from HDD/USB (LBA reads) and
  CD (El Torito blob copy), with automatic fallback.
- **Arbitrary kernel size**: The streaming loader in stage 2 handles kernels of
  any size via unreal mode, breaking the 256 KiB buffer limit.
- **Identical handoff**: Both BIOS and UEFI paths deliver the same machine
  state to the kernel — the kernel cannot tell which firmware booted it.
- **Graceful degradation**: VBE falls back to text, A20 failure is a warning
  not a halt, disk errors are non-fatal where possible.
