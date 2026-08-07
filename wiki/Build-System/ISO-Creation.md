# ISO and Disk Image Creation

This page explains how Forest OS builds bootable ISO and disk images. The entire
boot path runs through **ForeB** (the Forest Bootloader) -- GRUB has been
completely removed. ForeB handles both BIOS (legacy MBR) and UEFI boot via a
single unified build system.

## Quick Reference

| Command | Output | Description |
|---------|--------|-------------|
| `make iso` | `forebo.iso` | Hybrid ISO (BIOS + UEFI) |
| `make img` | `forebo.img` | Raw disk image (BIOS) |
| `make esp` | `esp.img` | FAT EFI System Partition |
| `make iso-bios` | `forebo.iso` | BIOS-only El Torito ISO |

## 1. ISO vs IMG Formats

**ISO 9660 (`.iso`)** is the standard optical disc format. Forest OS ISOs are
"hybrid" -- they boot on both BIOS and UEFI machines. The ISO contains:

- An El Torito BIOS boot image (ForeB stage1 + stage2 + stage3 + kernel)
- An El Torito UEFI boot image (a FAT ESP with `BOOTX64.EFI` + kernel + config)
- Optional GPT marking for USB boot via `-isohybrid-gpt-basdat`

**Raw disk image (`.img`)** is a flat byte-for-byte copy of what goes on a
disk. The ForeB disk image layout is:

```
Sector 0       : Stage 1 (MBR, 512 bytes)
Sectors 1-16   : Stage 2 (8 KiB, real-mode GUI/BIOS menu)
Sectors 17-32  : Stage 3 (8 KiB, protected-mode ELF loader)
Sector 48+     : Kernel ELF (raw file)
Sector 560+    : Optional initrd (multiboot module)
```

The image is 10 MiB (`IMAGE_SIZE_SECTORS = 20480`) by default.

**When to use which:**

- Use **ISO** for distribution (download, burn to CD, write to USB with Ventoy)
- Use **IMG** for direct `dd` to a USB drive or for QEMU disk testing
- Use **ESP** when you need just the UEFI partition (chainloading, custom setups)

## 2. BIOS Bootable ISO Creation

ForeB's BIOS boot uses a three-stage chain:

1. **Stage 1** (`bios/stage1.asm`) -- MBR, 512 bytes. Loads stage2 from LBA 1.
2. **Stage 2** (`bios/stage2.asm`) -- GUI menu, VBE video setup, kernel streaming loader.
3. **Stage 3** (`bios/stage3.asm`) -- Protected-mode ELF parser, multiboot handoff to kernel.

The BIOS ISO creation process (`make iso-bios`) works like this:

```bash
# Assemble ForeB stages (if not already built)
make -C ../foreboots all

# Create the boot blob: concatenate stage1 + stage2 + stage3
cat build/stage1.bin build/stage2.bin build/stage3.bin > /tmp/forebo_boot.img

# Pad to kernel sector offset (sector 48 = 24576 bytes)
BLOBSIZE=$(wc -c < /tmp/forebo_boot.img)
NEED=$(( 48 * 512 ))
if [ "$BLOBSIZE" -lt "$NEED" ]; then
    dd if=/dev/zero bs=1 count=$(( NEED - BLOBSIZE )) >> /tmp/forebo_boot.img
fi

# Append the kernel at sector 48
cat build/32bit-bios-debug/boot/kernel.bin >> /tmp/forebo_boot.img

# Build the ISO with xorriso
xorriso -as mkisofs \
    -b boot/forebo/forebo.img \
    -no-emul-boot \
    -boot-load-size 48 \
    -boot-info-table \
    -o forebo.iso \
    /tmp/forebo_iso_root
```

Key flags:

- `-b` -- path to the El Torito BIOS boot image (relative to ISO root)
- `-no-emul-boot` -- no floppy/hard-disk emulation; the BIOS loads the image
  directly and ForeB reads the real CD/DVD via INT 13h extensions
- `-boot-load-size 48` -- load 48 sectors (24 KiB) into memory; ForeB's LBA
  base offset correction handles the rest

## 3. UEFI Bootable ISO Creation

UEFI boot requires an **EFI System Partition** (ESP) -- a FAT16 image containing
`/EFI/BOOT/BOOTX64.EFI`. ForeB builds this with `make esp`:

