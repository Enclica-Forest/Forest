# =============================================================================
# makeconfigs/toolchain.mk — Per-arch toolchain detection
# =============================================================================
# Owns: FORESTOS_TOOLCHAIN_DIR, FORESTOS_TOOLCHAIN_PREFIX, per-arch
# CC/CXX/LD/AS/NASM/OBJCOPY/STRIP/NM, TOOLCHAIN_ARCH_DIR,
# FORESTOS_TOOLCHAIN_HAS_64BIT, LIBGCC detection, GENFW/SPLIT (UEFI).
# Relies on config.mk (ARCH/BOOT_MODE) and dirs.mk (REPO_ROOT).
# =============================================================================

FORESTOS_TOOLCHAIN_DIR ?= $(REPO_ROOT)/forestos-toolchain

# -----------------------------------------------------------------------------
# Per-architecture toolchain selection
# -----------------------------------------------------------------------------
ifeq ($(ARCH),32)
    FORESTOS_TOOLCHAIN_PREFIX := i686-forestos
    TOOLCHAIN_ARCH_DIR := $(FORESTOS_TOOLCHAIN_DIR)/install
    ARCH_FLAGS    := -m32 -march=i386 -mtune=i386
    ARCH_LDFLAGS  := -m elf_i386
    EFI_ARCH      := i386
else ifeq ($(ARCH),64)
    FORESTOS_TOOLCHAIN_PREFIX := x86_64-forestos
    TOOLCHAIN_ARCH_DIR := $(FORESTOS_TOOLCHAIN_DIR)/install

    # Check if Forest OS 64-bit toolchain exists; fall back to host x86_64.
    ifeq ($(wildcard $(TOOLCHAIN_ARCH_DIR)/bin/$(FORESTOS_TOOLCHAIN_PREFIX)-gcc),)
        $(info Forest OS 64-bit toolchain not found - using host x86_64 toolchain)
        FORESTOS_TOOLCHAIN_PREFIX := x86_64-linux-gnu
        TOOLCHAIN_ARCH_DIR :=
        CC := gcc
        CXX := g++
        LD := ld
        AR := ar
        STRIP := strip
        OBJCOPY := objcopy
        OBJDUMP := objdump
        READELF := readelf
        SIZE := size
        FORESTOS_TOOLCHAIN_HAS_64BIT := false
    else
        FORESTOS_TOOLCHAIN_HAS_64BIT := true
    endif

    ARCH_FLAGS    := -m64 -march=x86-64 -mcmodel=kernel
    ARCH_LDFLAGS  := -m elf_x86_64
    EFI_ARCH      := x86_64
else ifeq ($(ARCH),arm)
    # ARMv7 cross-compiler detection
    TOOLCHAIN_ARCH_DIR :=
    ifneq ($(shell which arm-none-eabi-gcc 2>/dev/null),)
        FORESTOS_TOOLCHAIN_PREFIX := arm-none-eabi
        CC := arm-none-eabi-gcc
        CXX := arm-none-eabi-g++
        LD := arm-none-eabi-ld
        AS := arm-none-eabi-as
        OBJCOPY := arm-none-eabi-objcopy
        STRIP := arm-none-eabi-strip
    else ifneq ($(shell which arm-linux-gnueabi-gcc 2>/dev/null),)
        FORESTOS_TOOLCHAIN_PREFIX := arm-linux-gnueabi
        CC := arm-linux-gnueabi-gcc
        CXX := arm-linux-gnueabi-g++
        LD := arm-linux-gnueabi-ld
        AS := arm-linux-gnueabi-as
        OBJCOPY := arm-linux-gnueabi-objcopy
        STRIP := arm-linux-gnueabi-strip
    else ifneq ($(shell which arm-linux-gnueabihf-gcc 2>/dev/null),)
        FORESTOS_TOOLCHAIN_PREFIX := arm-linux-gnueabihf
        CC := arm-linux-gnueabihf-gcc
        CXX := arm-linux-gnueabihf-g++
        LD := arm-linux-gnueabihf-ld
        AS := arm-linux-gnueabihf-as
        OBJCOPY := arm-linux-gnueabihf-objcopy
        STRIP := arm-linux-gnueabihf-strip
    else
        $(warning No ARM cross-compiler found. Install arm-none-eabi-gcc or arm-linux-gnueabi-gcc)
        FORESTOS_TOOLCHAIN_PREFIX := arm-none-eabi
        CC := arm-none-eabi-gcc
        CXX := arm-none-eabi-g++
        LD := arm-none-eabi-ld
        AS := arm-none-eabi-as
        OBJCOPY := arm-none-eabi-objcopy
        STRIP := arm-none-eabi-strip
    endif
    NASM :=
    FORESTOS_TOOLCHAIN_HAS_64BIT := false
    ARCH_FLAGS    := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=softfp -marm
    ARCH_LDFLAGS  := -m armelf
    EFI_ARCH      := arm
