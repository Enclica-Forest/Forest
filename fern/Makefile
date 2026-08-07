# =============================================================================
# FOREST OS ADVANCED MULTIARCH BUILD SYSTEM
# =============================================================================
# Thin orchestrator. All configuration, directory layout, toolchain detection,
# flags, source selection, feature gating, userspace, QEMU, ISO/IMG, ForeB and
# cleanup logic live in build/*.mk (see the include block below). This file
# keeps only the compile/link pattern rules and the top-level targets that are
# NOT owned by any fragment.
#
# Configuration is driven by build-config.mk (generated from .forestos_config
# by conf.sh). Use:
#   make menuconfig / make defconfig / make oldconfig   -> invoke conf.sh
#   make configcheck / make show-config                 -> inspect effective config
#   bare make                                           -> build per build-config.mk
# =============================================================================

.DEFAULT_GOAL := help

# =============================================================================
# INCLUDES (order per build-spec.md)
# =============================================================================
include build/config.mk
include build/dirs.mk
include build/toolchain.mk
include build/flags.mk
include build/kernel-sources.mk
include build/features/memory.mk
include build/features/filesystems.mk
include build/features/graphics.mk
include build/features/opengl.mk
include build/features/networking.mk
include build/features/audio.mk
include build/features/usb.mk
include build/features/storage.mk
include build/features/input.mk
include build/features/ipc.mk
include build/features/timers.mk
include build/features/interrupts.mk
include build/features/security.mk
include build/features/hardware.mk
include build/features/scheduler.mk
include build/features/compat.mk
include build/iso.mk
include build/foreb.mk
include build/qemu-run.mk
include build/clean.mk

# =============================================================================
# DERIVED ARCH TUPLES (not owned by any fragment)
# =============================================================================
# build/config.mk owns ARCH; build/toolchain.mk owns FORESTOS_TOOLCHAIN_PREFIX.
# The friendly target tuple / arch name below are used by show-config / info.
ifeq ($(ARCH),32)
    TARGET_ARCH  := i386
    TARGET_TUPLE := i686-forestos
else ifeq ($(ARCH),64)
    TARGET_ARCH  := x86_64
    TARGET_TUPLE := x86_64-forestos
else ifeq ($(ARCH),arm)
    TARGET_ARCH  := arm
    TARGET_TUPLE := arm-none-eabi
else ifeq ($(ARCH),aarch64)
    TARGET_ARCH  := aarch64
    TARGET_TUPLE := aarch64-linux-gnu
else ifeq ($(ARCH),riscv64)
    TARGET_ARCH  := riscv64
    TARGET_TUPLE := riscv64-unknown-elf
endif

# =============================================================================
# PHONY TARGET DECLARATIONS (rules owned by this file)
# =============================================================================
.PHONY: help help-advanced all build show-config \
        ensure-toolchain validate-toolchain toolchain toolchain-setup \
        build32 build64 buildarm buildaarch64 buildriscv64 \
        build32-bios build32-uefi build64-bios build64-uefi \
        buildaarch64-bios buildaarch64-uefi buildriscv64-bios buildriscv64-uefi \
        buildall crossarcinterpret \
        dist dist-clean \
        info list-objects size stats \
        menuconfig defconfig oldconfig allnoconfig allyesconfig configcheck

