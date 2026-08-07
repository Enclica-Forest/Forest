# =============================================================================
# ISO / IMG (BOOTABLE IMAGE) CREATION
# =============================================================================
# This fragment owns the bootable-image build rules. It is included after
# build/userspace.mk (#7) and before build/foreb.mk (#10).
#
# Honors:
#   - GRUB_TIMEOUT          (config int; default 5)  -> injected into grub.cfg
#   - ENABLE_FOREB_BOOTLOADER (yes|no)              -> ForeB path when yes+bios
#   - BOOT_MODE             (bios|uefi)             -> selects OUTPUT / recipe
#
# Depends on (defined earlier by other fragments / the main Makefile):
#   - OUTDIR, GRUBDIR, DISTDIR, INITRD_DIR          (build/dirs.mk)
#   - ARCH, BOOT_MODE, BUILD_TYPE                   (build/config.mk)
#   - OK_COLOR / WARN_COLOR / NO_COLOR / INFO_COLOR (build/flags.mk region)
#   - $(INITRD) file rule                          (main Makefile body)
#   - forebo-image phony target                    (build/foreb.mk, included AFTER this file)
#
# The main Makefile keeps the actual compile/link/initrd rules; this file only
# declares the bootable-image outputs and the iso/img packaging recipes.

# -----------------------------------------------------------------------------
# Output binary + boot-file paths (defined here if not already set earlier)
# -----------------------------------------------------------------------------
ifeq ($(BOOT_MODE),uefi)
    ifndef OUTPUT_ELF
    OUTPUT_ELF := $(OUTDIR)/kernel.elf
    endif
    ifndef OUTPUT
    OUTPUT := $(OUTDIR)/BOOTX64.EFI
    endif
else
    ifndef OUTPUT
    OUTPUT := $(OUTDIR)/boot/kernel.bin
    endif
endif

ifndef GRUB_CFG
GRUB_CFG := $(GRUBDIR)/grub.cfg
endif

ifndef INITRD
INITRD := $(OUTDIR)/boot/initrd.tar
endif

ifndef INITRD_FILES
INITRD_FILES := $(shell find $(INITRD_DIR) -type f 2>/dev/null)
endif

# Build the initrd tarball from the $(INITRD_DIR) tree (kernel loads it as a
# multiboot module). Rebuilds whenever any file under initrd/ changes.
$(INITRD): $(INITRD_FILES)
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Building initrd: $@ (from $(INITRD_DIR)/)$(NO_COLOR)"
	@tar -C $(INITRD_DIR) -cf $@ .

# Distribution file names (timestamped once via := guarded by ifndef so the
# shell `date` is not re-evaluated on every reference).
ifndef ISO_NAME
ISO_NAME := forestos_$(ARCH)bit_$(BOOT_MODE)_$(BUILD_TYPE)_$(shell date +%Y%m%d_%H%M%S).iso
endif
ifndef ISO
ISO := $(DISTDIR)/$(ISO_NAME)
endif

ifndef IMG_NAME
IMG_NAME := forestos_$(ARCH)bit_$(BOOT_MODE)_$(BUILD_TYPE)_$(shell date +%Y%m%d_%H%M%S).img
endif
ifndef IMG
IMG := $(DISTDIR)/$(IMG_NAME)
endif

.PHONY: iso img

# -----------------------------------------------------------------------------
# GRUB config copy rule (used by the BIOS GRUB path)
# -----------------------------------------------------------------------------
$(GRUB_CFG): Grub/grub.cfg
	@mkdir -p $(GRUBDIR)
	@echo "$(OK_COLOR)Copying GRUB config...$(NO_COLOR)"
	@cp $< $@

