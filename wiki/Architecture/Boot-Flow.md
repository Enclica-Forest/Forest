# Forest OS Boot Flow: Power-On to Login Prompt

This is the definitive reference for how Forest OS boots — from the moment you
press the power button to when you see a login prompt. The system supports two
completely independent firmware paths (BIOS and native UEFI) that converge on a
single, byte-for-byte identical kernel handoff.

## Overview: The Big Picture

```
    Power On / Reset
          |
          v
    +-----------+          +-----------+
    | BIOS/CSM  |          |   UEFI    |
    +-----------+          +-----------+
          |                       |
    ForeB Stage1            BOOTX64.EFI
    (512B MBR)              (EFI app)
          |                       |
    ForeB Stage2            efi_main()
    (8 KiB, 16-bit)         (64-bit C)
          |                       |
    ForeB Stage3            ExitBootServices
    (8 KiB, 32-bit)         + long mode teardown
          |                       |
          +-------+   +-----------+
                  |   |
                  v   v
          IDENTICAL KERNEL HANDOFF
          32-bit PM, EAX=0x2BADB002
          EBX=0x1800, EIP=kernel entry
                  |
                  v
           Forest OS Kernel
           (Fern, src/boot64.asm)
                  |
                  v
            startk() -> kmain()
                  |
                  v
            Login / Shell
```

The key insight: **the kernel cannot tell which firmware booted it**. Both
BIOS and UEFI paths produce the exact same CPU state and memory layout at
handoff. This is the "kernel handoff contract" — the invariant that makes the
dual-firmware architecture work.

---

## Part 1: BIOS Boot Path

The BIOS path uses a three-stage NASM bootloader called **ForeB** (Forest
Bootloader). Each stage has a strict size budget and a specific job.

### Stage 1: The MBR (512 bytes)

**File:** `foreboots/bios/stage1.asm`
**Size:** Exactly 512 bytes (one sector)
**Mode:** 16-bit real mode
**Loaded at:** `0x7C00` by BIOS, then relocated to `0x0600`

When BIOS finds a bootable disk (MBR signature `0xAA55` at bytes 510-511), it
loads the first 512 bytes to `0x7C00` and jumps there. Stage 1 does the
following:

1. **Normalize segments** — sets `CS=DS=ES=SS=0`, `SP=STACK_TOP`
2. **Save boot drive** — BIOS passes the drive number in `DL` (e.g., `0x80`
   for first hard disk)
3. **Relocate itself** — copies 512 bytes from `0x7C00` to `0x0600` to free
   up the BIOS load area, then jumps to the relocated code
4. **Try LBA disk read** — uses INT 13h extensions (AH=42h) to read stage2
   from disk sectors 1-16 to physical address `0x8000`
5. **Verify stage2 magic** — checks that word at `[0x8002]` equals `0xFEB1`
6. **Read stage3** — loads sectors 17-32 to physical address `0x5000`
7. **Jump to stage2** — `jmp 0x0800:0x0000`

If this is an El Torito CD boot (no-emulation), the BIOS has already loaded
the full 48-sector blob at `0x7C00`. Stage 1 detects this by checking if the
LBA read fails, then copies stage2 and stage3 from their fixed offsets within
the blob (stage2 at `0x7E00`, stage3 at `0x9E00`).

```
BIOS loads 512B -> 0x7C00
stage1 relocates -> 0x0600
stage1 reads LBA 1-16 -> 0x8000 (stage2)
stage1 reads LBA 17-32 -> 0x5000 (stage3)
stage1 verifies magic 0xFEB1 at 0x8002
stage1 jumps -> 0x0800:0x0000 (stage2)
```

### Stage 2: The Heavy Lifter (8 KiB)

**File:** `foreboots/bios/stage2.asm` | **Mode:** 16-bit real | **Loaded at:** `0x8000`

Stage 2 handles everything from A20 enablement to a graphical boot menu, all
in real mode with brief protected-mode excursions for framebuffer access.