# =============================================================================
# HELP
# =============================================================================
help:
	@echo "$(INFO_COLOR)Forest OS Build System (config-driven)$(NO_COLOR)"
	@echo ""
	@echo "$(OK_COLOR)Current config:$(NO_COLOR)  ARCH=$(ARCH) BOOT_MODE=$(BOOT_MODE) BUILD_TYPE=$(BUILD_TYPE)"
	@echo "  (from build-config.mk; override with ARCH= BOOT_MODE= BUILD_TYPE= on the CLI)"
	@echo ""
	@echo "$(OK_COLOR)Configuration targets:$(NO_COLOR)"
	@echo "  menuconfig    Interactive config TUI (./conf.sh --menuconfig)"
	@echo "  defconfig     Write sane defaults (.forestos_config + build-config.mk)"
	@echo "  oldconfig     Re-validate existing .forestos_config + regenerate"
	@echo "  allnoconfig   All features off (except required-on)"
	@echo "  allyesconfig  All features on"
	@echo "  configcheck   Validate build-config.mk + print effective config"
	@echo "  show-config   Full effective config dump"
	@echo ""
	@echo "$(OK_COLOR)Main targets:$(NO_COLOR)"
	@echo "  all           Build kernel + bootable image (iso or img)"
	@echo "  build         Build kernel binary only"
	@echo "  iso           Create ISO image (BIOS)"
	@echo "  img           Create disk image (UEFI)"
	@echo "  dist          Create distribution package"
	@echo "  clean         Clean build files for current config"
	@echo ""
	@echo "$(OK_COLOR)Toolchain:$(NO_COLOR)        toolchain (build cross-gcc for current ARCH) / validate-toolchain"
	@echo "$(OK_COLOR)ForeB bootloader:$(NO_COLOR)  forebo / forebo-image / forebo-qemu / forebo-check / forebo-clean"
	@echo "$(OK_COLOR)Multi-arch:$(NO_COLOR)       build32 build64 buildarm buildaarch64 buildriscv64 buildall crossarcinterpret"
	@echo "$(OK_COLOR)Run in QEMU:$(NO_COLOR)      run run-bios run-uefi run32 run64 runarm runaarch64 debug"
	@echo ""
	@echo "$(OK_COLOR)Examples:$(NO_COLOR)"
	@echo "  make defconfig && make            # sane defaults, then build"
	@echo "  make menuconfig                   # tweak config interactively"
	@echo "  make ARCH=64 BOOT_MODE=uefi       # CLI override (wins this run)"
	@echo "  make help-advanced                # detailed help"

# =============================================================================
# CONFIG TARGETS (invoke conf.sh)
# =============================================================================
menuconfig: ; @./conf.sh --menuconfig
defconfig:  ; @./conf.sh --defconfig
oldconfig:  ; @./conf.sh --oldconfig
allnoconfig: ; @./conf.sh --allnoconfig
allyesconfig: ; @./conf.sh --allyesconfig

# configcheck: delegates to build/config.mk's configcheck-internal (which prints
# the effective core choices + enabled feature count). conf.sh has no --validate
# sub-command, so we rely on configcheck-internal alone.
configcheck: configcheck-internal

# =============================================================================
# MAIN BUILD TARGETS
# =============================================================================
# CLEAN_BEFORE_BUILD hook: when CLEAN_BEFORE_BUILD=yes (process option from
# build-config.mk) build/clean.mk's maybe-clean-before-build runs `make clean`
# before any compile; otherwise it is a silent no-op.
all: maybe-clean-before-build ensure-toolchain show-config build
ifeq ($(BOOT_MODE),uefi)
	@$(MAKE) img
else
	@$(MAKE) iso
endif

build: maybe-clean-before-build ensure-toolchain $(OUTPUT)

# =============================================================================
# SHOW-CONFIG — comprehensive effective configuration dump
# =============================================================================
show-config:
	@echo "$(INFO_COLOR)===== Forest-OS effective build configuration =====$(NO_COLOR)"
	@echo "  ARCH            = $(ARCH)  ($(TARGET_ARCH))"
	@echo "  BOOT_MODE       = $(BOOT_MODE)"
	@echo "  BUILD_TYPE      = $(BUILD_TYPE)"
	@echo "  TARGET_TUPLE    = $(TARGET_TUPLE)"
	@echo "  TOOLCHAIN       = $(FORESTOS_TOOLCHAIN_PREFIX)"
	@echo "  OBJDIR          = $(OBJDIR)"
	@echo "  OUTDIR          = $(OUTDIR)"
	@echo "  OUTPUT          = $(OUTPUT)"
	@echo "  PARALLEL_JOBS   = $(PARALLEL_JOBS)"
	@echo ""
	@echo "$(INFO_COLOR)--- core process/runtime options ---$(NO_COLOR)"
	@echo "  CLEAN_BEFORE_BUILD   = $(CLEAN_BEFORE_BUILD)"
	@echo "  VERBOSE              = $(VERBOSE)"
	@echo "  GENERATE_CHECKSUMS   = $(GENERATE_CHECKSUMS)"
	@echo "  ENABLE_TESTING       = $(ENABLE_TESTING)"
	@echo "  ROOT_AUTOLOGIN       = $(ROOT_AUTOLOGIN)"
	@echo "  QEMU_MEMORY          = $(QEMU_MEMORY)"
	@echo "  QEMU_ENABLE_KVM      = $(QEMU_ENABLE_KVM)"
	@echo "  QEMU_NETWORK         = $(QEMU_NETWORK)"
	@echo "  QEMU_USB             = $(QEMU_USB)"
	@echo "  ENABLE_FOREB_BOOTLOADER = $(ENABLE_FOREB_BOOTLOADER)"
	@echo ""
	@echo "$(INFO_COLOR)--- key feature states ---$(NO_COLOR)"
	@echo "  ENABLE_VFS           = $(ENABLE_VFS)"
	@echo "  ENABLE_GRAPHICS      = $(ENABLE_GRAPHICS)"
	@echo "  ENABLE_NETWORKING    = $(ENABLE_NETWORKING)"
	@echo "  ENABLE_AUDIO         = $(ENABLE_AUDIO)"
	@echo "  ENABLE_USB           = $(ENABLE_USB)"
	@echo "  ENABLE_PCI           = $(ENABLE_PCI)"
	@echo "  ENABLE_ACPI          = $(ENABLE_ACPI)"
	@echo "  ENABLE_PAGING        = $(ENABLE_PAGING)"
	@echo "  ENABLE_SMP           = $(ENABLE_SMP)"
	@echo "  ENABLE_ELF_LOADER    = $(ENABLE_ELF_LOADER)"
	@echo ""
	@echo "$(INFO_COLOR)--- feature totals ---$(NO_COLOR)"
	@echo "  Enabled feature booleans: $(words $(foreach o,$(ALL_BOOL_OPTIONS),$(if $(filter yes,$($(o))),x))) / $(words $(ALL_BOOL_OPTIONS))"
	@echo "  FEATURE_FLAGS = $(FEATURE_FLAGS)"
	@echo "  build-config.mk present: $(if $(wildcard build-config.mk),yes,no)"

