# ForeB Architecture — Two-Firmware Boot (BIOS + native UEFI)

ForeB boots the 64-bit **Forest OS** kernel from either legacy **BIOS/CSM** or
native **UEFI (x86-64)** firmware. Both firmware paths converge on a single,
**byte-for-byte identical kernel handoff**: they hand control to the kernel's
ELF entry point in **32-bit protected mode** with the exact same registers and
memory structures. The kernel does not know or care which firmware booted it.

- **The kernel handoff contract is the invariant.** Everything else — how the
  menu is drawn, how the framebuffer is obtained, how the memory map is built,
  how the ELF is loaded — differs between the two firmwares, but the final CPU
  state and the `multiboot_info_t` at `0x1800` are identical.
- **Single source of truth for the ABI**: `config.h` (NASM) and
  `include/boot_protocol.h` (C) define the same magic values, flag bits, fixed
  physical addresses and struct offsets. `include/boot_protocol.inc` bridges the
  two so BIOS assembly and the UEFI C loader can never drift apart.

---

## The invariant: kernel handoff contract

Both paths jump to the kernel with **exactly** this state:

```
CPU mode        32-bit protected mode, paging OFF, PAE OFF, interrupts OFF (CLI)
GDT             flat: CS=0x08 (code, base 0 / limit 4 GiB)
                      DS=ES=FS=GS=SS=0x10 (data, base 0 / limit 4 GiB)
EAX             0x2BADB002   (MULTIBOOT1_MAGIC)
EBX             0x00001800   (physical addr of multiboot_info_t)
EIP             kernel ELF e_entry   (e.g. 0x00100000)
PICs            both fully masked:  out 0x21,0xFF   out 0xA1,0xFF   (MANDATORY)
Stack           loader-provided small valid SS:ESP in flat data (kernel resets)
```

The 64-bit Forest kernel performs its **own** long-mode transition after entry
(`src/boot64.asm`). Neither loader may enter the kernel in long mode — the UEFI
loader, which itself runs in long mode, MUST tear long mode back down to 32-bit
PM before the jump.

### Structures the kernel reads (physical addresses, identity-mapped low RAM)

| Addr     | Structure                       | Size   | Notes                                   |
|----------|---------------------------------|--------|-----------------------------------------|
| `0x1000` | `foreboots_boot_info`           | 152 B  | ForeB-native rich info (forward-looking)|
| `0x1100` | `foreboots_mmap_entry[32]`      | 24 B/e | 64-bit-safe E820 array                  |
| `0x1400` | `mb_mmap_entry[…]`              | 24 B/e | multiboot mmap (`.size=20`, walk `+size+4`)|
| `0x1800` | `multiboot_info_t`  **(EBX)**   | 112 B  | the one the kernel consumes today       |

Only `multiboot_info_t` (via EBX) is strictly required today; the rest is the
ForeB layout reproduced for byte-for-byte parity. See `include/boot_protocol.h`
for the annotated field offsets and compile-time `_Static_assert`s.

---

## Path A — BIOS / CSM (existing, unchanged)

Real-mode NASM staged loader. Contract preserved exactly as it always was.

```
stage1.asm  → stage1.bin   512 B MBR. Relocates to 0x0600, loads stage2 via
                           INT 13h (LBA), handles El Torito no-emulation CD.
stage2.asm  → stage2.bin   8 KiB. A20, INT 15h E820 memory map, VBE mode set,
                           graphical boot menu, disk I/O, streams kernel PT_LOADs.
stage3.asm  → stage3.bin   8 KiB. Enters 32-bit PM, parses ELF (if not preloaded),
                           builds multiboot_info_t @ 0x1800, masks PICs, jumps.
```

Firmware services used: **INT 13h** (disk), **INT 15h E820** (memory),
**INT 10h / VBE** (video). Config and struct layouts come from `config.h`.

---

## Path B — Native UEFI (new)