**Initialization sequence:**
1. **Boot info** — zeroes `foreboots_boot_info` at `0x1000`, fills magic/version/drive
2. **A20 gate** — tries fast A20 (port `0x92`) → keyboard controller (port `0x64`/`0x60`) → BIOS INT 15h AX=`0x2401`. Verified via wrap test (`0xFFFF:0x0510` vs `0x0000:0x0500`)
3. **CPU detection** — CPUID checks for long mode (EDX bit 29 of 0x80000001) and PAE
4. **E820 memory map** — INT 15h AX=`E820h` loop, up to 32 entries at `0x1100`
5. **VBE video mode** — queries VBE mode list, sets 8bpp mode (800x600 or 640x480) for the menu, programs a custom DAC palette with Forest's green/brown theme

**The graphical menu** is drawn using **unreal mode** — a trick where you briefly enter protected mode to set up a 4 GB data segment (FS), then return to real mode and write directly to the linear framebuffer at its physical address via `FS:[phys]`. Renders: dark green background, ASCII art tree logo (8x8 ROM font), "Forest OS" title, boot entries, amber countdown timer, and footer. Keyboard input via INT 16h (real mode) between framebuffer writes (protected mode excursions via `run_in_pm`).

**After user selection:**
- Re-selects a 32bpp kernel video mode from the chain: `1920x1080x32 → 1280x720x32 → 1024x768x32 → 800x600x32 → 640x480x32 → VGA text`
- Loads kernel ELF from disk sector 48+ to `0x10000` (chunked to 63 sectors/call)
- Optionally loads initrd from `FOREB_INITRD_START_SECTOR` to `0x40000`
- Tears down VBE, restores text mode, builds `multiboot_info_t` at `0x1800`
- Enters 32-bit protected mode (sets CR0.PE, loads flat GDT, far-jumps to stage 3)

### Stage 3: ELF Loader and Handoff (8 KiB)

**File:** `foreboots/bios/stage3.asm`
**Size:** Up to 8,192 bytes
**Mode:** 32-bit protected mode
**Loaded at:** `0x5000` (physical)

Stage 3 enters in 32-bit protected mode with flat segments. It receives:
- `ESI` = physical address of `foreboots_boot_info`
- `EDI` = physical address of `multiboot_info_t`

What it does:

1. **Parse the kernel ELF** — reads the ELF header at `kernel_load_addr`
   (`0x10000`), checks the magic (`\x7FELF`), determines ELF class (32 or 64)

2. **Copy PT_LOAD segments** — for each program header with `p_type == PT_LOAD`:
   - Copy `p_filesz` bytes from `(elf_base + p_offset)` to `p_paddr`
   - Zero-fill the BSS region (`p_memsz - p_filesz` bytes after the segment)

3. **Record entry point** — `e_entry` from the ELF header

4. **Mask PICs** — writes `0xFF` to both master PIC (`0x21`) and slave PIC
   (`0xA1`) to mask all IRQs. This is **mandatory** — the kernel's early
   entry code enables interrupts with only a page-fault handler installed, so
   a live BIOS timer IRQ would triple-fault before the kernel can set up its
   own IDT.

5. **Jump to kernel** — with the Multiboot1 handoff state:
   ```
   EAX = 0x2BADB002   (MULTIBOOT1_MAGIC)
   EBX = 0x1800       (pointer to multiboot_info_t)
   EIP = kernel entry point (e.g., 0x100000)
   ```

---

## Part 2: UEFI Boot Path

The UEFI path uses a single self-contained EFI application written in C. No
gnu-efi, no libc, no CRT — it defines all EFI structures inline and links
with just `clang` and `ld.lld`.

### Firmware Entry

**File:** `foreboots/uefi/bootx64.c` | **Mode:** 64-bit long mode
**Entry:** `efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE *)`