# =============================================================================
# TOOLCHAIN VALIDATION
# =============================================================================
.PHONY: ensure-toolchain validate-toolchain
ensure-toolchain: validate-toolchain
	@echo "$(OK_COLOR)Toolchain validation passed for $(FORESTOS_TOOLCHAIN_PREFIX)$(NO_COLOR)"

validate-toolchain:
	@echo "$(INFO_COLOR)Validating Forest OS toolchain...$(NO_COLOR)"
	@if [ "$(ARCH)" = "arm" ] || [ "$(ARCH)" = "aarch64" ] || [ "$(ARCH)" = "riscv64" ]; then \
		echo "$(INFO_COLOR)ARM/AArch64/RISC-V build: using system cross-compiler $(FORESTOS_TOOLCHAIN_PREFIX)$(NO_COLOR)"; \
	elif [ "$(FORESTOS_TOOLCHAIN_HAS_64BIT)" = "false" ]; then \
		echo "$(WARN_COLOR)Using fallback host toolchain for 64-bit builds$(NO_COLOR)"; \
		echo "$(INFO_COLOR)Using toolchain: $(FORESTOS_TOOLCHAIN_PREFIX)$(NO_COLOR)"; \
	else \
		if [ ! -d "$(FORESTOS_TOOLCHAIN_DIR)" ]; then \
			echo "$(ERROR_COLOR)Toolchain directory not found: $(FORESTOS_TOOLCHAIN_DIR)$(NO_COLOR)"; \
			echo "$(ERROR_COLOR)Please install Forest OS toolchain (see docs/DEVELOPMENT_GUIDES.md)$(NO_COLOR)"; \
			exit 1; \
		fi; \
		if [ ! -d "$(TOOLCHAIN_ARCH_DIR)" ]; then \
			echo "$(ERROR_COLOR)Architecture toolchain not found: $(TOOLCHAIN_ARCH_DIR)$(NO_COLOR)"; \
			echo "$(ERROR_COLOR)Please build the $(FORESTOS_TOOLCHAIN_PREFIX) toolchain: run 'make toolchain'$(NO_COLOR)"; \
			echo "$(INFO_COLOR)  (or ARCH=$(ARCH) make toolchain; see forestos-toolchain/README.md)$(NO_COLOR)"; \
			exit 1; \
		fi; \
	fi
	@if ! command -v $(CC) >/dev/null 2>&1 && [ ! -x "$(CC)" ]; then \
		echo "$(ERROR_COLOR)Required compiler '$(CC)' not found in PATH.$(NO_COLOR)"; \
		exit 1; \
	fi
	@if ! command -v $(LD) >/dev/null 2>&1 && [ ! -x "$(LD)" ]; then \
		echo "$(ERROR_COLOR)Required linker '$(LD)' not found in PATH.$(NO_COLOR)"; \
		exit 1; \
	fi
	@if ! command -v $(AS) >/dev/null 2>&1 && [ ! -x "$(AS)" ]; then \
		echo "$(ERROR_COLOR)Required assembler '$(AS)' not found in PATH.$(NO_COLOR)"; \
		exit 1; \
	fi
	@echo "$(OK_COLOR)Using toolchain: $(FORESTOS_TOOLCHAIN_PREFIX)$(NO_COLOR)"
	@echo "$(INFO_COLOR)GCC version: $(NO_COLOR)"
	@$(CC) --version | head -1
	@echo "$(INFO_COLOR)LD version: $(NO_COLOR)"
	@$(LD) --version | head -1