# -----------------------------------------------------------------------------
# `iso` phony target — exactly ONE definition is emitted below depending on
# (ENABLE_FOREB_BOOTLOADER, BOOT_MODE):
#   - yes + bios  -> ForeB path (recipe here, delegates to forebo-image)
#   - anything else -> GRUB path (no recipe; depends on $(ISO) file rule)
# Make allows a target to be declared with prerequisites multiple times, but
# only ONE declaration may carry a recipe — the branching below guarantees that.
# -----------------------------------------------------------------------------
# The distributable `.iso` is ALWAYS a real GRUB El Torito image (ISO9660,
# browsable + BIOS/CD bootable). ForeB is a raw-MBR *disk* bootloader and can
# never be a CD ISO, so it is offered separately via `make forebo-image` /
# `make run` (a raw disk image), not as the .iso.
iso: ensure-toolchain $(ISO)

# -----------------------------------------------------------------------------
# $(ISO) file rule — conditional on BOOT_MODE.
# In BIOS mode the GRUB rule is skipped when ForeB is enabled (the ForeB `iso`
# recipe above already produces $(ISO) directly).
# -----------------------------------------------------------------------------
ifeq ($(BOOT_MODE),bios)

# BIOS ISO creation (GRUB El Torito hybrid). Stage a clean ISO tree containing
# just the kernel, initrd and grub.cfg, then let grub-mkrescue build a hybrid
# image bootable as CD (-cdrom / -boot d) AND as USB/HDD (isohybrid MBR), and
# openable in any archive manager as ISO9660.
GRUB_ISO_ROOT := $(OUTDIR)/grub_iso_root
$(ISO): $(OUTPUT) $(INITRD) Grub/grub.cfg
	@mkdir -p $(DISTDIR) $(GRUB_ISO_ROOT)/boot/grub
	@echo "$(OK_COLOR)Staging GRUB ISO tree...$(NO_COLOR)"
	@cp $(OUTPUT) $(GRUB_ISO_ROOT)/boot/kernel.bin
	@cp $(INITRD) $(GRUB_ISO_ROOT)/boot/initrd.tar
	@cp Grub/grub.cfg $(GRUB_ISO_ROOT)/boot/grub/grub.cfg
	@echo "$(OK_COLOR)Building BIOS GRUB hybrid ISO...$(NO_COLOR)"
	@grub-mkrescue -o $@ $(GRUB_ISO_ROOT)
	@echo "$(OK_COLOR)ISO created: $@$(NO_COLOR)"

else
# UEFI ISO creation (always uses GRUB, even when ForeB is enabled — ForeB is
# BIOS-only). The grub.cfg menu below is generated with the configured
# GRUB_TIMEOUT instead of a hard-coded value.
img: ensure-toolchain $(ISO)