```bash
# Create a 48 MiB FAT16 image
mkfs.fat -C -F 16 -n FOREB esp.img 49152

# Create directory structure
mmd -i esp.img ::/EFI ::/EFI/BOOT ::/forebo ::/forebo/icons

# Copy the UEFI application
mcopy -i esp.img build/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI

# Copy kernel, config, and assets
mcopy -i esp.img build/32bit-bios-debug/boot/kernel.elf ::/forebo/kernel.elf
mcopy -i esp.img forebo.cfg ::/forebo/forebo.cfg
mcopy -i esp.img assets/bg.bmp ::/forebo/bg.bmp
mcopy -i esp.img assets/icons/*.tga ::/forebo/icons/
mcopy -i esp.img assets/initrd.tar ::/forebo/initrd.tar
```

The ESP is then embedded in the ISO as a **second El Torito boot entry** with
the EFI platform ID, so UEFI firmware loads `BOOTX64.EFI` from it.

The UEFI loader (`uefi/bootx64.c`) is compiled with clang as a freestanding
PE/COFF application:

```bash
clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
    -mno-red-zone -mno-mmx -mno-sse -mno-stack-arg-probe \
    -Wall -Wextra -std=c11 -Iinclude -Iuefi \
    -c uefi/bootx64.c -o build/uefi/bootx64.o

ld.lld -flavor link -subsystem:efi_application -entry:efi_main \
    -out:build/BOOTX64.EFI build/uefi/*.o
```

## 4. Hybrid ISO (BIOS + UEFI)

A hybrid ISO boots on both legacy BIOS and modern UEFI machines. ForeB creates
one with `make iso` (which calls `make iso-hybrid`):

```bash
xorriso -as mkisofs \
    -V FOREB \
    -b boot/forebo/forebo.img \          # BIOS El Torito image
    -no-emul-boot \
    -boot-load-size 48 \
    -boot-info-table \
    -eltorito-alt-boot \                  # second boot catalog entry
    -e boot/efi/esp.img \                # UEFI El Torito image (ESP)
    -no-emul-boot \
    -isohybrid-gpt-basdat \              # GPT ESP marking for USB boot
    -o build/forebo.iso \
    /tmp/forebo_iso_root
```

How it works:

1. The BIOS boot entry points to `forebo.img` (the concatenated stage1+2+3+kernel blob)
2. `-eltorito-alt-boot` starts a second boot catalog entry
3. The UEFI entry points to `esp.img` (the FAT16 EFI System Partition)
4. `-isohybrid-gpt-basdat` marks the ESP as an EFI System Partition in a
   protective GPT, enabling USB boot on real UEFI hardware

The `ISOHYBRID_FLAG` variable auto-detects whether xorriso supports
`-isohybrid-gpt-basdat` and skips it gracefully if not.

## 5. The xorriso-Based ISO Creation Process

Forest OS uses **xorriso** (not mkisofs/genisoimage) for ISO creation. The
`-as mkisofs` flag provides backward-compatible mkisofs command-line syntax.

### Step-by-step (hybrid ISO)

```
1. Build ForeB stages (NASM)
   foreboots/bios/stage1.asm  ->  build/stage1.bin  (512 B)
   foreboots/bios/stage2.asm  ->  build/stage2.bin  (<=8 KiB)
   foreboots/bios/stage3.asm  ->  build/stage3.bin  (<=8 KiB)

2. Build UEFI application (clang + ld.lld)
   foreboots/uefi/*.c  ->  build/BOOTX64.EFI

3. Build ESP (mkfs.fat + mcopy)
   build/esp.img  (48 MiB FAT16 with BOOTX64.EFI + kernel + config + assets)

4. Assemble BIOS boot blob
   cat stage1.bin stage2.bin stage3.bin > forebo_boot.img
   pad to sector 48, then append kernel ELF
   optionally append initrd at sector 560+

5. Create ISO root directory
   /tmp/forebo_iso_root/
     boot/forebo/forebo.img      # BIOS boot blob
     boot/forebo/forebo.cfg      # ForeB config
     boot/efi/esp.img            # UEFI ESP image

6. Run xorriso
   xorriso -as mkisofs -b ... -e ... -o forebo.iso /tmp/forebo_iso_root
```

### Inspecting the resulting ISO