# Build the Forest OS cross-toolchain from source for the configured ARCH.
# Delegates to the canonical consolidated builder in the toolchain source
# package. Honors FORESTOS_TOOLCHAIN_DIR (so overrides / the symlink both work)
# and forwards the current ARCH. Extra flags via TOOLCHAIN_ARGS, e.g.:
#   make toolchain ARCH=64
#   make toolchain TOOLCHAIN_ARGS="--jobs 4 --clean"
#   make toolchain TOOLCHAIN_ARGS="--arch both"
TOOLCHAIN_SCRIPT := $(FORESTOS_TOOLCHAIN_DIR)/build-toolchain.sh
# Map kernel ARCH (32/64/arm/aarch64) to the builder's --arch (32/64 only).
ifeq ($(ARCH),64)
    TOOLCHAIN_BUILD_ARCH := 64
else
    TOOLCHAIN_BUILD_ARCH := 32
endif

.PHONY: toolchain
toolchain:
	@if [ "$(ARCH)" != "32" ] && [ "$(ARCH)" != "64" ]; then \
		echo "$(WARN_COLOR)ARCH=$(ARCH) has no Forest OS cross-toolchain; building for $(TOOLCHAIN_BUILD_ARCH)-bit instead.$(NO_COLOR)"; \
		echo "$(INFO_COLOR)(ARM/AArch64 use a system cross-compiler, not this builder.)$(NO_COLOR)"; \
	fi
	@if [ ! -x "$(TOOLCHAIN_SCRIPT)" ]; then \
		echo "$(ERROR_COLOR)Toolchain build script not found or not executable:$(NO_COLOR)"; \
		echo "$(ERROR_COLOR)  $(TOOLCHAIN_SCRIPT)$(NO_COLOR)"; \
		echo "$(INFO_COLOR)Set FORESTOS_TOOLCHAIN_DIR to the toolchain source package, e.g.$(NO_COLOR)"; \
		echo "$(INFO_COLOR)  make toolchain FORESTOS_TOOLCHAIN_DIR=$(REPO_ROOT)/../forestos-toolchain$(NO_COLOR)"; \
		exit 1; \
	fi
	@echo "$(INFO_COLOR)Building Forest OS cross-toolchain (arch=$(TOOLCHAIN_BUILD_ARCH)) via $(TOOLCHAIN_SCRIPT)$(NO_COLOR)"
	"$(TOOLCHAIN_SCRIPT)" --arch $(TOOLCHAIN_BUILD_ARCH) $(TOOLCHAIN_ARGS)
	@echo "$(OK_COLOR)Toolchain build finished. Verify with: make validate-toolchain$(NO_COLOR)"

.PHONY: toolchain-setup
toolchain-setup:
	@echo "$(INFO_COLOR)Forest OS Toolchain Setup$(NO_COLOR)"
	@echo "$(WARN_COLOR)This script helps set up the cross-compiler toolchain.$(NO_COLOR)"
	@echo ""
	@echo "1. Install build dependencies:"
	@echo "   sudo apt install build-essential libgmp-dev libmpfr-dev libmpc-dev texinfo"
	@echo ""
	@echo "2. Choose toolchain source:"
	@echo "   a) Download prebuilt toolchain"
	@echo "   b) Build from source (requires more time)"
	@echo ""
	@echo "3. Toolchain directory: $(FORESTOS_TOOLCHAIN_DIR)"
	@echo ""
	@echo "See docs/DEVELOPMENT_GUIDES.md for detailed instructions."

# =============================================================================
# KERNEL BUILD RULES (link)
# =============================================================================

# UEFI kernel build
ifeq ($(BOOT_MODE),uefi)