$(ISO): $(OUTPUT) $(INITRD)
	@mkdir -p $(DISTDIR) $(OUTDIR)/iso_root
	@echo "$(OK_COLOR)Creating UEFI ISO...$(NO_COLOR)"
	@# Create EFI directory structure
	@mkdir -p $(OUTDIR)/iso_root/EFI/BOOT
	@echo "Copying EFI file: $(OUTPUT) -> $(OUTDIR)/iso_root/EFI/BOOT/BOOTX64.EFI"
	@cp $(OUTPUT) $(OUTDIR)/iso_root/EFI/BOOT/BOOTX64.EFI
	@-cp $(INITRD) $(OUTDIR)/iso_root/ 2>/dev/null || true
	@# Create startup script for EFI shell
	@echo 'fs0:' > $(OUTDIR)/iso_root/startup.nsh
	@echo 'EFI\BOOT\BOOTX64.EFI' >> $(OUTDIR)/iso_root/startup.nsh
	@# Create GRUB directory structure for BIOS boot
	@mkdir -p $(OUTDIR)/iso_root/boot/grub/i386-pc
	@# Copy GRUB boot images and modules from system installation
	@cp /usr/lib/grub/i386-pc/*.img $(OUTDIR)/iso_root/boot/grub/i386-pc/ 2>/dev/null || true
	@cp /usr/lib/grub/i386-pc/*.lst $(OUTDIR)/iso_root/boot/grub/i386-pc/ 2>/dev/null || true
	@cp /usr/lib/grub/i386-pc/*.mod $(OUTDIR)/iso_root/boot/grub/i386-pc/ 2>/dev/null || true
	@cp /usr/lib/grub/i386-pc/*.lst $(OUTDIR)/iso_root/boot/grub/i386-pc/ 2>/dev/null || true
	@cp /usr/lib/grub/i386-pc/lzma_decompress.img $(OUTDIR)/iso_root/boot/grub/i386-pc/ 2>/dev/null || true
	@cp /usr/lib/grub/i386-pc/boot.img $(OUTDIR)/iso_root/boot/grub/i386-pc/ 2>/dev/null || true
	@# Copy kernel and initrd for GRUB
	@cp $(if $(filter uefi,$(BOOT_MODE)),$(OUTPUT_ELF),$(OUTPUT)) $(OUTDIR)/iso_root/boot/kernel.elf
	@-cp $(INITRD) $(OUTDIR)/iso_root/boot/ 2>/dev/null || true
	@# Create GRUB configuration — timeout is driven by GRUB_TIMEOUT (config int)
	@echo 'set timeout=$(GRUB_TIMEOUT)' > $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo 'set default=0' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo 'menuentry "Forest OS (BIOS)" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    set gfxpayload=keep' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '    module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '}' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo 'submenu "Resolution selection" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "1920x1080" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=1920x1080x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "1600x900" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=1600x900x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "1366x768" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=1366x768x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "1280x720" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=1280x720x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "1024x768" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=1024x768x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "3840x2160 (4K UHD)" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=3840x2160x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "2560x1440 (QHD)" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=2560x1440x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "1920x1200" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=1920x1200x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "1680x1050" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=1680x1050x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "1440x900" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=1440x900x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "1280x1024" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=1280x1024x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "1280x800" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=1280x800x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "1152x864" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=1152x864x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "1024x600" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=1024x600x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "800x600" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=800x600x32' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    menuentry "Auto (Fallback)" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        set gfxpayload=keep' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '        multiboot2 /boot/kernel.elf' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '        module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '    }' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '}' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo 'menuentry "Forest OS (Quiet Splash)" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    set gfxpayload=keep' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kernel.elf quiet' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@if [ -f $(INITRD) ]; then \
		echo '    module2 /boot/initrd.tar' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg; \
	fi
	@echo '}' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo 'menuentry "Forest OS (UEFI)" {' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '    chainloader /EFI/BOOT/BOOTX64.EFI' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@echo '}' >> $(OUTDIR)/iso_root/boot/grub/grub.cfg
	@ls -la $(OUTDIR)/iso_root/EFI/BOOT/
	@ls -la $(OUTDIR)/iso_root/boot/
	@# Create the ISO with GRUB for BIOS boot
	@if command -v grub-mkrescue >/dev/null 2>&1; then \
		grub-mkrescue -o $@ $(OUTDIR)/iso_root; \
	elif command -v xorriso >/dev/null 2>&1; then \
		xorriso -as mkisofs -o $@ -iso-level 3 \
			-eltorito-boot boot/grub/i386-pc/eltorito.img -no-emul-boot \
			-boot-load-size 4 -boot-info-table -volid "FOREST_OS" $(OUTDIR)/iso_root; \
	elif command -v mkisofs >/dev/null 2>&1; then \
		mkisofs -o $@ -b boot/grub/i386-pc/eltorito.img -no-emul-boot \
			-boot-load-size 4 -boot-info-table -volid "FOREST_OS" $(OUTDIR)/iso_root; \
	else \
		echo "$(WARN_COLOR)Warning: No ISO creation tools found$(NO_COLOR)"; \
		tar -cf $@ -C $(OUTDIR)/iso_root .; \
	fi
	@rm -rf $(OUTDIR)/iso_root
	@echo "$(OK_COLOR)UEFI ISO created: $@$(NO_COLOR)"
endif