The UEFI path is a single self-contained EFI application — no gnu-efi, no libc.
It loads `BOOTX64.EFI` from the ESP at `\EFI\BOOT\BOOTX64.EFI`.

### Step-by-Step UEFI Boot

1. **Serial debug** — initializes COM1 (115200 baud). All verbose logging goes to serial since `ConOut` is painfully slow at high resolutions (each newline memmoves the entire screen).

2. **GOP** — locates Graphics Output Protocol, records framebuffer base/dimensions/pitch (always 32bpp RGB, `framebuffer_type = 1`).

3. **Parse forebo.cfg** — reads `\forebo\forebo.cfg` from ESP for menu entries, timeout, theme, icons. Entry types: `forest` (Multiboot), `linux` (EFI-stub), `chainload`, `shell`, `recovery`, `tools`, `setup`, `reboot`.

4. **Load visual assets** — optional background image (BMP/TGA), per-entry icons (32x32 TGA), cursor sprite, panel chrome.

5. **Graphical boot menu** — double-buffered (tear-free), mouse cursor, window manager, animations (fade-in, falling leaves, sliding selection), 8x16 font with 2x scaling, scrolling menu with scrollbar, submenus with breadcrumb navigation.

6. **Load kernel** — reads `\forebo\kernel.elf` from ESP into `AllocatePages` buffer, chunked with animated progress bar. Optional `module=`/`module2=` from config.

7. **Reserve low memory** — allocates pages for `0x1000..0x27FF` (structs) and kernel PT_LOAD destinations before `ExitBootServices`.

8. **Memory map** — `GetMemoryMap` → E820 conversion. `EfiConventional/BootServices*/Loader*` → usable (1), `ACPIReclaim` → 3, else → 2 (reserved).

9. **ELF parse + stage segments** — same logic as BIOS stage 3, but **before** `ExitBootServices` so firmware memory still works.

10. **Build handoff structs** — writes `foreboots_boot_info` at `0x1000`, mmap arrays at `0x1100`/`0x1400`, `multiboot_info_t` at `0x1800`.

11. **ExitBootServices** — firmware releases memory.

12. **Tear down long mode** — loads 32-bit flat GDT, leaves long mode submode, clears `CR0.PG`, `EFER.LME`, `CR4.PAE`, far-jumps to 32-bit CS.

13. **Hand off** — `EAX=0x2BADB002, EBX=0x1800, EIP=kernel entry, PICs masked, interrupts off` — byte-for-byte identical to BIOS.

---

## Part 3: The Multiboot1 Handoff Protocol

The kernel handoff is the heart of the bootloader-kernel interface. Both
BIOS and UEFI produce identical state.

### Register State at Entry

```
CPU Mode:     32-bit protected mode
Paging:       OFF
PAE:          OFF
Interrupts:   OFF (CLI)
GDT:          Flat — CS=0x08 (code, base 0, limit 4 GiB)
              DS=ES=FS=GS=SS=0x10 (data, base 0, limit 4 GiB)
EAX:          0x2BADB002   (MULTIBOOT1_MAGIC)
EBX:          0x00001800   (physical addr of multiboot_info_t)
EIP:          kernel ELF e_entry (e.g., 0x00100000)
PICs:         Both fully masked (IRQs disabled at hardware level)
```

The magic `0x2BADB002` is the "I was loaded by a Multiboot1-compliant
bootloader" signal. It's the Multiboot1 header magic (`0x1BADB002`) with bit
31 set, as specified by the Multiboot1 spec.

### The multiboot_info_t Structure

Located at physical address `0x1800` (112 bytes). The kernel reads it via the
pointer in EBX.