# AArch64 UEFI: link only UEFI stub + shared UEFI modules (no bare-metal boot)
ifeq ($(ARCH),aarch64)
UEFI_LINK_OBJECTS := $(AARCH64_UEFI_OBJECTS)
# RISC-V UEFI: link only UEFI stub + shared UEFI modules (no bare-metal boot)
else ifeq ($(ARCH),riscv64)
UEFI_LINK_OBJECTS := $(RISCV64_UEFI_OBJECTS)
else
UEFI_LINK_OBJECTS := $(ALL_OBJECTS)
endif

$(OUTPUT): $(OUTPUT_ELF)
	@mkdir -p $(EFIDIR)
	@echo "$(OK_COLOR)Converting ELF to UEFI PE32+ binary...$(NO_COLOR)"
	@$(OBJCOPY) -j .multiboot -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel* \
	            -j .rela* -j .reloc -j .bss -j .stack --target=efi-app-$(EFI_ARCH) $< $@
	@echo "$(OK_COLOR)UEFI kernel binary generated: $@$(NO_COLOR)"

$(OUTPUT_ELF): $(UEFI_LINK_OBJECTS)
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Linking UEFI ELF binary...$(NO_COLOR)"
	@$(LD) $(LDFLAGS) -o $@ $^ $(LIBGCC)

# BIOS kernel build
else
$(OUTPUT): $(ALL_OBJECTS)
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Linking BIOS kernel ELF...$(NO_COLOR)"
	@$(LD) $(LDFLAGS) -o $@ $^ $(LIBGCC)
	@echo "$(OK_COLOR)Kernel ELF binary generated: $@$(NO_COLOR)"
endif

# =============================================================================
# OBJECT FILE BUILD RULES
# =============================================================================

# Special interrupt handling compilation
$(OBJDIR)/interrupt.o: $(SRCDIR)/interrupt.c
	@mkdir -p $(OBJDIR)
	@echo "$(OK_COLOR)Compiling (interrupt) $<...$(NO_COLOR)"
	@$(CC) $(INTERRUPT_CFLAGS) -c -o $@ $<

$(OBJDIR)/interrupt_handlers.o: $(SRCDIR)/interrupt_handlers.c
	@mkdir -p $(OBJDIR)
	@echo "$(OK_COLOR)Compiling (interrupt) $<...$(NO_COLOR)"
	@$(CC) $(INTERRUPT_CFLAGS) -c -o $@ $<

# uACPI compilation
$(OBJDIR)/uacpi_%.o: $(UACPI_SRCDIR)/%.c
	@mkdir -p $(OBJDIR)
	@echo "$(OK_COLOR)Compiling (uACPI) $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Standard C compilation
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Canopy C compilation (handles subdirectories)
$(OBJDIR)/canopy/%.o: $(SRCDIR)/canopy/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling Canopy $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/canopy/render/%.o: $(SRCDIR)/canopy/render/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling Canopy Render $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/canopy/compositor/%.o: $(SRCDIR)/canopy/compositor/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling Canopy Compositor $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/canopy/wm/%.o: $(SRCDIR)/canopy/wm/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling Canopy WM $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/canopy/de/%.o: $(SRCDIR)/canopy/de/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling Canopy DE $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/canopy/theme/%.o: $(SRCDIR)/canopy/theme/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling Canopy Theme $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/canopy/widgets/%.o: $(SRCDIR)/canopy/widgets/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling Canopy Widgets $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/canopy/apps/%.o: $(SRCDIR)/canopy/apps/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling Canopy Apps $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Input subsystem compilation
$(OBJDIR)/input/%.o: $(SRCDIR)/input/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling Input subsystem $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Assembly compilation
$(OBJDIR)/%.o: $(SRCDIR)/%.asm
	@mkdir -p $(OBJDIR)
	@echo "$(OK_COLOR)Assembling $<...$(NO_COLOR)"
	@$(NASM) $(NASMFLAGS) -o $@ $<

$(OBJDIR)/%.o: $(SRCDIR)/%.s
	@mkdir -p $(OBJDIR)
	@echo "$(OK_COLOR)Assembling $<...$(NO_COLOR)"
	@$(AS) $(ASFLAGS) -o $@ $<

