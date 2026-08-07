# =============================================================================
# FOREB BOOTLOADER TARGETS (build/foreb.mk)
# =============================================================================
# Owns the `forebo*` phony targets (moved out of the main Makefile).
#
# The whole subsystem is gated on ENABLE_FOREB_BOOTLOADER (a `process` bool
# from build-config.mk; guaranteed `yes`/`no` by build/config.mk).
#
#   ENABLE_FOREB_BOOTLOADER=yes  -> build/run ForeB via `$(MAKE) -C $(FOREBO_DIR)`
#   ENABLE_FOREB_BOOTLOADER=no   -> every `forebo*` target prints a warning
#                                  explaining how to enable it and exits 1.
#
# Depends on (defined by earlier includes / main Makefile body):
#   REPO_ROOT   (build/dirs.mk)
#   OUTPUT      (main Makefile OUTPUT FILES section)
#   QEMU_MEMORY (build/config.mk, int; default 512)
#   ENABLE_FOREB_BOOTLOADER (build/config.mk, bool)
#   Color vars INFO_COLOR/OK_COLOR/WARN_COLOR/ERROR_COLOR/NO_COLOR (main Makefile)

.PHONY: forebo forebo-image forebo-qemu forebo-check forebo-clean

# ForeB source directory
FOREBO_DIR := $(REPO_ROOT)/foreboots

# ForeB kernel path follows the configured arch/boot/build output.
# `?=` so a caller may override on the command line. Deferred expansion means
# $(OUTPUT) need not be defined at include time — only when a recipe runs.
#
# MUST be absolute: the recipes below invoke `$(MAKE) -C $(FOREBO_DIR)`, so a
# relative $(OUTPUT) (e.g. build/32bit-bios-debug/boot/kernel.bin) would resolve
# against foreboots/ in the sub-make and silently fail to embed the kernel.
# $(abspath ...) is a no-op for already-absolute overrides.
FOREBO_KERNEL ?= $(abspath $(OUTPUT))

# QEMU memory (MiB) for ForeB test runs; follows build-config.mk QEMU_MEMORY.
FOREBO_QEMU_MEMORY ?= $(QEMU_MEMORY)

# -----------------------------------------------------------------------------
# Enabled path
# -----------------------------------------------------------------------------
ifeq ($(ENABLE_FOREB_BOOTLOADER),yes)

# Build ForeB stage1 and stage2 binaries
forebo:
	@echo "$(INFO_COLOR)Building ForeB - Forest Bootloader...$(NO_COLOR)"
	@if ! command -v nasm >/dev/null 2>&1; then \
		echo "$(ERROR_COLOR)NASM assembler not found. Install: sudo apt install nasm$(NO_COLOR)"; \
		exit 1; \
	fi
	@$(MAKE) -C $(FOREBO_DIR) all
	@echo "$(OK_COLOR)ForeB built successfully in $(FOREBO_DIR)/$(NO_COLOR)"

# Build ForeB + create a raw disk image (stage1 + stage2 + kernel)
forebo-image: forebo
	@echo "$(INFO_COLOR)Creating ForeB disk image...$(NO_COLOR)"
	@if [ -f "$(FOREBO_KERNEL)" ]; then \
		$(MAKE) -C $(FOREBO_DIR) image KERNEL=$(FOREBO_KERNEL); \
	else \
		echo "$(WARN_COLOR)Kernel not found at $(FOREBO_KERNEL). Build the OS first.$(NO_COLOR)"; \
		echo "$(INFO_COLOR)Creating image without kernel (ForeB only)...$(NO_COLOR)"; \
		$(MAKE) -C $(FOREBO_DIR) image; \
	fi

# Test ForeB in QEMU (pass configured KERNEL path + QEMU memory through)
forebo-qemu: forebo-image
	@echo "$(INFO_COLOR)Launching ForeB in QEMU...$(NO_COLOR)"
	@$(MAKE) -C $(FOREBO_DIR) qemu KERNEL=$(FOREBO_KERNEL) QEMU_MEMORY=$(FOREBO_QEMU_MEMORY)

# Verify ForeB binary sizes and MBR signature
forebo-check: forebo
	@$(MAKE) -C $(FOREBO_DIR) check
	@$(MAKE) -C $(FOREBO_DIR) check-mbr

# Clean ForeB build outputs
forebo-clean:
	@echo "$(INFO_COLOR)Cleaning ForeB build outputs...$(NO_COLOR)"
	@$(MAKE) -C $(FOREBO_DIR) clean

# -----------------------------------------------------------------------------
# Disabled path — warn and exit so users know to enable the config option
# -----------------------------------------------------------------------------
else

forebo forebo-image forebo-qemu forebo-check forebo-clean:
	@echo "$(WARN_COLOR)ForeB bootloader is disabled: ENABLE_FOREB_BOOTLOADER is not 'yes'.$(NO_COLOR)"
	@echo "$(INFO_COLOR)Enable it via:  ./conf.sh --menuconfig   (set ENABLE_FOREB_BOOTLOADER=y)$(NO_COLOR)"
	@echo "$(INFO_COLOR)then refresh:    ./conf.sh --generate     (writes build-config.mk)$(NO_COLOR)"
	@exit 1

endif