```
Offset  Field               Notes
------  -----               -----
  0     flags               MB_FLAG_* bitmask
  4     mem_lower           KiB usable RAM below 1 MiB (always 640)
  8     mem_upper           KiB usable RAM above 1 MiB
 12     boot_device         BIOS boot drive (0 on UEFI)
 16     cmdline             Physical pointer to NUL-terminated string
 20     mods_count          1 if initrd present, else 0
 24     mods_addr           Physical pointer to mb_module[]
 28     syms[16]            Zeroed (unused)
 44     mmap_length         Total bytes of mmap array
 48     mmap_addr           Physical pointer to mb_mmap_entry[] (at 0x1400)
 64     boot_loader_name    Physical pointer to "ForeB" string
 88     framebuffer_addr    64-bit physical LFB base address
 96     framebuffer_pitch   Bytes per scanline
100     framebuffer_width   Width in pixels
104     framebuffer_height  Height in pixels
108     framebuffer_bpp     Bits per pixel (u8!)
109     framebuffer_type    0=indexed, 1=RGB, 2=EGA text
```

### Flag Bits

| Bit | Flag              | Meaning                                   |
|-----|-------------------|-------------------------------------------|
| 0   | MB_FLAG_MEM       | mem_lower / mem_upper valid               |
| 2   | MB_FLAG_CMDLINE   | cmdline pointer valid                     |
| 3   | MB_FLAG_MODS      | mods_count / mods_addr valid (initrd)     |
| 6   | MB_FLAG_MMAP      | mmap_length / mmap_addr valid             |
| 9   | MB_FLAG_BOOTLOADER| boot_loader_name valid                    |
| 12  | MB_FLAG_FRAMEBUFFER| framebuffer_* fields valid               |

### Memory Map Entries

The mmap array (at `0x1400`) contains `mb_mmap_entry` structs (24 bytes each,
but the `.size` field is 20, so the kernel walks them as `p += size + 4`):

```
Offset  Field       Size
------  -----       ----
  0     size        4 bytes (= 20, always)
  4     addr_low    4 bytes
  8     addr_high   4 bytes (0 for <4 GiB)
 12     len_low     4 bytes
 16     len_high    4 bytes (0 for <4 GiB)
 20     type        4 bytes (1=usable, 2=reserved, 3=ACPI, 4=ACPI NVS)
```

### Module (Initrd) Entry

If an initrd is present, `mb_module` at the address in `mods_addr`:

```
Offset  Field       Size
------  -----       ----
  0     mod_start   4 bytes (physical start address)
  4     mod_end     4 bytes (physical end address)
  8     string      4 bytes (physical pointer to string, may be 0)
 12     reserved    4 bytes (0)
```

---

## Part 4: Kernel Initialization

### Entry: start (boot64.asm)

**File:** `fern/src/boot64.asm`

Even though the kernel is 64-bit, it enters in 32-bit protected mode (just
like GRUB hands off). The file `boot64.asm` is the trampoline that transitions
to long mode.

1. **Verify CPUID** — toggles EFLAGS bit 21 to confirm CPUID support
2. **Verify long mode** — CPUID 0x80000001, checks EDX bit 29
3. **Build page tables** — constructs PML4/PDPT/PD to:
   - **Identity-map** the first 2 MiB (so the current code keeps running)
   - **Map kernel at high half** — `0xFFFFFFFF80100000` → physical `0x100000`
     (8 entries, 2 MiB each = 16 MiB of kernel space)
4. **Enable PAE** (CR4 bit 5)
5. **Set EFER.LME** (long mode enable, MSR 0xC0000080 bit 8)
6. **Optionally enable NX** (EFER bit 11, if CPU supports it)
7. **Enable SMEP/SMAP** (if CPU supports and build options enable)
8. **Enable paging** (CR0.PG) → activates long mode
9. **Load 64-bit GDT** and far-jump to `long_mode_start`

At `long_mode_start` (now in 64-bit mode):
1. Reload segment registers with 64-bit selectors
2. Set RSP to `_stack_top` (high-half kernel stack)
3. Zero the `.bss` section
4. Pass multiboot magic (RDI) and info pointer (RSI) to `startk`

### Entry: startk (kernel.c)

**File:** `fern/src/kernel.c`