A single self-contained EFI application: `uefi/bootx64.c` → `BOOTX64.EFI`.
No gnu-efi, no libc/CRT — EFI structs are defined inline. It reproduces
everything the three BIOS stages do, using UEFI Boot Services instead of BIOS
interrupts, and then performs the **identical** 32-bit PM handoff.

```
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
  │
  ├─ 1. Graphics Output Protocol (GOP)
  │       LocateProtocol(gEfiGraphicsOutputProtocolGuid)
  │       framebuffer_addr = Mode->FrameBufferBase
  │       width/height     = Info->HorizontalResolution / VerticalResolution
  │       pitch            = Info->PixelsPerScanLine * 4
  │       bpp = 32, framebuffer_type = 1 (RGB)   ← replaces BIOS VBE
  │
  ├─ 2. Simple File System — load kernel ELF from the ESP
  │       LocateHandleBuffer(SimpleFileSystem) → OpenVolume → root
  │       Open("\\forebo\\kernel.elf") → read whole file into an
  │       AllocatePages buffer (identity-mapped low/high RAM).   ← replaces INT 13h
  │       (Optional: "\\forebo\\initrd.img" → mb_module.)
  │
  ├─ 3. GetMemoryMap → E820-equivalent
  │       Build foreboots_mmap_entry[] and mb_mmap_entry[] from the EFI map.
  │       EfiConventional/BootServices{Code,Data}/Loader{Code,Data} → 1 (usable)
  │       ACPIReclaim → 3   ACPIMemoryNVS → 4   everything else → 2 (reserved)
  │       mem_lower = 640;  mem_upper = Σ(usable > 1 MiB) in KiB.   ← replaces E820
  │
  ├─ 4. AllocatePages to guarantee the fixed contract pages are usable RAM
  │       0x1000..0x27FF (structs) and the kernel PT_LOAD dests (e.g. 0x100000)
  │       before firmware stops owning memory.
  │
  ├─ 5. Parse ELF + stage PT_LOAD segments (does stage3's job)
  │       For each PT_LOAD: copy p_filesz from (elf+p_offset) → p_paddr,
  │       then zero (p_memsz - p_filesz). entry = e_entry (low 32 bits).
  │
  ├─ 6. Write the structs at their fixed physical addresses
  │       foreboots_boot_info @ 0x1000, foreboots_mmap @ 0x1100,
  │       mb_mmap @ 0x1400, multiboot_info_t @ 0x1800, cmdline/name strings.
  │       cmdline: entry 0 = "", 1 = "nofb", 2 = "safe".  name = "ForeB".
  │
  ├─ 7. ExitBootServices(ImageHandle, MapKey)      ← firmware releases memory
  │       (re-GetMemoryMap to obtain a fresh MapKey immediately before this).
  │
  └─ 8. Tear down long mode → 32-bit PM → jump (the shared handoff)
          load a 32-bit flat GDT (null / 0x08 code / 0x10 data),
          leave compatibility submode, CR0.PG=0, EFER.LME=0, CR4.PAE=0,
          far-jump into 0x08, set DS=ES=FS=GS=SS=0x10,
          EAX=0x2BADB002, EBX=0x1800, mask both PICs, jmp e_entry.
```

### BIOS vs UEFI, service by service

| Concern           | BIOS path                    | UEFI path                              |
|-------------------|------------------------------|----------------------------------------|
| Disk / kernel load| INT 13h LBA reads            | SimpleFileSystem `\forebo\kernel.elf`  |
| Memory map        | INT 15h E820                 | `GetMemoryMap` → E820 conversion       |
| Framebuffer       | VBE (INT 10h)                | Graphics Output Protocol (GOP)         |
| Video type        | 8bpp→0, ≥16bpp→1, nofb→2      | GOP always RGB → 1                      |
| Entry CPU mode    | already 16→32-bit PM         | long mode → **torn down** to 32-bit PM |
| ABI source        | `config.h` / `.inc`          | `boot_protocol.h` (same values)        |

---

## Unified directory layout