```bash
# View El Torito boot catalog
xorriso -indev forebo.iso -report_el_torito as_mkisofs

# Extract and verify the MBR signature of the BIOS boot image
dd if=forebo.iso bs=512 skip=<boot_image_lba> count=1 | xxd -s 510 -l 2
# Should show: 55aa

# List files on the ISO
isoinfo -l -i forebo.iso
```

## 6. Disk Image Creation

The raw disk image (`forebo.img`) is created with `dd`:

```bash
# Create a blank 10 MiB image
dd if=/dev/zero of=build/forebo.img bs=512 count=20480 status=progress

# Write ForeB stages to fixed sector positions
dd if=build/stage1.bin of=build/forebo.img bs=512 count=1 conv=notrunc
dd if=build/stage2.bin of=build/forebo.img bs=512 seek=1 conv=notrunc
dd if=build/stage3.bin of=build/forebo.img bs=512 seek=17 conv=notrunc

# Write kernel at sector 48
dd if=kernel.bin of=build/forebo.img bs=512 seek=48 conv=notrunc

# Write initrd at the computed sector (past the kernel, 16-sector aligned)
dd if=initrd.tar of=build/forebo.img bs=512 seek=560 conv=notrunc
```

The `conv=notrunc` flag is critical -- it prevents `dd` from truncating the
image to the size of the input file.

### Verify the image

```bash
# Check MBR signature (bytes 510-511 must be 0x55 0xAA)
xxd -s 510 -l 2 build/forebo.img

# Check stage2 magic (bytes 2-3 of stage2 = 0xFEB1)
xxd -s 514 -l 2 build/forebo.img

# Verify ForeB stages
make -C ../foreboots check
```

## 7. initrd Inclusion

Forest OS supports an initrd (initial ramdisk) as a **multiboot1 module**.

### Building the initrd

The initrd is a tarball of files from `initrd/`:

```bash
# Build from the initrd directory tree
tar -C initrd -cf build/boot/initrd.tar .
```

The kernel build system (`fern/build/iso.mk`) automates this:

```make
$(INITRD): $(INITRD_FILES)
    tar -C $(INITRD_DIR) -cf $@ .
```

### BIOS initrd sector placement

ForeB's stage2 reads the initrd from a **fixed disk sector** (configured at
assemble time via `-DFOREB_INITRD_START_SECTOR`). The default sector is
computed from the kernel size -- just past the kernel, 16-sector aligned:

```
BIOS_INITRD_SECTOR = ((KERNEL_SEEK + KERNEL_SECTORS + 31) / 16) * 16
```

For a typical kernel this lands around sector 560. Override with:

```bash
make -C ../foreboots image BIOS_INITRD_SECTOR=600 BIOS_INITRD_SECTORS=256
```

### UEFI initrd

On the UEFI path, the initrd is placed on the ESP at `/forebo/initrd.tar` and
loaded by ForeB's UEFI loader. For Linux-type entries, the initrd is loaded
via the `LINUX_EFI_INITRD_MEDIA` LoadFile2 protocol.

## 8. Filesystem Layout on the ISO/IMG

### ISO 9660 layout

```
/                           ISO root
  boot/
    forebo/
      forebo.img            BIOS boot blob (stage1+2+3+kernel+initrd)
      forebo.cfg            ForeB configuration file
    efi/
      esp.img               FAT16 EFI System Partition image
```

### BIOS disk image layout

```
Sector   0 (0x000):  Stage 1 -- MBR boot code + partition table + 0x55AA
Sectors  1-16:       Stage 2 -- Real-mode GUI, VBE setup, kernel loader (8 KiB)
Sectors 17-32:       Stage 3 -- Protected-mode ELF parser (8 KiB)
Sector  48:          Kernel ELF (raw binary, variable size)
Sector  560+:        initrd tarball (multiboot module)
```

### UEFI ESP layout

```
/                           FAT16 root
  EFI/
    BOOT/
      BOOTX64.EFI           ForeB UEFI application (PE32+)
  forebo/
    kernel.elf              Forest kernel ELF
    forebo.cfg              ForeB configuration
    bg.bmp                  Menu background image
    icons/                  Menu icons (TGA format)
      os.tga, text.tga, ...
    initrd.tar              Initrd tarball (multiboot module)
    vmlinuz.README          Placeholder for Linux EFI-stub kernel
```

## 9. Testing the ISO/IMG in QEMU

ForeB provides ready-made QEMU targets:

### BIOS disk image

