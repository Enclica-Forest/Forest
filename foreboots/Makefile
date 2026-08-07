# =============================================================================
# ForeB - Forest Bootloader Build System (unified BIOS + UEFI)
# =============================================================================
# BIOS (legacy) targets:
#   all         - Build stage1/2/3 BIOS binaries AND the UEFI BOOTX64.EFI app
#   image       - Build a raw disk image (forebo.img) with ForeB + kernel
#   iso         - Build a HYBRID ISO (BIOS + UEFI bootable)  [= iso-hybrid]
#   iso-bios    - Build a BIOS-only El Torito ISO (legacy behavior)
#   install     - Write stage1 + stage2 + stage3 to a real disk (set DISK=)
#   qemu        - Test ForeB (BIOS) in QEMU using forebo.img
#   qemu-iso    - Test the hybrid ISO in QEMU under BIOS
#   qemu-debug  - Run BIOS image with GDB stub (localhost:1234)
#   check       - Verify BIOS binary sizes and boot signatures
#   disasm      - Disassemble stages for inspection
#   clean       - Remove build outputs
#
# UEFI targets:
#   uefi          - Build BOOTX64.EFI (clang + ld.lld, self-contained)
#   esp           - Build a FAT EFI System Partition image (esp.img)
#   iso-hybrid    - Build a single ISO bootable on BOTH BIOS and UEFI
#   qemu-uefi     - Boot BOOTX64.EFI via OVMF using esp.img
#   qemu-uefi-iso - Boot the hybrid ISO under OVMF
#
# Variables:
#   DISK      - Target block device or image file for install (e.g. /dev/sdb)
#   KERNEL    - Path to kernel ELF/binary to embed (BIOS image + ESP + ISO)
#               Default: ../build/32bit-bios-debug/boot/kernel.bin
#   INITRD    - Optional initrd to embed (see config.h)
#   NASMFLAGS - Extra NASM flags (e.g. -DFOREB_DEFAULT_WIDTH=1280)
# =============================================================================

.PHONY: all clean install iso iso-bios iso-hybrid qemu qemu-iso qemu-debug \
        check disasm help uefi esp qemu-uefi qemu-uefi-iso screenshots assets \
        uefi-aa64 uefi-riscv esp-aa64 qemu-aa64 qemu-riscv uefi-all

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------
NASM     := nasm
DD       := dd
QEMU     := qemu-system-i386
QEMU64   := qemu-system-x86_64
OBJDUMP  := objdump
XORRISO  := xorriso
CLANG    := clang
LDLLD    := ld.lld
MKFSFAT  := mkfs.fat
MMD      := mmd
MCOPY    := mcopy

# Sources / outputs (BIOS)
STAGE1_SRC  := stage1.asm
STAGE2_SRC  := stage2.asm
STAGE3_SRC  := stage3.asm
STAGE1_BIN  := stage1.bin
STAGE2_BIN  := stage2.bin
STAGE3_BIN  := stage3.bin
DISK_IMAGE  := forebo.img
ISO_IMAGE   := forebo.iso

