# =============================================================================
# DIRECTORY STRUCTURE
# =============================================================================
# This fragment is included AFTER build/config.mk, so ARCH/BOOT_MODE/BUILD_TYPE
# are already defined. Do NOT redefine them here.

REPO_ROOT := $(abspath $(CURDIR))
SRCDIR := src
# Use "Nbit" suffix for x86 arches; use plain name for arm/aarch64
ifeq ($(filter arm aarch64,$(ARCH)),)
    ARCH_DIR_SUFFIX := $(ARCH)bit
else
    ARCH_DIR_SUFFIX := $(ARCH)
endif
OBJDIR := obj/$(ARCH_DIR_SUFFIX)-$(BOOT_MODE)-$(BUILD_TYPE)
OUTDIR := build/$(ARCH_DIR_SUFFIX)-$(BOOT_MODE)-$(BUILD_TYPE)
DISTDIR := dist
TOOLSDIR := tools

# Source directories
USER_SRCDIR := userspace
INITRD_DIR := initrd
LIBC_DIR := $(abspath $(CURDIR)/../../libs/libc)
FORESTCORE_DIR := libs/forestcore
UACPI_SRCDIR := libs/uacpi/source

# Output directories
EFIDIR := $(OUTDIR)/EFI/BOOT
INITRD_BIN_DIR := $(INITRD_DIR)/bin
INITRD_USR_BIN_DIR := $(INITRD_DIR)/usr/bin