```bash
make -C ../foreboots qemu
# Equivalent to:
qemu-system-i386 \
    -drive file=build/forebo.img,format=raw,index=0,media=disk \
    -m 128M -vga std -serial stdio
```

### BIOS from ISO

```bash
make -C ../foreboots qemu-iso
# Equivalent to:
qemu-system-i386 -cdrom build/forebo.iso -m 128M -vga std -serial stdio
```

### UEFI from ESP

```bash
make -C ../foreboots qemu-uefi
# Requires OVMF firmware:
qemu-system-x86_64 -machine q35 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
    -drive if=pflash,format=raw,file=build/OVMF_VARS.local.fd \
    -drive format=raw,file=build/esp.img \
    -device qemu-xhci -device usb-tablet -device usb-mouse -device usb-kbd \
    -m 256M -serial stdio
```

### UEFI from hybrid ISO

```bash
make -C ../foreboots qemu-uefi-iso
# Same as above but boots from the ISO instead of esp.img
```

### Debug mode (GDB stub)

```bash
make -C ../foreboots qemu-debug
# GDB stub listens on localhost:1234
gdb -ex "target remote :1234" -ex "symbol-file build/stage1.bin"
```

### Tips

- Use `-serial stdio` to see ForeB's serial debug output
- Use `-display none -monitor stdio` for headless testing with QEMU monitor
- The UEFI path requires a copy of OVMF_VARS (writable NVRAM) per run
- Cross-arch emulation (aarch64, riscv64) is slower -- allow 20-30 seconds

## 10. Writing to Physical Media

### USB drive (from disk image)

```bash
# Identify your USB device (CAREFUL: wrong device = data loss)
lsblk

# Write the raw disk image
sudo dd if=build/forebo.img of=/dev/sdX bs=4M status=progress oflag=sync
```

### USB drive (from hybrid ISO)

The hybrid ISO can be written directly to a USB drive:

```bash
sudo dd if=build/forebo.iso of=/dev/sdX bs=4M status=progress oflag=sync
```

Or use a tool like Ventoy, Rufus, or `cat`:

```bash
sudo cat build/forebo.iso > /dev/sdX
```

### CD/DVD

```bash
# Burn ISO to CD
cdrecord dev=/dev/sr0 build/forebo.iso

# Or with wodim
wodim -v dev=/dev/sr0 build/forebo.iso

# Or use xorriso directly
xorriso -as cdrecord -v dev=/dev/sr0 build/forebo.iso
```

### Direct install to disk (ForeB stages only)

```bash
make -C ../foreboots install-disk DISK=/dev/sdX
# WARNING: Overwrites the MBR. Only use on dedicated drives.
```

## 11. Troubleshooting Boot Issues

### BIOS: "No bootable device"

- Verify MBR signature: `xxd -s 510 -l 2 build/forebo.img` should show `55aa`
- Verify stage2 magic: `xxd -s 514 -l 2 build/forebo.img` should show `b1fe`
- Check stage sizes: `make -C ../foreboots check`
- Ensure kernel was built before the image (`make build` then `make iso`)

### BIOS: Boots ForeB menu but kernel fails to load

- Verify kernel exists at the expected path:
  `ls -la build/32bit-bios-debug/boot/kernel.bin`
- Check kernel sector alignment: the kernel must start at sector 48
- Ensure the kernel is a valid ELF (stage3 parses ELF headers)
- Try with serial output: add `-serial stdio` to QEMU and check for
  ForeB's debug banners

### UEFI: Boots to firmware shell instead of ForeB

- Verify `BOOTX64.EFI` exists on the ESP: `mdir -i esp.img ::/EFI/BOOT/`
- Verify the ESP is FAT16: `file esp.img` should show "DOS/MBR boot sector"
- Check OVMF firmware paths: ensure `OVMF_CODE.4m.fd` and `OVMF_VARS.4m.fd`
  exist at `/usr/share/edk2/x64/`
- Copy OVMF_VARS to a writable file before each run (QEMU requires this)

### UEFI: ForeB menu appears but kernel doesn't boot

- Verify `kernel.elf` is on the ESP: `mdir -i esp.img ::/forebo/`
- Check `forebo.cfg` -- the `kernel=` path must be correct
- Ensure the kernel was linked for the right architecture (32-bit PM)

### ISO: xorriso errors

