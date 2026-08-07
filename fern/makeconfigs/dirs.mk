# =============================================================================
# makeconfigs/dirs.mk — Directory layout
# =============================================================================
# Included AFTER config.mk, so ARCH/BOOT_MODE/BUILD_TYPE are already defined.
# Do NOT redefine them here. Owns every path variable the rest of the build
# references: source/output dirs, arch suffix, and the derived kernel OUTPUT.
# =============================================================================

REPO_ROOT := $(abspath $(CURDIR))
SRCDIR    := src
DISTDIR   := dist
TOOLSDIR  := tools

# Use "Nbit" suffix for x86 arches; plain name for arm/aarch64.
ifeq ($(filter arm aarch64,$(ARCH)),)
    ARCH_DIR_SUFFIX := $(ARCH)bit
else
    ARCH_DIR_SUFFIX := $(ARCH)
endif

OBJDIR := obj/$(ARCH_DIR_SUFFIX)-$(BOOT_MODE)-$(BUILD_TYPE)
OUTDIR := build/$(ARCH_DIR_SUFFIX)-$(BOOT_MODE)-$(BUILD_TYPE)

# Source directories
USER_SRCDIR    := userspace
INITRD_DIR     := initrd
LIBC_DIR       := libs/libc
FORESTCORE_DIR := libs/forestcore
UACPI_SRCDIR   := libs/uacpi/source
QRCODEGEN_DIR  := libs/qrcodegen
LEAFGFX_DIR    := libs/leafgfx

# Output directories
GRUBDIR           := $(OUTDIR)/boot/grub
EFIDIR            := $(OUTDIR)/EFI/BOOT
INITRD_BIN_DIR    := $(INITRD_DIR)/bin
INITRD_USR_BIN_DIR := $(INITRD_DIR)/usr/bin

# -----------------------------------------------------------------------------
# Derived kernel output paths
# -----------------------------------------------------------------------------
# UEFI: link an intermediate ELF, then objcopy to a PE32+ EFI binary in EFIDIR.
# BIOS: link straight to the final kernel.bin loaded by GRUB/ForeB.
ifeq ($(BOOT_MODE),uefi)
    OUTPUT_ELF := $(OUTDIR)/boot/kernel.elf
    OUTPUT     := $(EFIDIR)/kernel.efi
else
    OUTPUT_ELF :=
    OUTPUT     := $(OUTDIR)/boot/kernel.bin
endif

# End of makeconfigs/dirs.mk