else ifeq ($(ARCH),aarch64)
    TOOLCHAIN_ARCH_DIR :=
    ifneq ($(shell which aarch64-linux-gnu-gcc 2>/dev/null),)
        FORESTOS_TOOLCHAIN_PREFIX := aarch64-linux-gnu
        CC := aarch64-linux-gnu-gcc
        CXX := aarch64-linux-gnu-g++
        LD := aarch64-linux-gnu-ld
        AS := aarch64-linux-gnu-as
        OBJCOPY := aarch64-linux-gnu-objcopy
        STRIP := aarch64-linux-gnu-strip
    else ifneq ($(shell which aarch64-none-elf-gcc 2>/dev/null),)
        FORESTOS_TOOLCHAIN_PREFIX := aarch64-none-elf
        CC := aarch64-none-elf-gcc
        CXX := aarch64-none-elf-g++
        LD := aarch64-none-elf-ld
        AS := aarch64-none-elf-as
        OBJCOPY := aarch64-none-elf-objcopy
        STRIP := aarch64-none-elf-strip
    else
        $(warning No AArch64 cross-compiler found. Install aarch64-linux-gnu-gcc)
        FORESTOS_TOOLCHAIN_PREFIX := aarch64-linux-gnu
        CC := aarch64-linux-gnu-gcc
        CXX := aarch64-linux-gnu-g++
        LD := aarch64-linux-gnu-ld
        AS := aarch64-linux-gnu-as
        OBJCOPY := aarch64-linux-gnu-objcopy
        STRIP := aarch64-linux-gnu-strip
    endif
    NASM :=
    FORESTOS_TOOLCHAIN_HAS_64BIT := false
    ARCH_FLAGS    := -march=armv8-a
    ARCH_LDFLAGS  := -m aarch64elf
    EFI_ARCH      := aarch64
endif

# -----------------------------------------------------------------------------
# x86/x86_64 toolchain selection (ARM/AArch64 set their tools above)
# -----------------------------------------------------------------------------
ifeq ($(filter arm aarch64,$(ARCH)),)
    ifeq ($(FORESTOS_TOOLCHAIN_HAS_64BIT),false)
        CC := gcc
        CXX := g++
        LD := ld
        AS := as
        NASM := nasm
        OBJCOPY ?= objcopy
        STRIP ?= strip
    else
        CC := $(TOOLCHAIN_ARCH_DIR)/bin/$(FORESTOS_TOOLCHAIN_PREFIX)-gcc
        CXX := $(TOOLCHAIN_ARCH_DIR)/bin/$(FORESTOS_TOOLCHAIN_PREFIX)-g++
        LD := $(TOOLCHAIN_ARCH_DIR)/bin/$(FORESTOS_TOOLCHAIN_PREFIX)-ld
        AS := $(TOOLCHAIN_ARCH_DIR)/bin/$(FORESTOS_TOOLCHAIN_PREFIX)-as
        NASM := nasm
        OBJCOPY ?= $(TOOLCHAIN_ARCH_DIR)/bin/$(FORESTOS_TOOLCHAIN_PREFIX)-objcopy
        STRIP ?= $(TOOLCHAIN_ARCH_DIR)/bin/$(FORESTOS_TOOLCHAIN_PREFIX)-strip
    endif
endif
NASM ?= nasm

ifeq ($(TOOLCHAIN_ARCH_DIR),)
    NM := $(FORESTOS_TOOLCHAIN_PREFIX)-nm
else
    NM := $(TOOLCHAIN_ARCH_DIR)/bin/$(FORESTOS_TOOLCHAIN_PREFIX)-nm
endif

# -----------------------------------------------------------------------------
# libgcc.a for soft-float support (freestanding floating-point helpers)
# -----------------------------------------------------------------------------
LIBGCC_PATH := $(shell $(CC) -print-libgcc-file-name 2>/dev/null)
ifeq ($(shell test -f "$(LIBGCC_PATH)" && echo yes),yes)
    LIBGCC := $(LIBGCC_PATH)
else
    LIBGCC :=
endif

# -----------------------------------------------------------------------------
# UEFI host tools (only needed in uefi boot mode)
# -----------------------------------------------------------------------------
ifeq ($(BOOT_MODE),uefi)
    GENFW := GenFw
    SPLIT := split
endif

# End of makeconfigs/toolchain.mk