# UEFI sources / outputs
UEFI_SRC        := uefi/bootx64.c
UEFI_OBJ        := uefi/bootx64.o
# Graphical UI: GOP-drawn forest-theme menu + in-place load progress bar.
# Shares include/font8x8.h + include/forebo_theme.h with the BIOS path.
UEFI_UI_SRC     := uefi/ui.c
UEFI_UI_OBJ     := uefi/ui.o
# Feature modules. AUTO-DISCOVERED: every uefi/*.c EXCEPT the loader (bootx64.c)
# and the graphical UI (ui.c), which have their own dedicated object rules below.
# This wildcard means new modules added by this upgrade -- input.c (mouse), wm.c
# (window manager/compositor), boot_linux.c (EFI-stub vmlinuz + initrd),
# chainload.c (LoadImage of GRUB/another EFI app), fs_ext.c (ext2/3/4 read-only),
# fs_btrfs.c (btrfs detect/subvol listing), recovery.c (disk/data-recovery tools)
# -- are compiled and linked into BOOTX64.EFI automatically the moment their .c
# file exists, with no further Makefile edits. Original modules (image/config/
# modules/shell/anim) are picked up by the same glob.
UEFI_MOD_SRC    := $(filter-out $(UEFI_SRC) $(UEFI_UI_SRC),$(sort $(wildcard uefi/*.c)))
UEFI_MOD_OBJ    := $(UEFI_MOD_SRC:.c=.o)
# NASM long-mode -> 32-bit PM handoff trampoline (nasm -f win64). This is the
# forebo_handoff() the C loader externs. Falls back to uefi/trampoline.asm if a
# user drops one in, otherwise uses the shipped uefi/handoff64to32.asm.
UEFI_TRAMP_SRC  := $(firstword $(wildcard uefi/trampoline.asm) uefi/handoff64to32.asm)
UEFI_TRAMP_OBJ  := $(UEFI_TRAMP_SRC:.asm=.o)
UEFI_OBJS       := $(UEFI_OBJ) $(UEFI_UI_OBJ) $(UEFI_MOD_OBJ) $(UEFI_TRAMP_OBJ)
EFI_APP         := BOOTX64.EFI
ESP_IMAGE       := esp.img

# UEFI toolchain flags (exact recipe from the build contract; -Iinclude added so
# the shared boot_protocol.h can be found).
# -mno-stack-arg-probe: the MS-ABI target emits __chkstk stack-probe calls for
# functions with large (>1 page) stack frames -- e.g. the fs/chainload modules'
# on-stack sector buffers. There is no CRT in this freestanding link, so suppress
# the probe (UEFI gives us an ample, guard-page-free stack). -Iuefi so the new
# arch.h/efi_ext.h headers resolve for every module.
UEFI_CFLAGS  := -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
                -mno-red-zone -mno-mmx -mno-sse -mno-stack-arg-probe \
                -Wall -Wextra -std=c11 -Iinclude -Iuefi
UEFI_LDFLAGS := -flavor link -subsystem:efi_application -entry:efi_main

# OVMF firmware (UEFI). VARS is copied to a writable temp per run.
OVMF_CODE      := /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS      := /usr/share/edk2/x64/OVMF_VARS.4m.fd
OVMF_VARS_COPY := OVMF_VARS.local.fd

# ESP FAT image size in KiB (~48 MiB). Must hold BOOTX64.EFI plus the kernel
# ELF (the Forest debug kernel's apparent size is ~16 MiB), so 16 MiB is too
# small; 48 MiB leaves comfortable headroom.
ESP_SIZE_KIB := 49152

# NASM flags: flat binary. We avoid -w+all because NASM's struc/endstruc
# emit internal [warning push/pop] pragmas that are flagged under -w+all, and
# ForeB stages are flat binaries loaded at their ORG address so the absolute
# dword/word relocations are correct and benign. -D overrides may be appended.
NASMFLAGS   ?= -f bin

# Disk layout (MUST match config.h defaults):
#   sector 0       : stage1 (MBR)
#   sectors 1..16  : stage2  (seek=1)
#   sectors 17..32 : stage3  (seek=17)
#   sector  48+    : kernel  (seek=48)
STAGE2_SEEK  := 1
STAGE3_SEEK  := 17
KERNEL_SEEK  := 48

# 10 MiB blank image
IMAGE_SIZE_SECTORS := 20480

# Default kernel path (relative to foreboots/)
KERNEL      ?= ../build/32bit-bios-debug/boot/kernel.bin
INITRD      ?=
# Optional real Linux payload for the 'type=linux' entry (EFI-stub vmlinuz +
# initrd). If unset/missing, the ESP gets a /forebo/vmlinuz.README placeholder
# instead. Usage: make esp VMLINUZ=/boot/vmlinuz LINUX_INITRD=/boot/initrd.img
VMLINUZ      ?=
LINUX_INITRD ?=

DISK        ?= $(DISK_IMAGE)

# -----------------------------------------------------------------------------
# BIOS multiboot module (initrd) - config-defined disk sector
# -----------------------------------------------------------------------------
# The BIOS path can register ONE multiboot1 module (an initrd). stage2 reads it
# from a fixed disk sector into low memory (INITRD_LOAD_PHYS) and publishes it
# via the multiboot mods array (MB_FLAG_MODS) + foreboots_boot_info.initrd_*.
# This is enabled at *assemble* time by passing FOREB_INITRD_START_SECTOR to
# stage2 (config.h defaults it to 0 = disabled, so a bare `nasm` build is
# unchanged). Set BIOS_INITRD_SECTOR=0 to build a module-less stage2.
#
# The module MUST sit on disk *past* the kernel (kernel starts at sector 48 and
# can be many MiB). The default sector is therefore COMPUTED from the current
# kernel's size: just past the kernel, 16-sector-aligned, with a small margin.
# If no kernel is present it falls back to sector 560. Override by setting
# BIOS_INITRD_SECTOR / BIOS_INITRD_SECTORS explicitly. Because the default
# depends on the kernel size, stage2 also lists $(KERNEL) as a prerequisite so a
# changed kernel re-bakes the sector into stage2 (keeping the assembled sector
# and the dd'd sector in lock-step).
#
#   BIOS_INITRD_SECTOR  - LBA (relative to the boot image) of the module.
#   BIOS_INITRD_SECTORS - # of 512 B sectors stage2 reads (module max size). tar
#                         tolerates trailing zero padding, so this is an upper
#                         bound; the sample initrd.tar is a few KiB.
KERNEL_SECTORS := $(shell if [ -f "$(KERNEL)" ]; then \
    echo $$(( ( $$(wc -c < "$(KERNEL)") + 511 ) / 512 )); else echo 0; fi)
ifeq ($(KERNEL_SECTORS),0)
BIOS_INITRD_SECTOR   ?= 560
else
BIOS_INITRD_SECTOR   ?= $(shell echo $$(( ( ( $(KERNEL_SEEK) + $(KERNEL_SECTORS) + 31 ) / 16 ) * 16 )) )
endif
BIOS_INITRD_SECTORS  ?= 128
BIOS_INITRD_DEFS := -DFOREB_INITRD_START_SECTOR=$(BIOS_INITRD_SECTOR) \
                    -DINITRD_MAX_SECTORS=$(BIOS_INITRD_SECTORS)
# Where the sample initrd is written on the BIOS disk image / boot blob.
INITRD_SEEK          := $(BIOS_INITRD_SECTOR)

# Generated visual assets (backgrounds/icons) + sample module.
GEN_ASSETS  := tools/gen_assets.py
ASSETS_DIR  := assets
ASSET_BG    := $(ASSETS_DIR)/bg.bmp
SAMPLE_INITRD := $(ASSETS_DIR)/initrd.tar
FOREBO_CFG  := forebo.cfg

# Detect optional isohybrid GPT marking support in xorriso
ISOHYBRID_FLAG := $(shell $(XORRISO) -as mkisofs -help 2>&1 | grep -q 'isohybrid-gpt-basdat' && echo -isohybrid-gpt-basdat)

# Colors
RED   := \033[31;01m
GREEN := \033[32;01m
CYAN  := \033[36;01m
RESET := \033[0m

# =============================================================================
# Default target: BIOS binaries + UEFI app
# =============================================================================

all: $(STAGE1_BIN) $(STAGE2_BIN) $(STAGE3_BIN) $(EFI_APP)
	@echo "$(GREEN)ForeB build complete (BIOS stages + BOOTX64.EFI).$(RESET)"
	@$(MAKE) --no-print-directory check

# =============================================================================
# Assembly rules (BIOS stages)
# =============================================================================

$(STAGE1_BIN): $(STAGE1_SRC) config.h
	@echo "$(CYAN)Assembling Stage 1 (MBR)...$(RESET)"
	$(NASM) $(NASMFLAGS) -l stage1.lst -o $@ $<
	@echo "$(GREEN)  Stage 1: $@ ($$(wc -c < $@) bytes, must be 512)$(RESET)"

$(STAGE2_BIN): $(STAGE2_SRC) config.h forebo.h forebo64.h $(wildcard $(KERNEL))
	@echo "$(CYAN)Assembling Stage 2 (GUI + BIOS + menu, initrd sector $(BIOS_INITRD_SECTOR))...$(RESET)"
	$(NASM) $(NASMFLAGS) $(BIOS_INITRD_DEFS) -l stage2.lst -o $@ $<
	@echo "$(GREEN)  Stage 2: $@ ($$(wc -c < $@) bytes, max 8192)$(RESET)"

$(STAGE3_BIN): $(STAGE3_SRC) config.h forebo64.h
	@echo "$(CYAN)Assembling Stage 3 (PM ELF loader + handoff)...$(RESET)"
	$(NASM) $(NASMFLAGS) -l stage3.lst -o $@ $<
	@echo "$(GREEN)  Stage 3: $@ ($$(wc -c < $@) bytes, max 8192)$(RESET)"

# =============================================================================
# UEFI application (BOOTX64.EFI)
# =============================================================================

# Shared header set for the UEFI C modules. The per-module uefi/*.h are picked
# up via $(wildcard) so a not-yet-created header never breaks parsing.
UEFI_MOD_HDRS := uefi/efi.h uefi/efi_ext.h uefi/arch.h include/boot_protocol.h \
                 include/forebo_cfg.h include/forebo_theme.h include/font8x16.h \
                 $(wildcard uefi/ui.h uefi/image.h uefi/config.h uefi/modules.h \
                            uefi/shell.h uefi/anim.h uefi/wm.h uefi/input.h \
                            uefi/boot_linux.h uefi/chainload.h uefi/fs_ext.h \
                            uefi/fs_btrfs.h uefi/recovery.h uefi/tools.h \
                            uefi/fwsetup.h)

# C -> object (freestanding PE/COFF target)
$(UEFI_OBJ): $(UEFI_SRC) include/boot_protocol.h uefi/efi.h $(UEFI_MOD_HDRS)
	@echo "$(CYAN)Compiling UEFI loader ($<)...$(RESET)"
	$(CLANG) $(UEFI_CFLAGS) -c $< -o $@

# Graphical UI module (GOP menu + progress bar) -> object. Same freestanding
# recipe as the loader; pulls in the shared font + theme headers.
$(UEFI_UI_OBJ): $(UEFI_UI_SRC) include/font8x16.h include/forebo_theme.h uefi/efi.h
	@echo "$(CYAN)Compiling UEFI graphical UI ($<)...$(RESET)"
	$(CLANG) $(UEFI_CFLAGS) -c $< -o $@

# Feature modules (image/config/modules/shell/anim) -> objects. Static pattern
# rule scoped to exactly $(UEFI_MOD_OBJ); same freestanding clang recipe.
$(UEFI_MOD_OBJ): uefi/%.o: uefi/%.c $(UEFI_MOD_HDRS)
	@echo "$(CYAN)Compiling UEFI module ($<)...$(RESET)"
	$(CLANG) $(UEFI_CFLAGS) -c $< -o $@

# NASM trampoline -> win64 object. -I. finds config.h, -Iinclude finds
# boot_protocol.inc (which the trampoline %includes).
uefi/%.o: uefi/%.asm include/boot_protocol.inc config.h
	@echo "$(CYAN)Assembling UEFI trampoline ($<)...$(RESET)"
	$(NASM) -f win64 -I. -Iinclude -o $@ $<

# Link the PE32+ EFI application
uefi: $(EFI_APP)

$(EFI_APP): $(UEFI_OBJS)
	@echo "$(CYAN)Linking UEFI application ($(EFI_APP))...$(RESET)"
	$(LDLLD) $(UEFI_LDFLAGS) -out:$@ $(UEFI_OBJS)
	@echo "$(GREEN)  UEFI app: $@ ($$(wc -c < $@) bytes)$(RESET)"

# =============================================================================
# Cross-architecture UEFI apps: BOOTAA64.EFI (aarch64) + BOOTRISCV64.EFI (riscv64)
# =============================================================================
# ForeB's UEFI C tree compiles for three UEFI arches from one source (see
# uefi/arch.h). The x86-only Forest multiboot handoff (handoff64to32.asm +
# forebo_handoff()) is #ifdef-guarded to x86_64, so the aarch64/riscv builds link
# WITHOUT the NASM trampoline and expose UI + shell + Linux(vmlinuz) boot +
# chainload + filesystems + recovery. Objects go to per-arch build dirs so they
# never clash with the x86 uefi/*.o.
#
#   aarch64: clang -target aarch64-unknown-windows  => valid PE-COFF, fully built
#            and bootable under /usr/share/edk2/aa64 OVMF (see qemu-aa64).
#   riscv64: clang has NO RISC-V COFF/PE backend and this binutils objcopy has no
#            pei-riscv64 target, so a BOOTRISCV64.EFI PE CANNOT be produced with
#            the installed toolchain. 'uefi-riscv' still COMPILES every source to
#            RISC-V ELF objects (proving the C is portable) and links a static-PIE
#            ELF, then prints the packaging boundary (needs edk2 GenFw). It does
#            NOT fail the build.
# -----------------------------------------------------------------------------
EFI_APP_AA64   := BOOTAA64.EFI
EFI_APP_RISCV  := BOOTRISCV64.EFI
AA64_BUILD     := build/aa64
RISCV_BUILD    := build/riscv
ESP_AA64_IMAGE := esp-aa64.img

# Every uefi/*.c takes part in the cross-arch build (bootx64.c included -- it is
# arch-guarded; its handoff call compiles to foreb_multiboot_unsupported()). The
# x86 NASM trampoline is deliberately excluded.
UEFI_ALL_SRC   := $(sort $(wildcard uefi/*.c))
AA64_OBJS      := $(patsubst uefi/%.c,$(AA64_BUILD)/%.o,$(UEFI_ALL_SRC))
RISCV_OBJS     := $(patsubst uefi/%.c,$(RISCV_BUILD)/%.o,$(UEFI_ALL_SRC))

# aarch64: same freestanding recipe as x86 but the aarch64 Windows (PE) target;
# drop the x86-only -mno-mmx/-mno-sse and add -mgeneral-regs-only (no FP/NEON in
# firmware context). -Iuefi so arch.h/efi_ext.h resolve.
AA64_CFLAGS    := -target aarch64-unknown-windows -ffreestanding -fshort-wchar \
                  -mno-red-zone -mgeneral-regs-only -mno-stack-arg-probe \
                  -Wall -Wextra -std=c11 -Iinclude -Iuefi
# riscv64: ELF object target (no PE backend). -Wno-ignored-attributes silences the
# harmless 'ms_abi ignored' notes (RISC-V UEFI uses the standard C ABI, which is
# what clang emits anyway).
RISCV_CFLAGS   := -target riscv64-unknown-elf -ffreestanding -fshort-wchar \
                  -fPIC -Wall -std=c11 -Wno-ignored-attributes -mno-relax \
                  -Iinclude -Iuefi

# Cross-arch OVMF firmware (paths verified on this box; NOT the x64 4m.fd names).
OVMF_AA64_CODE  := /usr/share/edk2/aa64/QEMU_EFI.fd
OVMF_AA64_VARS  := /usr/share/edk2/aa64/QEMU_VARS.fd
OVMF_AA64_VARS_COPY := QEMU_VARS.aa64.local.fd
OVMF_RISCV_CODE := /usr/share/edk2/riscv64/RISCV_VIRT_CODE.fd
OVMF_RISCV_VARS := /usr/share/edk2/riscv64/RISCV_VIRT_VARS.fd

# ---- aarch64 object rule + link ----
$(AA64_BUILD)/%.o: uefi/%.c $(UEFI_MOD_HDRS)
	@mkdir -p $(AA64_BUILD)
	@echo "$(CYAN)Compiling (aarch64) $<...$(RESET)"
	$(CLANG) $(AA64_CFLAGS) -c $< -o $@

uefi-aa64: $(EFI_APP_AA64)

# Build every UEFI arch we can: x86 PE + aarch64 PE, then the riscv objects +
# boundary note. Never fails on the riscv boundary.
uefi-all: uefi uefi-aa64 uefi-riscv
	@echo "$(GREEN)Cross-arch UEFI build complete (x64 + aa64 PE; riscv objects).$(RESET)"

$(EFI_APP_AA64): $(AA64_OBJS)
	@echo "$(CYAN)Linking aarch64 UEFI application ($(EFI_APP_AA64))...$(RESET)"
	$(LDLLD) -flavor link -subsystem:efi_application -entry:efi_main \
	         -out:$@ $(AA64_OBJS)
	@echo "$(GREEN)  aarch64 UEFI app: $@ ($$(wc -c < $@) bytes)$(RESET)"

# ---- riscv64 object rule + best-effort ELF link + boundary note ----
$(RISCV_BUILD)/%.o: uefi/%.c $(UEFI_MOD_HDRS)
	@mkdir -p $(RISCV_BUILD)
	@echo "$(CYAN)Compiling (riscv64 ELF) $<...$(RESET)"
	$(CLANG) $(RISCV_CFLAGS) -c $< -o $@

uefi-riscv: $(RISCV_OBJS)
	@echo "$(CYAN)Linking riscv64 static-PIE ELF (proves portability)...$(RESET)"
	@$(LDLLD) -flavor gnu -pie -e efi_main -o $(RISCV_BUILD)/forebo-riscv.elf \
	         $(RISCV_OBJS) 2>/dev/null \
	  && echo "$(GREEN)  riscv64 ELF linked: $(RISCV_BUILD)/forebo-riscv.elf$(RESET)" \
	  || echo "$(RED)  riscv64 ELF link skipped/failed (informational).$(RESET)"
	@echo ""
	@echo "$(RED)========================= PACKAGING BOUNDARY =========================$(RESET)"
	@echo "$(CYAN)ForeB riscv64: every uefi/*.c COMPILED to RISC-V objects OK, but a"
	@echo "bootable BOOTRISCV64.EFI (PE32+) CANNOT be produced with this toolchain:"
	@echo "  * clang/lld have no RISC-V COFF/PE backend (emit ELF only)."
	@echo "  * this binutils objcopy has no 'pei-riscv64' target for ELF->PE."
	@echo "To ship BOOTRISCV64.EFI, convert the ELF with edk2 GenFw (BaseTools) or"
	@echo "a binutils built with RISC-V PE support, then place it at"
	@echo "::/EFI/BOOT/BOOTRISCV64.EFI on the ESP.$(RESET)"
	@echo "$(RED)======================================================================$(RESET)"

# =============================================================================
# Cross-arch ESP images + QEMU (aarch64 fully bootable; riscv note only)
# =============================================================================
# esp-aa64: FAT image with BOOTAA64.EFI at the default removable-media path plus
# the same forebo.cfg + assets the x86 ESP carries. There is NO Forest kernel.elf
# (multiboot is x86-only); instead a placeholder /forebo/vmlinuz.README documents
# where to drop a real EFI-stub vmlinuz for the 'linux' entry type.
esp-aa64: $(ESP_AA64_IMAGE)

$(ESP_AA64_IMAGE): $(EFI_APP_AA64) assets $(FOREBO_CFG)
	@echo "$(CYAN)Building aarch64 ESP ($(ESP_AA64_IMAGE))...$(RESET)"
	@rm -f $(ESP_AA64_IMAGE)
	$(MKFSFAT) -C -F 16 -n FOREBAA64 $(ESP_AA64_IMAGE) $(ESP_SIZE_KIB) >/dev/null
	$(MMD) -i $(ESP_AA64_IMAGE) ::/EFI ::/EFI/BOOT ::/forebo ::/forebo/icons
	$(MCOPY) -i $(ESP_AA64_IMAGE) $(EFI_APP_AA64) ::/EFI/BOOT/BOOTAA64.EFI
	@echo "  BOOTAA64.EFI -> ::/EFI/BOOT/BOOTAA64.EFI"
	$(MCOPY) -i $(ESP_AA64_IMAGE) $(FOREBO_CFG) ::/forebo/forebo.cfg
	@printf 'Drop an EFI-stub vmlinuz here as \\forebo\\vmlinuz and reference it\nwith a `type=linux` entry in forebo.cfg (initrd= optional). ForeB boots it\nvia LoadImage/StartImage with the LINUX_EFI_INITRD_MEDIA LoadFile2 protocol.\n' > /tmp/forebo_vmlinuz_readme.txt
	$(MCOPY) -i $(ESP_AA64_IMAGE) /tmp/forebo_vmlinuz_readme.txt ::/forebo/vmlinuz.README
	@rm -f /tmp/forebo_vmlinuz_readme.txt
	@if [ -f "$(ASSET_BG)" ]; then $(MCOPY) -i $(ESP_AA64_IMAGE) $(ASSET_BG) ::/forebo/bg.bmp; fi
	@if ls $(ASSETS_DIR)/icons/*.tga >/dev/null 2>&1; then $(MCOPY) -i $(ESP_AA64_IMAGE) $(ASSETS_DIR)/icons/*.tga ::/forebo/icons/; fi
	@echo "$(GREEN)  aarch64 ESP ready: $(ESP_AA64_IMAGE)$(RESET)"

# Boot BOOTAA64.EFI under aarch64 OVMF. VERIFIED to reach the ForeB menu (forest
# background + mouse cursor + Linux/chainload/shell/recovery entries) on this box.
# Notes on the device recipe (these matter on ArmVirt OVMF):
#   * ramfb           -> a simple GOP framebuffer ForeB's ui.c draws into. Works
#                        with any -display backend (unlike some virtio-gpu paths).
#   * qemu-xhci + usb-storage,bootindex=0 -> the ESP as a REMOVABLE USB volume.
#     OVMF's BDS auto-boots the removable \EFI\BOOT\BOOTAA64.EFI reliably; a bare
#     virtio-blk data disk is often NOT added to BootOrder and the firmware stalls
#     on its default (non-block) boot option instead.
#   * -nic none       -> drop QEMU's default NIC so OVMF does not sit in a PXE
#                        attempt before falling through to the USB ESP.
# Cross-arch runs under TCG (no KVM), so the menu takes ~20-30 s to appear.
qemu-aa64: esp-aa64
	@echo "$(CYAN)Starting QEMU (aarch64/virt, OVMF) with $(ESP_AA64_IMAGE)...$(RESET)"
	@echo "$(CYAN)  (TCG-emulated: allow ~20-30s for the menu to render.)$(RESET)"
	@cp -f $(OVMF_AA64_VARS) $(OVMF_AA64_VARS_COPY)
	qemu-system-aarch64 -M virt -cpu cortex-a57 \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_AA64_CODE) \
	  -drive if=pflash,format=raw,file=$(OVMF_AA64_VARS_COPY) \
	  -device ramfb -device qemu-xhci \
	  -drive if=none,file=$(ESP_AA64_IMAGE),format=raw,id=espaa64 \
	  -device usb-storage,drive=espaa64,bootindex=0 \
	  -nic none -m 512M -serial stdio \
	  -name "ForeB UEFI aarch64"

# riscv has no producible PE yet (see uefi-riscv). This target documents the
# would-be run line so it is ready the moment a GenFw-built BOOTRISCV64.EFI exists.
qemu-riscv:
	@echo "$(RED)qemu-riscv: BOOTRISCV64.EFI is not produced by this toolchain.$(RESET)"
	@echo "$(CYAN)Run 'make uefi-riscv' to compile the RISC-V objects + see the boundary."
	@echo "Once a GenFw-built BOOTRISCV64.EFI exists on a FAT ESP, boot it with:"
	@echo "  qemu-system-riscv64 -M virt -m 256M \\ "
	@echo "    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_RISCV_CODE) \\ "
	@echo "    -drive if=pflash,format=raw,file=<copy of $(OVMF_RISCV_VARS)> \\ "
	@echo "    -drive format=raw,file=<esp-riscv.img> -serial stdio -display none$(RESET)"

# =============================================================================
# Size + signature verification (BIOS)
# =============================================================================

check: $(STAGE1_BIN) $(STAGE2_BIN) $(STAGE3_BIN)
	@echo "$(CYAN)Checking binary sizes and signatures...$(RESET)"
	@S1=$$(wc -c < $(STAGE1_BIN)); \
	 if [ "$$S1" -eq 512 ]; then \
	   echo "$(GREEN)  [OK] stage1.bin: $$S1 bytes (exactly 512)$(RESET)"; \
	 else echo "$(RED)  [FAIL] stage1.bin: $$S1 bytes (expected 512!)$(RESET)"; exit 1; fi
	@S2=$$(wc -c < $(STAGE2_BIN)); \
	 if [ "$$S2" -le 8192 ]; then \
	   echo "$(GREEN)  [OK] stage2.bin: $$S2 bytes (<= 8192)$(RESET)"; \
	 else echo "$(RED)  [FAIL] stage2.bin: $$S2 bytes (exceeds 8192!)$(RESET)"; exit 1; fi
	@S3=$$(wc -c < $(STAGE3_BIN)); \
	 if [ "$$S3" -le 8192 ]; then \
	   echo "$(GREEN)  [OK] stage3.bin: $$S3 bytes (<= 8192)$(RESET)"; \
	 else echo "$(RED)  [FAIL] stage3.bin: $$S3 bytes (exceeds 8192!)$(RESET)"; exit 1; fi
	@SIG=$$(xxd -s 510 -l 2 $(STAGE1_BIN) | awk '{print $$2}'); \
	 if [ "$$SIG" = "55aa" ]; then \
	   echo "$(GREEN)  [OK] MBR boot signature 0x55AA$(RESET)"; \
	 else echo "$(RED)  [FAIL] MBR signature: $$SIG (expected 55aa)$(RESET)"; exit 1; fi
	@MAG=$$(xxd -s 2 -l 2 $(STAGE2_BIN) | awk '{print $$2}'); \
	 if [ "$$MAG" = "b1fe" ]; then \
	   echo "$(GREEN)  [OK] stage2 magic 0xFEB1$(RESET)"; \
	 else echo "$(RED)  [FAIL] stage2 magic: $$MAG (expected b1fe)$(RESET)"; exit 1; fi

# =============================================================================
# Generated assets (UEFI background + icons, and the sample multiboot module)
# =============================================================================
# tools/gen_assets.py (stdlib only) renders assets/bg.bmp + assets/icons/*.tga
# straight from include/forebo_theme.h, so the theme is the single source of
# truth and no binaries are checked in. The sample initrd.tar is a tiny tar of a
# couple text files, created only if one is not already present. Both firmwares
# stage the result: UEFI copies them onto the ESP; BIOS dd's the module onto the
# disk image / boot blob at the configured initrd sector.
.PHONY: assets
assets: $(ASSET_BG) $(SAMPLE_INITRD)

$(ASSET_BG): $(GEN_ASSETS) include/forebo_theme.h
	@echo "$(CYAN)Generating UEFI assets (background + icons) via $(GEN_ASSETS)...$(RESET)"
	@python3 $(GEN_ASSETS)
	@echo "$(GREEN)  Assets generated under $(ASSETS_DIR)/$(RESET)"

$(SAMPLE_INITRD):
	@echo "$(CYAN)Building sample multiboot module ($(SAMPLE_INITRD))...$(RESET)"
	@mkdir -p $(ASSETS_DIR)/initrd_src/forebo
	@printf 'ForeB sample initrd (multiboot module).\nReplace with your real ramdisk.\n' > $(ASSETS_DIR)/initrd_src/README
	@printf 'ForeB initrd sample v1\n' > $(ASSETS_DIR)/initrd_src/forebo/version
	@printf 'Hello from the ForeB sample module!\n' > $(ASSETS_DIR)/initrd_src/forebo/hello.txt
	@tar -C $(ASSETS_DIR)/initrd_src -cf $(SAMPLE_INITRD) README forebo
	@rm -rf $(ASSETS_DIR)/initrd_src
	@echo "$(GREEN)  Sample module ready: $(SAMPLE_INITRD)$(RESET)"

# =============================================================================
# Disk image creation (BIOS)
# =============================================================================

$(DISK_IMAGE):
	@echo "$(CYAN)Creating blank disk image (10 MiB)...$(RESET)"
	$(DD) if=/dev/zero of=$(DISK_IMAGE) bs=512 count=$(IMAGE_SIZE_SECTORS) status=progress
	@echo "$(GREEN)  Created: $(DISK_IMAGE)$(RESET)"

# Build complete disk image: MBR + Stage2 + Stage3 + Kernel (+ sample module)
image: $(STAGE1_BIN) $(STAGE2_BIN) $(STAGE3_BIN) $(SAMPLE_INITRD) $(DISK_IMAGE)
	@echo "$(CYAN)Writing ForeB to disk image...$(RESET)"
	$(DD) if=$(STAGE1_BIN) of=$(DISK_IMAGE) bs=512 count=1 conv=notrunc
	@echo "  stage1 (MBR)  -> sector 0"
	$(DD) if=$(STAGE2_BIN) of=$(DISK_IMAGE) bs=512 seek=$(STAGE2_SEEK) conv=notrunc
	@echo "  stage2        -> sectors $(STAGE2_SECTOR_START)..$(STAGE2_SECTOR_END)"
	$(DD) if=$(STAGE3_BIN) of=$(DISK_IMAGE) bs=512 seek=$(STAGE3_SEEK) conv=notrunc
	@echo "  stage3        -> sectors $(STAGE3_SECTOR_START)..$(STAGE3_SECTOR_END)"
	@if [ -f "$(KERNEL)" ]; then \
	  echo "$(CYAN)  Writing kernel from $(KERNEL) -> sector $(KERNEL_SEEK)...$(RESET)"; \
	  $(DD) if=$(KERNEL) of=$(DISK_IMAGE) bs=512 seek=$(KERNEL_SEEK) conv=notrunc; \
	  echo "$(GREEN)  Kernel embedded at sector $(KERNEL_SEEK).$(RESET)"; \
	else \
	  echo "$(RED)  WARNING: Kernel not found at $(KERNEL).$(RESET)"; \
	  echo "$(RED)  Run 'make image KERNEL=/path/to/kernel.bin' or build the OS first.$(RESET)"; \
	fi
	@INITRD_FILE="$(INITRD)"; \
	 if [ -z "$$INITRD_FILE" ] && [ -f "$(SAMPLE_INITRD)" ]; then \
	   INITRD_FILE="$(SAMPLE_INITRD)"; \
	 fi; \
	 if [ -n "$$INITRD_FILE" ] && [ -f "$$INITRD_FILE" ]; then \
	   echo "$(CYAN)  Writing multiboot module $$INITRD_FILE -> sector $(INITRD_SEEK)...$(RESET)"; \
	   $(DD) if=$$INITRD_FILE of=$(DISK_IMAGE) bs=512 seek=$(INITRD_SEEK) conv=notrunc; \
	   echo "$(GREEN)  Module (initrd) embedded at sector $(INITRD_SEEK); stage2 built for sector $(BIOS_INITRD_SECTOR).$(RESET)"; \
	 else \
	   echo "$(RED)  Note: no initrd; stage2 will find zeros at sector $(BIOS_INITRD_SECTOR) (empty module).$(RESET)"; \
	 fi
	@echo "$(GREEN)Disk image ready: $(DISK_IMAGE)$(RESET)"

# Sector ranges (informational; kept in sync with config.h)
STAGE2_SECTOR_START := 1
STAGE2_SECTOR_END   := 16
STAGE3_SECTOR_START := 17
STAGE3_SECTOR_END   := 32
# INITRD_SEEK is defined earlier (= BIOS_INITRD_SECTOR) so the sector stage2
# assembles for and the sector the image is written to always agree.

# =============================================================================
# Bootable ISOs
# =============================================================================
# ForeB boots from the BIOS El Torito boot image which is a concatenated
# stage1+stage2+stage3 "boot blob". The MBR (stage1) is the first 512 bytes;
# BIOS loads it as the no-emulation boot image, stage1 loads stage2 from LBA 1,
# stage2 loads stage3 from LBA 17 and the kernel from LBA 48. The kernel is
# placed on the boot blob at blob-sector 48 (LBAs are relative to the start of
# the El Torito boot image).
#
# For the hybrid ISO we additionally register the FAT ESP image (esp.img) as a
# SECOND El Torito boot entry with the EFI platform id, so UEFI firmware loads
# EFI/BOOT/BOOTX64.EFI from it. The -isohybrid-gpt-basdat flag marks that ESP as
# an EFI System Partition in a protective GPT so the same ISO also boots from USB
# on real UEFI machines.

# 'iso' now produces the dual-boot hybrid image.
iso: iso-hybrid

# ----- Legacy BIOS-only ISO (original behavior) -----
iso-bios: $(STAGE1_BIN) $(STAGE2_BIN) $(STAGE3_BIN) $(SAMPLE_INITRD)
	@echo "$(CYAN)Creating BIOS-only bootable ISO with ForeB...$(RESET)"
	@rm -rf /tmp/forebo_iso_root
	@mkdir -p /tmp/forebo_iso_root/boot/forebo
	@cat $(STAGE1_BIN) $(STAGE2_BIN) $(STAGE3_BIN) > /tmp/forebo_boot.img
	@BLOBSIZE=$$(wc -c < /tmp/forebo_boot.img); \
	  NEED=$$(( $(KERNEL_SEEK) * 512 )); \
	  if [ "$$BLOBSIZE" -lt "$$NEED" ]; then \
	    dd if=/dev/zero bs=1 count=$$(( NEED - BLOBSIZE )) >> /tmp/forebo_boot.img 2>/dev/null; \
	  fi
	@if [ -f "$(KERNEL)" ]; then \
	  cat $(KERNEL) >> /tmp/forebo_boot.img; \
	  echo "  Kernel appended to boot blob at sector $(KERNEL_SEEK)."; \
	else \
	  echo "$(RED)  WARNING: kernel not found at $(KERNEL) -- ISO will boot ForeB only.$(RESET)"; \
	fi
	@# --- append the sample multiboot module at the BIOS initrd sector ---
	@if [ -f "$(SAMPLE_INITRD)" ]; then \
	  BLOBSIZE=$$(wc -c < /tmp/forebo_boot.img); \
	  NEED=$$(( $(INITRD_SEEK) * 512 )); \
	  if [ "$$BLOBSIZE" -lt "$$NEED" ]; then \
	    dd if=/dev/zero bs=1 count=$$(( NEED - BLOBSIZE )) >> /tmp/forebo_boot.img 2>/dev/null; \
	    cat $(SAMPLE_INITRD) >> /tmp/forebo_boot.img; \
	    echo "  Sample module appended to boot blob at sector $(INITRD_SEEK)."; \
	  else \
	    echo "$(RED)  WARNING: kernel overruns initrd sector $(INITRD_SEEK); module skipped.$(RESET)"; \
	  fi; \
	fi
	@cp /tmp/forebo_boot.img /tmp/forebo_iso_root/boot/forebo/forebo.img
	@cp $(FOREBO_CFG) /tmp/forebo_iso_root/boot/forebo/forebo.cfg 2>/dev/null || true
	@if command -v $(XORRISO) >/dev/null 2>&1; then \
	  $(XORRISO) -as mkisofs \
	    -b boot/forebo/forebo.img \
	    -no-emul-boot \
	    -boot-load-size 48 \
	    -boot-info-table \
	    -o $(ISO_IMAGE) \
	    /tmp/forebo_iso_root; \
	  echo "$(GREEN)  ISO created: $(ISO_IMAGE)$(RESET)"; \
	else \
	  echo "$(RED)xorriso not found; falling back to raw image.$(RESET)"; \
	  cp /tmp/forebo_boot.img $(ISO_IMAGE); \
	fi
	@rm -rf /tmp/forebo_iso_root /tmp/forebo_boot.img

# ----- Hybrid BIOS + UEFI ISO -----
iso-hybrid: $(STAGE1_BIN) $(STAGE2_BIN) $(STAGE3_BIN) $(SAMPLE_INITRD) $(ESP_IMAGE)
	@echo "$(CYAN)Creating HYBRID (BIOS + UEFI) bootable ISO...$(RESET)"
	@rm -rf /tmp/forebo_iso_root
	@mkdir -p /tmp/forebo_iso_root/boot/forebo /tmp/forebo_iso_root/boot/efi
	@# --- BIOS El Torito boot blob: stage1 + stage2 + stage3 + kernel ---
	@cat $(STAGE1_BIN) $(STAGE2_BIN) $(STAGE3_BIN) > /tmp/forebo_boot.img
	@BLOBSIZE=$$(wc -c < /tmp/forebo_boot.img); \
	  NEED=$$(( $(KERNEL_SEEK) * 512 )); \
	  if [ "$$BLOBSIZE" -lt "$$NEED" ]; then \
	    dd if=/dev/zero bs=1 count=$$(( NEED - BLOBSIZE )) >> /tmp/forebo_boot.img 2>/dev/null; \
	  fi
	@if [ -f "$(KERNEL)" ]; then \
	  cat $(KERNEL) >> /tmp/forebo_boot.img; \
	  echo "  Kernel appended to BIOS boot blob at sector $(KERNEL_SEEK)."; \
	else \
	  echo "$(RED)  WARNING: kernel not found at $(KERNEL) -- BIOS path boots ForeB only.$(RESET)"; \
	fi
	@# --- append the sample multiboot module at the BIOS initrd sector ---
	@if [ -f "$(SAMPLE_INITRD)" ]; then \
	  BLOBSIZE=$$(wc -c < /tmp/forebo_boot.img); \
	  NEED=$$(( $(INITRD_SEEK) * 512 )); \
	  if [ "$$BLOBSIZE" -lt "$$NEED" ]; then \
	    dd if=/dev/zero bs=1 count=$$(( NEED - BLOBSIZE )) >> /tmp/forebo_boot.img 2>/dev/null; \
	    cat $(SAMPLE_INITRD) >> /tmp/forebo_boot.img; \
	    echo "  Sample module appended to BIOS boot blob at sector $(INITRD_SEEK)."; \
	  else \
	    echo "$(RED)  WARNING: kernel overruns initrd sector $(INITRD_SEEK); module skipped.$(RESET)"; \
	  fi; \
	fi
	@cp /tmp/forebo_boot.img /tmp/forebo_iso_root/boot/forebo/forebo.img
	@# --- UEFI El Torito boot image: the FAT ESP built by the 'esp' target ---
	@cp $(ESP_IMAGE) /tmp/forebo_iso_root/boot/efi/esp.img
	@cp $(FOREBO_CFG) /tmp/forebo_iso_root/boot/forebo/forebo.cfg 2>/dev/null || true
	@$(XORRISO) -as mkisofs \
	    -V FOREB \
	    -b boot/forebo/forebo.img \
	    -no-emul-boot \
	    -boot-load-size 48 \
	    -boot-info-table \
	    -eltorito-alt-boot \
	    -e boot/efi/esp.img \
	    -no-emul-boot \
	    $(ISOHYBRID_FLAG) \
	    -o $(ISO_IMAGE) \
	    /tmp/forebo_iso_root
	@echo "$(GREEN)  Hybrid ISO created: $(ISO_IMAGE) (BIOS + UEFI)$(RESET)"
	@rm -rf /tmp/forebo_iso_root /tmp/forebo_boot.img

# =============================================================================
# EFI System Partition FAT image (esp.img)
# =============================================================================

esp: $(ESP_IMAGE)

$(ESP_IMAGE): $(EFI_APP) assets $(FOREBO_CFG)
	@echo "$(CYAN)Building EFI System Partition image ($(ESP_IMAGE), $(ESP_SIZE_KIB) KiB)...$(RESET)"
	@rm -f $(ESP_IMAGE)
	$(MKFSFAT) -C -F 16 -n FOREB $(ESP_IMAGE) $(ESP_SIZE_KIB) >/dev/null
	$(MMD) -i $(ESP_IMAGE) ::/EFI ::/EFI/BOOT ::/forebo ::/forebo/icons
	$(MCOPY) -i $(ESP_IMAGE) $(EFI_APP) ::/EFI/BOOT/BOOTX64.EFI
	@echo "  BOOTX64.EFI -> ::/EFI/BOOT/BOOTX64.EFI"
	@if [ -f "$(KERNEL)" ]; then \
	  $(MCOPY) -i $(ESP_IMAGE) $(KERNEL) ::/forebo/kernel.elf; \
	  echo "$(GREEN)  Kernel  -> ::/forebo/kernel.elf$(RESET)"; \
	else \
	  echo "$(RED)  WARNING: kernel not found at $(KERNEL) -- ESP has no kernel.$(RESET)"; \
	fi
	@# --- Config + assets (this upgrade) -----------------------------------
	$(MCOPY) -i $(ESP_IMAGE) $(FOREBO_CFG) ::/forebo/forebo.cfg
	@echo "  forebo.cfg  -> ::/forebo/forebo.cfg"
	@if [ -f "$(ASSET_BG)" ]; then \
	  $(MCOPY) -i $(ESP_IMAGE) $(ASSET_BG) ::/forebo/bg.bmp; \
	  echo "  bg.bmp      -> ::/forebo/bg.bmp"; \
	fi
	@if ls $(ASSETS_DIR)/icons/*.tga >/dev/null 2>&1; then \
	  $(MCOPY) -i $(ESP_IMAGE) $(ASSETS_DIR)/icons/*.tga ::/forebo/icons/; \
	  echo "  icons/*.tga -> ::/forebo/icons/"; \
	fi
	@if [ -f "$(SAMPLE_INITRD)" ]; then \
	  $(MCOPY) -i $(ESP_IMAGE) $(SAMPLE_INITRD) ::/forebo/initrd.tar; \
	  echo "  initrd.tar  -> ::/forebo/initrd.tar"; \
	fi
	@# --- Linux-boot placeholder: if no real EFI-stub vmlinuz is staged, drop a
	@#     note documenting where a 'type=linux' entry expects it. -------------
	@if [ -f "$(VMLINUZ)" ]; then \
	  $(MCOPY) -i $(ESP_IMAGE) $(VMLINUZ) ::/forebo/vmlinuz; \
	  echo "  vmlinuz     -> ::/forebo/vmlinuz"; \
	  if [ -f "$(LINUX_INITRD)" ]; then \
	    $(MCOPY) -i $(ESP_IMAGE) $(LINUX_INITRD) ::/forebo/initrd.img; \
	    echo "  initrd.img  -> ::/forebo/initrd.img"; \
	  fi; \
	else \
	  printf 'ForeB Linux boot: drop an EFI-stub vmlinuz here as \\forebo\\vmlinuz\nand add a `type=linux` entry (see forebo.cfg). ForeB LoadImage/StartImages it\nwith cmdline as LoadOptions and initrd via LINUX_EFI_INITRD_MEDIA (LoadFile2).\nBuild with: make esp VMLINUZ=/boot/vmlinuz LINUX_INITRD=/boot/initrd.img\n' > /tmp/forebo_vmlinuz_readme.txt; \
	  $(MCOPY) -i $(ESP_IMAGE) /tmp/forebo_vmlinuz_readme.txt ::/forebo/vmlinuz.README; \
	  rm -f /tmp/forebo_vmlinuz_readme.txt; \
	  echo "  vmlinuz.README -> ::/forebo/vmlinuz.README (no real vmlinuz staged)"; \
	fi
	@echo "$(GREEN)  ESP image ready: $(ESP_IMAGE)$(RESET)"

# =============================================================================
# Install to real disk (DANGEROUS)
# =============================================================================

install: $(STAGE1_BIN) $(STAGE2_BIN) $(STAGE3_BIN)
	@if [ "$(DISK)" = "$(DISK_IMAGE)" ]; then \
	  echo "$(RED)ERROR: Set DISK= to a real device (e.g. make install DISK=/dev/sdb)$(RESET)"; \
	  exit 1; \
	fi
	@echo "$(RED)WARNING: This will overwrite the MBR of $(DISK)!$(RESET)"
	@echo "Press Ctrl+C to cancel, or Enter to continue..."
	@read dummy
	@echo "$(CYAN)Writing ForeB Stage 1 to MBR of $(DISK)...$(RESET)"
	$(DD) if=$(STAGE1_BIN) of=$(DISK) bs=512 count=1 conv=notrunc
	@echo "  stage2 -> sectors 1..16"
	$(DD) if=$(STAGE2_BIN) of=$(DISK) bs=512 seek=$(STAGE2_SEEK) conv=notrunc
	@echo "  stage3 -> sectors 17..32"
	$(DD) if=$(STAGE3_BIN) of=$(DISK) bs=512 seek=$(STAGE3_SEEK) conv=notrunc
	@sync
	@echo "$(GREEN)ForeB installed to $(DISK).$(RESET)"

# =============================================================================
# QEMU testing
# =============================================================================

qemu: image
	@echo "$(CYAN)Starting QEMU with ForeB disk image (BIOS)...$(RESET)"
	$(QEMU) \
	  -drive file=$(DISK_IMAGE),format=raw,index=0,media=disk \
	  -m 128M -vga std -serial stdio \
	  -name "ForeB - Forest Bootloader"

qemu-iso: iso
	@echo "$(CYAN)Starting QEMU with ForeB hybrid ISO (BIOS)...$(RESET)"
	$(QEMU) \
	  -cdrom $(ISO_IMAGE) \
	  -m 128M -vga std -serial stdio \
	  -name "ForeB ISO - Forest Bootloader"

qemu-debug: image
	@echo "$(CYAN)Starting QEMU in debug mode (GDB stub at localhost:1234)...$(RESET)"
	$(QEMU) \
	  -drive file=$(DISK_IMAGE),format=raw,index=0,media=disk \
	  -m 128M -vga std -serial stdio -no-reboot -s -S \
	  -name "ForeB Debug"

# ----- UEFI (OVMF) -----
qemu-uefi: esp
	@echo "$(CYAN)Starting QEMU (UEFI/OVMF) with esp.img...$(RESET)"
	@cp -f $(OVMF_VARS) $(OVMF_VARS_COPY)
	$(QEMU64) -machine q35 \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(OVMF_VARS_COPY) \
	  -drive format=raw,file=$(ESP_IMAGE) \
	  -device qemu-xhci -device usb-tablet -device usb-mouse -device usb-kbd \
	  -m 256M -serial stdio \
	  -name "ForeB UEFI - esp.img"

qemu-uefi-iso: iso-hybrid
	@echo "$(CYAN)Starting QEMU (UEFI/OVMF) with hybrid ISO...$(RESET)"
	@cp -f $(OVMF_VARS) $(OVMF_VARS_COPY)
	$(QEMU64) -machine q35 \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(OVMF_VARS_COPY) \
	  -cdrom $(ISO_IMAGE) \
	  -device qemu-xhci -device usb-tablet -device usb-mouse -device usb-kbd \
	  -m 256M -serial stdio \
	  -name "ForeB UEFI - hybrid ISO"

# =============================================================================
# Screenshots (dev helper, best-effort)
# =============================================================================
# Boot each firmware path headless in QEMU and grab a framebuffer screenshot of
# the graphical forest menu via the QEMU monitor 'screendump' command (a PPM).
# Handy for eyeballing the GOP-drawn UEFI menu and the re-enabled BIOS menu.
# Requires python3 (used only to talk to the monitor socket). Serial output is
# captured alongside each shot. Override SHOTDIR / SHOT_DELAY as needed.
SHOTDIR    ?= screenshots
SHOT_DELAY ?= 6

screenshots: image esp
	@mkdir -p $(SHOTDIR)
	@cp -f $(OVMF_VARS) $(OVMF_VARS_COPY)
	@echo "$(CYAN)Capturing BIOS menu -> $(SHOTDIR)/foreb-bios.ppm ...$(RESET)"
	@rm -f $(SHOTDIR)/mon-bios.sock
	@$(QEMU) -drive file=$(DISK_IMAGE),format=raw,index=0,media=disk \
	  -m 128M -vga std -display none -no-reboot \
	  -serial file:$(SHOTDIR)/serial-bios.log \
	  -monitor unix:$(SHOTDIR)/mon-bios.sock,server,nowait & \
	  QPID=$$!; sleep $(SHOT_DELAY); \
	  python3 -c "import socket,time; s=socket.socket(socket.AF_UNIX); s.connect('$(SHOTDIR)/mon-bios.sock'); time.sleep(0.3); s.sendall(b'screendump $(SHOTDIR)/foreb-bios.ppm\n'); time.sleep(1.0); s.close()" || true; \
	  sleep 1; kill $$QPID 2>/dev/null; wait $$QPID 2>/dev/null; true
	@echo "$(CYAN)Capturing UEFI menu -> $(SHOTDIR)/foreb-uefi.ppm ...$(RESET)"
	@rm -f $(SHOTDIR)/mon-uefi.sock
	@$(QEMU64) -machine q35 \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(OVMF_VARS_COPY) \
	  -drive format=raw,file=$(ESP_IMAGE) \
	  -device qemu-xhci -device usb-tablet -device usb-mouse -device usb-kbd \
	  -m 256M -display none -no-reboot \
	  -serial file:$(SHOTDIR)/serial-uefi.log \
	  -monitor unix:$(SHOTDIR)/mon-uefi.sock,server,nowait & \
	  QPID=$$!; sleep $(SHOT_DELAY); \
	  python3 -c "import socket,time; s=socket.socket(socket.AF_UNIX); s.connect('$(SHOTDIR)/mon-uefi.sock'); time.sleep(0.3); s.sendall(b'screendump $(SHOTDIR)/foreb-uefi.ppm\n'); time.sleep(1.0); s.close()" || true; \
	  sleep 1; kill $$QPID 2>/dev/null; wait $$QPID 2>/dev/null; true
	@echo "$(GREEN)  Screenshots + serial logs in $(SHOTDIR)/$(RESET)"

# =============================================================================
# Disassembly / inspection
# =============================================================================

disasm: $(STAGE1_BIN) $(STAGE2_BIN) $(STAGE3_BIN)
	@echo "$(CYAN)Disassembling stage1.bin...$(RESET)"
	$(OBJDUMP) -D -b binary -m i8086 -M intel $(STAGE1_BIN) | head -80
	@echo ""
	@echo "$(CYAN)Disassembling stage3.bin (32-bit)...$(RESET)"
	$(OBJDUMP) -D -b binary -m i386 -M intel $(STAGE3_BIN) | head -120

# =============================================================================
# Cleanup
# =============================================================================

clean:
	@echo "$(CYAN)Cleaning ForeB build outputs...$(RESET)"
	@rm -f $(STAGE1_BIN) $(STAGE2_BIN) $(STAGE3_BIN) $(DISK_IMAGE) $(ISO_IMAGE)
	@rm -f stage3_lm.bin
	@rm -f stage1.lst stage2.lst stage3.lst
	@rm -f $(UEFI_OBJ) $(UEFI_UI_OBJ) $(UEFI_MOD_OBJ) $(UEFI_TRAMP_OBJ) $(EFI_APP) $(ESP_IMAGE) $(OVMF_VARS_COPY)
	@rm -f $(EFI_APP_AA64) $(EFI_APP_RISCV) $(ESP_AA64_IMAGE) $(OVMF_AA64_VARS_COPY)
	@rm -rf $(AA64_BUILD) $(RISCV_BUILD) build
	@rm -rf $(SHOTDIR) $(ASSETS_DIR)
	@echo "$(GREEN)Clean complete.$(RESET)"

# =============================================================================
# Host tools: C++ installer + Qt6 customizer GUI (out-of-tree CMake builds)
# =============================================================================
.PHONY: tools forb-install forb-customizer

tools: forb-install forb-customizer

forb-install:
	@echo "$(CYAN)Building forb-install (C++ installer)...$(RESET)"
	@cd tools/forb-install && cmake -B build >/dev/null && cmake --build build -j
	@echo "$(GREEN)  -> tools/forb-install/build/forb-install (self-contained)$(RESET)"

forb-customizer:
	@echo "$(CYAN)Building forb-customizer (Qt6 GUI)...$(RESET)"
	@cd tools/forb-customizer && cmake -B build -DCMAKE_PREFIX_PATH=$$(qmake6 -query QT_INSTALL_PREFIX) >/dev/null && cmake --build build -j
	@echo "$(GREEN)  -> tools/forb-customizer/build/forb-customizer$(RESET)"

# forb-mkrescue: self-contained bootable ForeB rescue image (drop-in for
# grub-mkrescue). Rebuilds the embedded payload from the current BOOTX64.EFI.
.PHONY: rescue
rescue: $(EFI_APP) forb-install
	@cd tools/forb-install && cmake --build build -j >/dev/null
	@tools/forb-install/build/forb-mkrescue -o $(CURDIR)/forebo-rescue.iso -v
	@echo "$(GREEN)  -> forebo-rescue.iso (UEFI-bootable CD/USB)$(RESET)"

# =============================================================================
# Help
# =============================================================================

help:
	@echo "$(CYAN)ForeB - Forest Bootloader Build System (v2.0, BIOS + UEFI)$(RESET)"
	@echo ""
	@echo "$(GREEN)Build:$(RESET)"
	@echo "  all          Build BIOS stages (stage1/2/3) AND BOOTX64.EFI"
	@echo "  uefi         Build BOOTX64.EFI only (clang + ld.lld)"
	@echo "  check        Verify BIOS binary sizes + MBR/stage2 signatures"
	@echo "  disasm       Disassemble stages (requires objdump)"
	@echo ""
	@echo "$(GREEN)Images:$(RESET)"
	@echo "  image        Build forebo.img (stage1+2+3 + kernel [+initrd])"
	@echo "  esp          Build esp.img (FAT ESP: EFI + kernel + cfg + assets + module)"
	@echo "  assets       Generate assets/bg.bmp + icons/*.tga + sample initrd.tar"
	@echo "  iso          Build hybrid forebo.iso (BIOS + UEFI)  [= iso-hybrid]"
	@echo "  iso-hybrid   Build dual-boot ISO (BIOS El Torito + UEFI ESP)"
	@echo "  iso-bios     Build legacy BIOS-only El Torito ISO"
	@echo ""
	@echo "$(GREEN)QEMU:$(RESET)"
	@echo "  qemu           Run BIOS disk image in QEMU"
	@echo "  qemu-iso       Run hybrid ISO in QEMU (BIOS path)"
	@echo "  qemu-debug     Run BIOS image with GDB stub (localhost:1234)"
	@echo "  qemu-uefi      Run BOOTX64.EFI via OVMF using esp.img"
	@echo "  qemu-uefi-iso  Run hybrid ISO under OVMF (UEFI path)"
	@echo "  screenshots    Headless-boot both paths + grab menu PPMs (needs python3)"
	@echo ""
	@echo "$(GREEN)Cross-arch UEFI (aarch64 + riscv64):$(RESET)"
	@echo "  uefi-aa64      Build BOOTAA64.EFI (aarch64 PE, fully bootable)"
	@echo "  uefi-riscv     Compile riscv64 objects + print PE packaging boundary"
	@echo "  uefi-all       Build x64 + aa64 PE, then riscv objects (never fails)"
	@echo "  esp-aa64       Build esp-aa64.img (FAT ESP with BOOTAA64.EFI + cfg/assets)"
	@echo "  qemu-aa64      Boot BOOTAA64.EFI under aa64 OVMF (-M virt -cpu cortex-a57)"
	@echo "  qemu-riscv     Print the riscv64 run line (PE not producible yet)"
	@echo ""
	@echo "$(GREEN)Install:$(RESET)"
	@echo "  install DISK=/dev/sdX   Install to real disk (DANGER!)"
	@echo ""
	@echo "$(GREEN)Variables:$(RESET)"
	@echo "  KERNEL=/path/to/kernel.bin   Kernel ELF/binary for image/ESP/ISO"
	@echo "  INITRD=/path/to/initrd.tar   Optional (BIOS multiboot module)"
	@echo "  VMLINUZ=/boot/vmlinuz        EFI-stub Linux kernel for 'type=linux'"
	@echo "  LINUX_INITRD=/boot/initrd.img  Initrd for the Linux entry (LoadFile2)"
	@echo "  NASMFLAGS='-DFOREB_DEFAULT_WIDTH=1280 ...'  Override config.h"
	@echo ""
	@echo "$(GREEN)Disk layout (LBA sectors, 512 B each):$(RESET)"
	@echo "  0       stage1 (MBR)"
	@echo "  1-16    stage2 (8 KiB)"
	@echo "  17-32   stage3 (8 KiB)"
	@echo "  48+     kernel ELF"