# Disambiguate the dual interrupt stub sources by architecture so the correct
# NASM file is assembled when both .asm and .s variants exist.
ifeq ($(ARCH),64)
$(OBJDIR)/interrupt_stubs.o: $(SRCDIR)/interrupt_stubs.s
	@mkdir -p $(OBJDIR)
	@echo "$(OK_COLOR)Assembling $< with NASM...$(NO_COLOR)"
	@$(NASM) $(NASMFLAGS) -f elf64 -o $@ $<
else
$(OBJDIR)/interrupt_stubs.o: $(SRCDIR)/interrupt_stubs.asm
	@mkdir -p $(OBJDIR)
	@echo "$(OK_COLOR)Assembling $<...$(NO_COLOR)"
	@$(NASM) $(NASMFLAGS) -o $@ $<
endif

# Graphics subdirectory compilation
$(OBJDIR)/graphics/%.o: $(SRCDIR)/graphics/%.c
	@mkdir -p $(OBJDIR)/graphics
	@echo "$(OK_COLOR)Compiling graphics $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/graphics/drivers/%.o: $(SRCDIR)/graphics/drivers/%.c
	@mkdir -p $(OBJDIR)/graphics/drivers
	@echo "$(OK_COLOR)Compiling graphics driver $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# GL subsystem compilation
$(OBJDIR)/gl/%.o: $(SRCDIR)/gl/%.c
	@mkdir -p $(OBJDIR)/gl
	@echo "$(OK_COLOR)Compiling GL $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Boot mode specific compilation
$(OBJDIR)/uefi/%.o: $(SRCDIR)/uefi/%.c
	@mkdir -p $(OBJDIR)/uefi
	@echo "$(OK_COLOR)Compiling UEFI $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/bios/%.o: $(SRCDIR)/bios/%.c
	@mkdir -p $(OBJDIR)/bios
	@echo "$(OK_COLOR)Compiling BIOS $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# USB subsystem compilation
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling USB $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Sound driver requiring SSE for floating point
$(OBJDIR)/sound_sb16.o: $(SRCDIR)/sound_sb16.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling sound_sb16 $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -msse -c -o $@ $<

$(OBJDIR)/sound_sb16_test.o: $(SRCDIR)/sound_sb16_test.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling sound_sb16_test $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -msse -c -o $@ $<

$(OBJDIR)/stb_vorbis_port.o: $(SRCDIR)/stb_vorbis_port.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling stb_vorbis_port $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -msse -c -o $@ $<

$(OBJDIR)/timer_calibration.o: $(SRCDIR)/timer_calibration.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling timer_calibration $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -msse -c -o $@ $<

$(OBJDIR)/graphics/enhanced_cursor.o: $(SRCDIR)/graphics/enhanced_cursor.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling enhanced_cursor $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -msse -c -o $@ $<