- Install xorriso: `sudo apt install xorriso`
- Check that boot images exist before running xorriso
- If `-isohybrid-gpt-basdat` is unsupported, the ISO still works for
  CD boot; it just won't boot from USB on UEFI hardware

### General debugging

```bash
# Full serial + GDB debug session
make -C ../foreboots qemu-debug
# In another terminal:
gdb -ex "target remote :1234"

# Dump ISO contents
isoinfo -l -i build/forebo.iso

# Verify binary sizes match constraints
make -C ../foreboots check
# stage1: exactly 512 bytes
# stage2: <= 8192 bytes
# stage3: <= 8192 bytes
# MBR signature: 0x55AA at offset 510
# stage2 magic: 0xFEB1 at offset 2
```

## 12. Distribution Preparation

### Building a release ISO

```bash
# From the fern/ directory:
cd fern

# 1. Configure
make defconfig   # or make menuconfig

# 2. Build kernel + bootable image
make all
# This builds the kernel, then calls `make iso` (BIOS) or `make img` (UEFI)

# 3. Verify
make -C ../foreboots check

# 4. Test in QEMU
make run         # or make run-bios / make run-uefi
```

### Creating a distribution archive

```bash
make dist
# Creates dist/forestos-complete-YYYYMMDD_HHMMSS.tar.gz
```

### Checksums

If `GENERATE_CHECKSUMS=yes` is set in `build-config.mk`, the build system
generates SHA256 checksums for all output artifacts.

### Multi-architecture builds

```bash
# Build all architecture combinations
make buildall
# Builds: 32-bios, 32-uefi, 64-bios, 64-uefi, arm, aarch64, aarch64-uefi,
#         riscv64, riscv64-uefi
```

### Customizing the boot menu

Edit `foreboots/forebo.cfg` to change:
- Menu timeout and default entry
- Theme and colors (`theme=forest` or custom `color_*` values)
- Boot entries (`menuentry` blocks with `type=forest|linux|chainload|shell|...`)
- Background image (`background=/forebo/bg.bmp`)
- Input settings (mouse, cursor, animations)

### Generating visual assets

```bash
make -C ../foreboots assets
# Runs tools/gen_assets.py to render bg.bmp + icons/*.tga from forebo_theme.h
```

### Rescue image

```bash
make -C ../foreboots rescue
# Creates forebo-rescue.iso (a standalone ForeB-based rescue image)
```

## Summary of Makefile Targets

| Target | Location | Description |
|--------|----------|-------------|
| `make iso` | `fern/` | Build hybrid ISO (delegates to ForeB) |
| `make img` | `fern/` | Build raw disk image (delegates to ForeB) |
| `make iso` | `foreboots/` | Alias for `iso-hybrid` |
| `make iso-hybrid` | `foreboots/` | Hybrid BIOS + UEFI ISO |
| `make iso-bios` | `foreboots/` | BIOS-only El Torito ISO |
| `make image` | `foreboots/` | Raw BIOS disk image |
| `make esp` | `foreboots/` | FAT16 EFI System Partition image |
| `make all` | `foreboots/` | Build BIOS stages + BOOTX64.EFI |
| `make uefi` | `foreboots/` | Build BOOTX64.EFI only |
| `make qemu` | `foreboots/` | Test BIOS image in QEMU |
| `make qemu-iso` | `foreboots/` | Test hybrid ISO in QEMU (BIOS) |
| `make qemu-uefi` | `foreboots/` | Test ESP in QEMU (UEFI/OVMF) |
| `make qemu-uefi-iso` | `foreboots/` | Test hybrid ISO in QEMU (UEFI) |
| `make qemu-debug` | `foreboots/` | BIOS image with GDB stub |
| `make check` | `foreboots/` | Verify binary sizes and signatures |
| `make assets` | `foreboots/` | Generate visual assets |
| `make rescue` | `foreboots/` | Create rescue ISO |
| `make install-disk` | `foreboots/` | Write ForeB to real disk (DANGER) |

## Further Reading

- [ForeB Architecture](../Bootloader/Architecture.md) -- bootloader internals
- [Kernel Build](Kernel-Build.md) -- compiling the Forest kernel
- [Toolchain](Toolchain.md) -- cross-compiler setup
- [foreboots/README.md](../../foreboots/README.md) -- ForeB source documentation
- [forebo.cfg reference](../../foreboots/forebo.cfg) -- boot menu configuration