`startk()` is the C entry point. It does some early setup (syscall init) and
then calls `kmain(magic, mbi_addr)`.

### kmain: The Boot Sequence

`kmain()` is the kernel's main initialization. Here's the critical path:

```
kmain(magic, mbi_addr)
  |
  +-- clearScreen()                     # Early VGA text output
  +-- hardware_detect_init()            # CPUID detection
  +-- memory_init(magic, mbi_addr)      # E820 → PMM + VMM + page tables
  +-- kernel_finalize_framebuffer_mapping()
  +-- splash_init() + splash_start()   # Boot splash
  |
  +-- bitmap_pmm_init() + finalize()   # Physical memory allocator
  +-- enhanced_heap_init()              # Kernel heap
  +-- cow_init() / swap_init()          # Advanced memory features
  |
  +-- tasks_init()                      # Scheduler
  +-- acpi_init_with_multiboot()        # ACPI tables
  +-- smp_init() + smp_init_arch()      # Multi-CPU startup
  |
  +-- pci_init() + ata_init()           # Hardware enumeration
  +-- ramdisk_init(magic, mbi_addr)     # Parse initrd (tar/FAT)
  +-- vfs_init()                        # Mount root filesystem
  |
  +-- devfs_init()                      # /dev nodes (input, timers, PCI, block)
  +-- ps2_keyboard_init()               # Input drivers
  +-- ps2_mouse_init()
  |
  +-- timer_init(100)                   # 100 Hz scheduler tick
  +-- splash_stop()                     # Boot splash done
  +-- tty_exit_boot_mode()              # Switch to framebuffer TTY
  +-- sti                               # ENABLE INTERRUPTS
  |
  +-- [idle loop: hlt in a circle]
```

### RAM Disk and VFS

**File:** `fern/src/ramdisk.c`

The `ramdisk_init()` function locates the initrd module from the multiboot
info (`mods_count` / `mods_addr`). It supports:
- **tar archives** (ustar format) — the primary format
- **FAT12/FAT16/FAT32 filesystems** — for compatibility

The tar parser walks 512-byte blocks, reading octal size fields from ustar
headers, and populates an in-memory filesystem tree.

`vfs_init()` then mounts this as the root filesystem (`/`), making files
like `/bin/sh`, `/etc/passwd`, etc. available to the kernel.

### Session Management

**File:** `fern/src/session.c`

In the full build (not kernel-only), `session_run(autologin_root)` is called
to start the login/session manager. It:

1. Initializes authentication (`auth_init()`)
2. Sets up multi-TTY sessions (TTY 1 = GUI, TTY 2-9 = text)
3. Enters a loop that:
   - Calls `run_session_login()` for the current TTY
   - Handles TTY switching (Ctrl+Alt+F1-F12)
   - Manages the graphical desktop session

For the **kernel-only build** (current default for Fern), `session_run()` is
not called. Instead, `kmain()` enters an idle loop:

```c
for (;;) {
    __asm__ __volatile__("hlt");
}
```

The CPU halts until the next interrupt (timer, keyboard), then halts again.
This keeps the kernel responsive to input without launching userspace.

---

## Part 5: Memory Layout During Boot

### BIOS Path: Low Memory Map

```
Address     Contents
---------   --------
0x00000     Real-mode IVT (Interrupt Vector Table)
0x00500     DAP scratch space (Disk Address Packet)
0x00520     E820 scratch entry
0x00600     Stage 1 (relocated MBR, 512 B)
0x01000     foreboots_boot_info (152 B)
0x01100     ForeB E820 mmap array (32 x 24 B)
0x01400     Multiboot1 mmap array (32 x 24 B)
0x01800     multiboot_info_t (112 B) ← EBX points here
0x02000     VBE controller info (512 B)
0x02200     VBE mode info (256 B)
0x02400     DAC palette buffer (768 B)
0x05000     Stage 3 (8 KiB)
0x08000     Stage 2 (8 KiB)
0x10000     Kernel ELF file buffer (up to 256 KiB)
0x40000     Initrd load address
0x70000     Real-mode stack (SS=0x7000, SP=0xFFFE)
```