# Hardware drivers compilation
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling hardware driver $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# ARM32-specific source compilation (src/arm32/*.c and *.S)
$(OBJDIR)/arm32/%.o: $(SRCDIR)/arm32/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling ARM32 $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/arm32/%.o: $(SRCDIR)/arm32/%.S
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Assembling ARM32 $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# AArch64-specific source compilation (src/aarch64/*.c and *.S)
$(OBJDIR)/aarch64/%.o: $(SRCDIR)/aarch64/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling AArch64 $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/aarch64/%.o: $(SRCDIR)/aarch64/%.S
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Assembling AArch64 $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Cross-architecture interpreter compilation (src/crossarcinterpret/*.c)
$(OBJDIR)/crossarcinterpret/%.o: $(SRCDIR)/crossarcinterpret/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling crossarcinterpret $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Cross-architecture heap wrapper compilation (src/arch/*.c)
$(OBJDIR)/arch/%.o: $(SRCDIR)/arch/%.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling arch wrapper $<...$(NO_COLOR)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# QR Code generator library
$(OBJDIR)/qrcodegen.o: $(QRCODEGEN_DIR)/qrcodegen.c
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Compiling QR code library $<...$(NO_COLOR)"
	@$(CC) $(LIB_CFLAGS) -c -o $@ $<

# =============================================================================
# MULTI-ARCHITECTURE BUILD TARGETS
# =============================================================================
# These pass ARCH=/BOOT_MODE= on the command line, which overrides
# build-config.mk for that recursive make invocation (intended for explicit
# per-arch build targets).

build32:
	@$(MAKE) ARCH=32 build

build64:
	@$(MAKE) ARCH=64 build

# ARMv7 build (BIOS boot mode is a placeholder; ARM uses its own boot)
buildarm:
	@$(MAKE) ARCH=arm BOOT_MODE=bios BUILD_TYPE=$(BUILD_TYPE) build

# AArch64 build (BIOS)
buildaarch64:
	@$(MAKE) ARCH=aarch64 BOOT_MODE=bios BUILD_TYPE=$(BUILD_TYPE) build

# AArch64 BIOS build (explicit)
buildaarch64-bios:
	@$(MAKE) ARCH=aarch64 BOOT_MODE=bios BUILD_TYPE=$(BUILD_TYPE) all

# AArch64 UEFI build
buildaarch64-uefi:
	@$(MAKE) ARCH=aarch64 BOOT_MODE=uefi BUILD_TYPE=$(BUILD_TYPE) all

# RISC-V 64-bit build
buildriscv64:
	@$(MAKE) ARCH=riscv64 BOOT_MODE=bios BUILD_TYPE=$(BUILD_TYPE) build

# RISC-V 64-bit BIOS build (explicit)
buildriscv64-bios:
	@$(MAKE) ARCH=riscv64 BOOT_MODE=bios BUILD_TYPE=$(BUILD_TYPE) all

# RISC-V 64-bit UEFI build
buildriscv64-uefi:
	@$(MAKE) ARCH=riscv64 BOOT_MODE=uefi BUILD_TYPE=$(BUILD_TYPE) all

build32-bios:
	@$(MAKE) ARCH=32 BOOT_MODE=bios all

build32-uefi:
	@$(MAKE) ARCH=32 BOOT_MODE=uefi all

build64-bios:
	@$(MAKE) ARCH=64 BOOT_MODE=bios all

build64-uefi:
	@$(MAKE) ARCH=64 BOOT_MODE=uefi all

buildall: build32-bios build32-uefi build64-bios build64-uefi buildarm buildaarch64 buildaarch64-uefi buildriscv64 buildriscv64-uefi
	@echo "$(OK_COLOR)All architecture combinations built successfully!$(NO_COLOR)"

# Cross-architecture interpreter: compile src/crossarcinterpret/*.c and link
# with the kernel. Honors ENABLE_CROSSARC_INTERPRETER (per build/features/
# scheduler.mk note): bail out early with a clear message when disabled.
crossarcinterpret: ensure-toolchain $(ALL_OBJECTS) $(CROSSARC_OBJECTS)
ifeq ($(ENABLE_CROSSARC_INTERPRETER),no)
	@echo "$(WARN_COLOR)ENABLE_CROSSARC_INTERPRETER=no: cross-architecture interpreter is disabled.$(NO_COLOR)"
	@echo "$(INFO_COLOR)Enable it via ./conf.sh --menuconfig then ./conf.sh --generate.$(NO_COLOR)"
	@exit 1
else
	@mkdir -p $(OUTDIR)/boot
	@echo "$(OK_COLOR)Building crossarcinterpret kernel variant...$(NO_COLOR)"
	@$(LD) $(LDFLAGS) -o $(OUTDIR)/boot/kernel-crossarc.bin \
	       $(ALL_OBJECTS) $(CROSSARC_OBJECTS) $(LIBGCC)
	@echo "$(OK_COLOR)crossarcinterpret kernel: $(OUTDIR)/boot/kernel-crossarc.bin$(NO_COLOR)"
endif

# =============================================================================
# DISTRIBUTION PACKAGING
# =============================================================================

dist: buildall
	@echo "$(INFO_COLOR)Creating distribution package...$(NO_COLOR)"
	@mkdir -p $(DISTDIR)/forestos-complete/
	@echo "$(OK_COLOR)Copying binaries...$(NO_COLOR)"
	@cp -r build/* $(DISTDIR)/forestos-complete/
	@echo "$(OK_COLOR)Creating distribution archive...$(NO_COLOR)"
	@cd $(DISTDIR) && tar -czf forestos-complete-$(shell date +%Y%m%d_%H%M%S).tar.gz forestos-complete/
	@echo "$(OK_COLOR)Distribution package created in $(DISTDIR)/$(NO_COLOR)"

dist-clean:
	@echo "$(OK_COLOR)Cleaning distribution files...$(NO_COLOR)"
	@rm -rf $(DISTDIR)

# =============================================================================
# UTILITY TARGETS
# =============================================================================

info:
	@echo "$(INFO_COLOR)Forest OS Build System Information$(NO_COLOR)"
	@echo "Repository: $(REPO_ROOT)"
	@echo "Toolchain:  $(FORESTOS_TOOLCHAIN_PREFIX)"
	@echo "Target:     $(TARGET_TUPLE)"
	@echo "Objects:    $(OBJDIR)"
	@echo "Output:     $(OUTDIR)"
	@echo "Kernel bin: $(OUTPUT)"

list-objects:
	@echo "$(INFO_COLOR)Object files that will be built:$(NO_COLOR)"
	@echo "$(ALL_OBJECTS)" | tr ' ' '\n' | sort

size: $(OUTPUT)
	@echo "$(INFO_COLOR)Binary size information:$(NO_COLOR)"
	@ls -lh $(OUTPUT)
	@echo ""
	@$(STRIP) --version >/dev/null 2>&1 && \
		echo "Stripped size:" && \
		$(STRIP) -s $(OUTPUT) -o $(OUTPUT).stripped && \
		ls -lh $(OUTPUT).stripped && \
		rm -f $(OUTPUT).stripped || true

stats:
	@echo "$(INFO_COLOR)Build statistics:$(NO_COLOR)"
	@echo "C source files:   $(words $(CSOURCES) $(GRAPHICS_SRCS) $(PANICUI_SRCS))"
	@echo "Assembly files:   $(words $(ASM_SRCS))"
	@echo "Object files:     $(words $(ALL_OBJECTS))"
	@echo "uACPI sources:    $(words $(UACPI_SRCS))"
	@echo "Excluded sources: $(words $(EXCLUDED_CSOURCES))"

# =============================================================================
# ADVANCED HELP
# =============================================================================
help-advanced:
	@echo "$(INFO_COLOR)Forest OS Advanced Build System - Detailed Help$(NO_COLOR)"
	@echo ""
	@echo "$(OK_COLOR)Build System Features:$(NO_COLOR)"
	@echo "  • Multi-architecture support (32-bit, 64-bit, ARMv7, AArch64, RISC-V 64)"
	@echo "  • BIOS and UEFI boot mode support"
	@echo "  • Multiple build types (debug, release, optimize)"
	@echo "  • Cross-compilation toolchain integration"
	@echo "  • Distribution packaging"
	@echo "  • Config-driven feature gating (build/features/*.mk)"
	@echo ""
	@echo "$(OK_COLOR)Configuration flow:$(NO_COLOR)"
	@echo "  .forestos_config  --(conf.sh --generate)--> build-config.mk"
	@echo "  build-config.mk   --(build/config.mk)-----> Make vars (ARCH, ENABLE_*, ...)"
	@echo "  Bare make uses build-config.mk; CLI ARCH=/BOOT_MODE= override still wins."
	@echo ""
	@echo "$(OK_COLOR)Directory Structure:$(NO_COLOR)"
	@echo "  obj/           - Object files (per configuration)"
	@echo "  build/         - Built kernels (per configuration) + build/*.mk fragments"
	@echo "  dist/          - Distribution packages"
	@echo ""
	@echo "$(OK_COLOR)Configuration Examples:$(NO_COLOR)"
	@echo "  make defconfig                       # sane defaults"
	@echo "  make menuconfig                      # tweak interactively"
	@echo "  make configcheck                     # validate + show effective config"
	@echo "  make ARCH=32 BOOT_MODE=bios BUILD_TYPE=debug"
	@echo "  make ARCH=64 BOOT_MODE=uefi BUILD_TYPE=release"
	@echo "  make ARCH=arm  BUILD_TYPE=debug"
	@echo "  make ARCH=aarch64 BUILD_TYPE=debug"
	@echo "  make ARCH=riscv64 BUILD_TYPE=debug"
	@echo "  make buildall                        # all combinations"
	@echo "  make buildarm                        # ARMv7 kernel"
	@echo "  make buildaarch64                    # AArch64 kernel (BIOS)"
	@echo "  make buildaarch64-uefi               # AArch64 kernel (UEFI)"
	@echo "  make buildriscv64                    # RISC-V 64 kernel (BIOS)"
	@echo "  make buildriscv64-uefi               # RISC-V 64 kernel (UEFI)"
	@echo "  make run64                           # QEMU q35/qemu64"
	@echo "  make runarm                          # QEMU virt/cortex-a15"
	@echo "  make runaarch64                      # QEMU virt/cortex-a53"
	@echo "  make crossarcinterpret               # crossarcinterpret variant"
	@echo "  make dist                            # distribution package"

# End of Makefile
