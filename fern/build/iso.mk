# =============================================================================
# BOOTABLE IMAGE PLUMBING (kernel artifact + initrd)  —  boot via foreboots
# =============================================================================
# This fragment owns the kernel/initrd output paths and the initrd tarball rule,
# and delegates the bootable-image (`iso`/`img`) targets to foreboots (ForeB),
# THE Forest-OS bootloader. GRUB has been removed entirely — there is no
# grub.cfg emission and no grub-mkrescue ISO here anymore.
#
# Honors:
#   - BOOT_MODE   (bios|uefi)   -> selects OUTPUT (fern.bin vs BOOTX64.EFI)
#
# Depends on (defined earlier by other fragments / the main Makefile):
#   - OUTDIR, DISTDIR, INITRD_DIR                    (build/dirs.mk)
#   - ARCH, BOOT_MODE, BUILD_TYPE                    (build/config.mk)
#   - OK_COLOR / NO_COLOR                            (build/flags.mk region)
#   - forebo-image phony target                      (build/foreb.mk, included AFTER this file)
#
# The main Makefile keeps the actual compile/link rules; this file declares the
# kernel/initrd outputs, builds the initrd tarball, and points iso/img at ForeB.

# -----------------------------------------------------------------------------
# Output binary + boot-file paths (defined here if not already set earlier)
# -----------------------------------------------------------------------------
ifeq ($(BOOT_MODE),uefi)
    ifndef OUTPUT_ELF
    OUTPUT_ELF := $(OUTDIR)/fern.elf
    endif
    ifndef OUTPUT
    OUTPUT := $(OUTDIR)/BOOTX64.EFI
    endif
else
    ifndef OUTPUT
    OUTPUT := $(OUTDIR)/boot/fern.bin
    endif
endif

ifndef INITRD
INITRD := $(OUTDIR)/boot/initrd.tar
endif

ifndef INITRD_FILES
INITRD_FILES := $(shell find $(INITRD_DIR) -type f 2>/dev/null)
endif

# Build the initrd tarball from the $(INITRD_DIR) tree. The Fern kernel still
# loads it, and ForeB embeds an initrd in its disk image. Rebuilds whenever any
# file under initrd/ changes.
$(INITRD): $(INITRD_FILES)
	@mkdir -p $(dir $@)
	@echo "$(OK_COLOR)Building initrd: $@ (from $(INITRD_DIR)/)$(NO_COLOR)"
	@tar -C $(INITRD_DIR) -cf $@ .

# -----------------------------------------------------------------------------
# Bootable image targets — delegated to foreboots (ForeB).
# ForeB is BOTH the raw-MBR/BIOS bootloader and the UEFI/ESP path; forebo-image
# (build/foreb.mk) builds ForeB and embeds $(OUTPUT) as the Fern kernel, so both
# `iso` and `img` route through the same target. `make run` boots the resulting
# ForeB disk image (see build/qemu-run.mk), never a GRUB CD.
# -----------------------------------------------------------------------------
.PHONY: iso img
iso: forebo-image
img: forebo-image