```
foreboots/
├── config.h                  NASM: authoritative ABI (magics, addrs, strucs)
├── include/
│   ├── boot_protocol.h        C mirror of the ABI (packed structs + _Static_assert)
│   └── boot_protocol.inc      NASM bridge: %includes config.h, adds UEFI aliases
│                              + GDT descriptors; self-sufficient standalone too
├── forebo.h / forebo64.h      existing kernel-facing / 64-bit helper headers
├── stage1.asm  stage2.asm  stage3.asm      BIOS staged loader (Path A)
├── uefi/
│   └── bootx64.c  → BOOTX64.EFI            native UEFI loader (Path B)
├── Makefile                   builds both paths + the hybrid ISO
├── ARCHITECTURE.md            this document
└── forebo.iso / forebo.img    output images
```

### Build commands

BIOS stages: NASM `-f bin` per `Makefile`.

UEFI app (self-contained, no gnu-efi):
```
clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
      -mno-red-zone -mno-mmx -mno-sse -Wall -Wextra -std=c11 \
      -c uefi/bootx64.c -o uefi/bootx64.o
ld.lld -flavor link -subsystem:efi_application -entry:efi_main \
      -out:BOOTX64.EFI uefi/bootx64.o
```

---

## Hybrid-ISO boot flow (one image boots both firmwares)

A single `forebo.iso` carries **both** a BIOS El Torito boot image and an EFI
System Partition (ESP), so the same media boots on legacy and UEFI machines.

```
                        forebo.iso  (ISO-9660 + El Torito)
                                     │
              ┌──────────────────────┴───────────────────────┐
   legacy BIOS / CSM firmware                      UEFI firmware (x86-64)
              │                                               │
   El Torito no-emulation catalog                 EFI boot entry → ESP (FAT)
   boot image = stage1 (MBR)                       /EFI/BOOT/BOOTX64.EFI
              │                                               │
   stage1 → stage2 (menu, VBE, E820)              bootx64.c:
              │  loads kernel ELF from ISO          GOP + GetMemoryMap +
   stage2 → stage3 (32-bit PM,                      SimpleFileSystem load
              │  ELF parse, mb_info)                \forebo\kernel.elf,
              │                                      ExitBootServices,
              │                                      tear down long mode
              └───────────────┬───────────────────────────────┘
                              ▼
        IDENTICAL 32-bit PM handoff to kernel e_entry
        EAX=0x2BADB002  EBX=0x1800 (multiboot_info_t)  PICs masked
                              │
                              ▼
                 Forest OS kernel (does its own long-mode switch)
```

ISO construction (via `xorriso`):
- **BIOS**: El Torito no-emulation boot image = `stage1.bin` (+ following
  sectors for stage2/3/kernel), boot info table patched so stage1 finds its LBA.
- **UEFI**: an embedded FAT ESP image containing
  `/EFI/BOOT/BOOTX64.EFI` and `/forebo/kernel.elf` (+ optional `/forebo/initrd.img`),
  registered as the El Torito EFI boot image (`-eltorito-alt-boot -e … -no-emul-boot`).

The kernel ELF is reachable by **both** paths: the BIOS stages read it from the
ISO by LBA, while the UEFI loader reads it as `\forebo\kernel.elf` from the ESP.

---

## Why the ABI files exist

| File                       | Consumers            | Role                                             |
|----------------------------|----------------------|--------------------------------------------------|
| `config.h`                 | stage1/2/3.asm       | Authoritative NASM definitions (unchanged)       |
| `include/boot_protocol.inc`| BIOS asm (+ tools)   | `%include`s config.h; guards every symbol `%ifndef`; adds UEFI aliases + handoff GDT descriptors; also works standalone (`-DFOREB_NO_AUTO_CONFIG_INCLUDE`) |
| `include/boot_protocol.h`  | `uefi/bootx64.c`     | C mirror; `__attribute__((packed))` structs with `_Static_assert` on `sizeof` and key offsets so a mismatch fails the build |

Because all three derive from the same numbers and offsets, the BIOS and UEFI
loaders are guaranteed to produce a `multiboot_info_t` the kernel reads
identically — which is exactly what makes the two-firmware architecture safe.