### BIOS Path: High Memory

```
0x100000    Kernel PT_LOAD segments (copied from ELF)
            The kernel lives here physically.
            Linked at VMA 0xFFFFFFFF80100000 (high half).
```

### UEFI Path: Memory Layout

The UEFI loader builds the **same layout** at the **same addresses**.
The structures at `0x1000`-`0x27FF` are reserved via `AllocatePages` before
`ExitBootServices`. The kernel is loaded to `0x100000` (or wherever its
PT_LOAD segments specify).

After `ExitBootServices`, the loader owns all memory. It writes the structs
at their fixed addresses, tears down long mode, and jumps to the kernel —
producing an identical memory layout to the BIOS path.

### Kernel Virtual Memory (after boot64.asm paging)

```
Virtual Address              Physical Address     Size
-------------------------   -----------------    ----
0xFFFFFFFF80000000+          0x100000+            16 MiB (kernel code/data)
0xFFFF800000000000+          (identity mapped)    varies
0x0000000000000000+          0x00000000+          2 MiB (identity map, early boot)
```

The kernel is linked at `0xFFFFFFFF80100000` but loaded at physical
`0x100000`. The page tables set up in `boot64.asm` map the first 16 MiB of
physical memory (at `0x100000`) to the high-half virtual addresses starting
at `0xFFFFFFFF80100000`. This is the standard "higher half" kernel design.

---

## Part 6: How ForeB Loads the Kernel ELF

Both BIOS stage 3 and the UEFI loader perform the same ELF loading:

1. **Read ELF header** at `kernel_load_addr` (`0x10000` for BIOS, or the
   `AllocatePages` buffer for UEFI)

2. **Verify magic** — `\x7F` `E` `L` `F` (bytes 0-3)

3. **Determine class** — byte at offset 4: `1` = ELF32, `2` = ELF64

4. **Walk program headers** — starting at `e_phoff`, iterate `e_phnum` entries
   of size `e_phentsize`

5. **For each PT_LOAD segment:**
   ```
   source = elf_base + p_offset
   dest   = p_paddr          (physical address in the ELF)
   copy   = p_filesz bytes
   zero   = p_memsz - p_filesz bytes  (BSS region)
   ```

6. **Record entry point** — `e_entry` (low 32 bits for ELF64, full 32 bits
   for ELF32)

The kernel is an ELF linked at `0xFFFFFFFF80100000` (high half), but its
PT_LOAD segments specify `p_paddr` values in the low 4 GiB (e.g.,
`0x100000`). ForeB copies them to those physical addresses. The kernel's own
page tables (set up in `boot64.asm`) then map them to the high virtual
addresses.

---

## Part 7: Dual-Firmware Comparison

| Aspect              | BIOS Path                         | UEFI Path                         |
|---------------------|-----------------------------------|-----------------------------------|
| **Loader**          | Stage1/2/3 (NASM assembly)        | `bootx64.c` (freestanding C)      |
| **Firmware mode**   | 16-bit real mode                  | 64-bit long mode                  |
| **Framebuffer**     | VBE (INT 10h, 8bpp menu → 32bpp) | GOP (always 32bpp RGB)            |
| **Memory map**      | INT 15h E820                      | `GetMemoryMap` → E820 conversion  |
| **Kernel source**   | Raw disk sectors (LBA 48+)        | ESP FAT file `\forebo\kernel.elf` |
| **Pre-handoff CPU** | Already 32-bit PM                 | Must tear down long mode → 32 PM  |
| **Kernel entry**    | 32-bit PM, `EAX=0x2BADB002`      | **Identical**                     |
| **Boot info**       | `foreboots_boot_info` at `0x1000` | **Identical**                     |
| **Menu features**   | 8x8 font, 8bpp palette            | 8x16 font, 32bpp, mouse, WM, anim |
| **Config**          | Compile-time `config.h`           | Runtime `forebo.cfg` on ESP       |
| **Initrd**          | One module from disk sectors      | Multiple modules from ESP files   |

---

## Part 8: Hybrid ISO: One Image, Both Firmwares

A single `forebo.iso` can boot on both BIOS and UEFI machines:

```
                    forebo.iso (ISO-9660 + El Torito)
                              |
          +-------------------+-------------------+
          |                                       |
   BIOS / CSM firmware                    UEFI firmware
          |                                       |
   El Torito no-emulation               EFI boot entry
   catalog → stage1.bin                 → ESP (FAT)
          |                              → \EFI\BOOT\BOOTX64.EFI
   stage1 → stage2 → stage3                     |
          |                              bootx64.c:
   kernel ELF from ISO sectors         kernel.elf from ESP
          |                                       |
          +-------------------+-------------------+
                              |
                  IDENTICAL KERNEL HANDOFF
                  EAX=0x2BADB002, EBX=0x1800
```

The ISO embeds both a BIOS El Torito boot image and a FAT EFI System
Partition. The same kernel ELF is reachable by both paths: BIOS reads it
by LBA, UEFI reads it as a file.

---

## Summary: The Complete Boot Timeline

```
Power On
  |
  v
[BIOS POST / UEFI firmware init]
  |
  v
+--[BIOS]--+                    +--[UEFI]--+
| INT 19h  |                    | BDS phase|
| Load MBR |                    | Find ESP |
| to 0x7C00|                    | Load EFI |
+----+-----+                    +----+-----+
     |                                |
     v                                v
  Stage 1                      efi_main()
  (512B MBR)                   (64-bit C)
     |                                |
     v                                v
  Stage 2                      GOP + Config
  (A20,E820,                   + Assets
   VBE menu,                    + Menu
   disk I/O)                    + Load kernel
     |                                |
     v                                v
  Stage 3                      ExitBootServices
  (ELF parse,                  + Teardown long mode
   PT_LOAD copy)               + PT_LOAD copy
     |                                |
     +-------+      +----------------+
             |      |
             v      v
      KERNEL HANDOFF (32-bit PM)
      EAX=0x2BADB002
      EBX=0x1800
      EIP=kernel entry
             |
             v
      boot64.asm trampoline
      (CPUID check, page tables,
       long mode enable, GDT,
       far jump to 64-bit)
             |
             v
      startk() → kmain()
      (memory, drivers, VFS,
       scheduler, devices)
             |
             v
      splash screen → idle loop
      (or session_run → login)
```

---

## Source File Reference

| File                                    | Role                                       |
|-----------------------------------------|--------------------------------------------|
| `foreboots/bios/stage1.asm`             | MBR: relocate, LBA probe, load stage2/3    |
| `foreboots/bios/stage2.asm`             | A20, E820, VBE menu, disk I/O, PM entry   |
| `foreboots/bios/stage3.asm`             | 32-bit PM ELF loader, Multiboot1 handoff   |
| `foreboots/uefi/bootx64.c`              | Self-contained EFI app, GOP, ESP load, handoff |
| `foreboots/include/boot_protocol.h`     | C mirror of the handoff ABI (structs + asserts) |
| `foreboots/include/boot_protocol.inc`   | NASM mirror of the handoff ABI             |
| `foreboots/ARCHITECTURE.md`             | Architectural overview                     |
| `foreboots/README.md`                   | Full documentation of BIOS + UEFI paths    |
| `fern/src/boot64.asm`                   | 32→64-bit kernel trampoline (page tables)  |
| `fern/src/kernel.c`                     | `startk()` → `kmain()` — kernel init      |
| `fern/src/ramdisk.c`                    | tar/FAT initrd parser                      |
| `fern/src/session.c`                    | Login/session manager (multi-TTY)          |
