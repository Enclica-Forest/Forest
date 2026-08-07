# =============================================================================
# BUILD FLAGS CONFIGURATION  (extracted from Makefile lines 313-478)
# =============================================================================
# This fragment is included AFTER build/toolchain.mk.
# Inputs (guaranteed by build/config.mk + build/toolchain.mk):
#   ARCH, BOOT_MODE, BUILD_TYPE  : from build-config.mk
#   FEATURE_FLAGS                : from build/config.mk (feature -D defines)
#   numeric tunables (VFS_MAX_PATH, ...) : from build/config.mk (?= defaults)
#   ARCH_FLAGS, ARCH_LDFLAGS     : from build/toolchain.mk
#   SRCDIR, OBJDIR               : from build/dirs.mk
# Outputs:
#   COMMON_CFLAGS, CFLAGS, LDFLAGS, INTERRUPT_CFLAGS
#   LINKER_SCRIPT, BOOT_ASM, BOOT_OBJ, BOOT_OBJECTS
#   NASMFLAGS, ASFLAGS
#   NO_COLOR, OK_COLOR, ERROR_COLOR, WARN_COLOR, INFO_COLOR

# Common flags
COMMON_CFLAGS := $(ARCH_FLAGS) -ffreestanding -nostdlib -fno-pic -fno-pie \
                 -Wall -Wextra -I$(SRCDIR)/include -Ilibs/uacpi/include \
                 -Ilibs/qrcodegen \
                 -fcf-protection=none

# Treat warnings as errors (gated by CONFIG_WERROR process bool)
ifeq ($(WERROR),yes)
    COMMON_CFLAGS += -Werror
endif

# Architecture-specific flags
ifeq ($(ARCH),32)
    # 32-bit: Strict i386 compatibility, use x87 FPU for kernel floating-point paths
    COMMON_CFLAGS += -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-3dnow \
                      -D__i386__ -D__32BIT__ -mfpmath=387
else ifeq ($(ARCH),64)
    # 64-bit: x86_64 with kernel memory model, no SIMD in kernel
    COMMON_CFLAGS += -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 \
                      -D__x86_64__ -D__64BIT__ -mno-80387 -msoft-float
else ifeq ($(ARCH),arm)
    # ARMv7: NEON for FPU, softfp ABI for freestanding builds
    COMMON_CFLAGS += -D__ARM__ -D__ARM32__ -D__arm__ -mno-unaligned-access
else ifeq ($(ARCH),aarch64)
    # AArch64: no special SIMD exclusions needed for a basic kernel port
    COMMON_CFLAGS += -D__AARCH64__ -D__aarch64__
endif

# Boot mode specific flags
ifeq ($(BOOT_MODE),uefi)
    COMMON_CFLAGS += -DUEFI_BOOT -fshort-wchar
else
    COMMON_CFLAGS += -DBIOS_BOOT
endif

# Autologin flag (for debugging/testing)
ifeq ($(ROOT_AUTOLOGIN),yes)
    COMMON_CFLAGS += -DENABLE_ROOT_AUTOLOGIN
endif

# Include all feature flags from build-config.mk
COMMON_CFLAGS += $(FEATURE_FLAGS)

# Numeric configuration values from build-config.mk
COMMON_CFLAGS += -DVFS_MAX_PATH=$(VFS_MAX_PATH)
COMMON_CFLAGS += -DVFS_MAX_OPEN_FILES=$(VFS_MAX_OPEN_FILES)
COMMON_CFLAGS += -DVFS_MAX_MOUNTS=$(VFS_MAX_MOUNTS)
COMMON_CFLAGS += -DDISPLAY_DEFAULT_WIDTH=$(DISPLAY_DEFAULT_WIDTH)
COMMON_CFLAGS += -DDISPLAY_DEFAULT_HEIGHT=$(DISPLAY_DEFAULT_HEIGHT)
COMMON_CFLAGS += -DDISPLAY_DEFAULT_BPP=$(DISPLAY_DEFAULT_BPP)
COMMON_CFLAGS += -DNET_MAX_SOCKETS=$(NET_MAX_SOCKETS)
COMMON_CFLAGS += -DTCP_MAX_CONNECTIONS=$(TCP_MAX_CONNECTIONS)
COMMON_CFLAGS += -DTCP_WINDOW_SIZE=$(TCP_WINDOW_SIZE)
COMMON_CFLAGS += -DAUDIO_MAX_STREAMS=$(AUDIO_MAX_STREAMS)
COMMON_CFLAGS += -DAUDIO_RING_BUFFER_SIZE=$(AUDIO_RING_BUFFER_SIZE)
COMMON_CFLAGS += -DAUDIO_DEFAULT_SAMPLE_RATE=$(AUDIO_DEFAULT_SAMPLE_RATE)
COMMON_CFLAGS += -DSECURITY_AUTH_MAX_USERS=$(SECURITY_AUTH_MAX_USERS)
COMMON_CFLAGS += -DSECURITY_AUTH_MAX_GROUPS=$(SECURITY_AUTH_MAX_GROUPS)
COMMON_CFLAGS += -DSECURITY_MAX_TTY_SESSIONS=$(SECURITY_MAX_TTY_SESSIONS)
COMMON_CFLAGS += -DUSB_MAX_CONTROLLERS=$(USB_MAX_CONTROLLERS)
COMMON_CFLAGS += -DUSB_MAX_DEVICES=$(USB_MAX_DEVICES)
COMMON_CFLAGS += -DMAX_BLOCK_DEVICES=$(MAX_BLOCK_DEVICES)
COMMON_CFLAGS += -DIPC_MAX_CHANNELS=$(IPC_MAX_CHANNELS)
COMMON_CFLAGS += -DPOSIX_SHM_MAX_OBJECTS=$(POSIX_SHM_MAX_OBJECTS)
COMMON_CFLAGS += -DPIT_DEFAULT_FREQUENCY=$(PIT_DEFAULT_FREQUENCY)
COMMON_CFLAGS += -DDEBUG_LOG_LEVEL=$(DEBUG_LOG_LEVEL)
COMMON_CFLAGS += -DMAX_STACK_FRAMES=$(MAX_STACK_FRAMES)
COMMON_CFLAGS += -DPANIC_MAX_STACK_FRAMES=$(PANIC_MAX_STACK_FRAMES)
COMMON_CFLAGS += -DSERIAL_BAUD_RATE=$(SERIAL_BAUD_RATE)
COMMON_CFLAGS += -DTTY_MAX_VIRTUAL_TTYS=$(TTY_MAX_VIRTUAL_TTYS)
COMMON_CFLAGS += -DINTERRUPT_MAX_NESTING_DEPTH=$(INTERRUPT_MAX_NESTING_DEPTH)
COMMON_CFLAGS += -DSCHED_PRIORITY_LEVELS=$(SCHED_PRIORITY_LEVELS)
COMMON_CFLAGS += -DMAX_PROCESSES=$(MAX_PROCESSES)
COMMON_CFLAGS += -DUSER_STACK_PAGES=$(USER_STACK_PAGES)
COMMON_CFLAGS += -DMAX_PIPES=$(MAX_PIPES)
COMMON_CFLAGS += -DMAX_PTYS=$(MAX_PTYS)
COMMON_CFLAGS += -DKERNEL_HEAP_INITIAL_SIZE=$(KERNEL_HEAP_INITIAL_SIZE)
COMMON_CFLAGS += -DKERNEL_HEAP_MAX_SIZE=$(KERNEL_HEAP_MAX_SIZE)
COMMON_CFLAGS += -DKERNEL_STACK_SIZE=$(KERNEL_STACK_SIZE)

# Build type specific flags
ifeq ($(BUILD_TYPE),debug)
    CFLAGS := $(COMMON_CFLAGS) -g -O0
    LDFLAGS := $(ARCH_LDFLAGS) -g
else
ifeq ($(BUILD_TYPE),release)
    CFLAGS := $(COMMON_CFLAGS) -O0
    LDFLAGS := $(ARCH_LDFLAGS) -O0 --gc-sections -s
else
    CFLAGS := $(COMMON_CFLAGS) -Os
    LDFLAGS := $(ARCH_LDFLAGS) -O3 --gc-sections -s -flto
endif
endif

# Special interrupt handling flags
INTERRUPT_CFLAGS := $(CFLAGS) -mgeneral-regs-only

# Vendored / third-party library flags: same as CFLAGS but -Werror is dropped
# and known-irrelevant warnings silenced, so upstream code doesn't fail the
# build under CONFIG_WERROR=yes. Used by per-file rules (e.g. qrcodegen.o).
LIB_CFLAGS := $(filter-out -Werror,$(CFLAGS)) -Wno-overflow

# Linker script selection
ifeq ($(BOOT_MODE),uefi)
    LINKER_SCRIPT := src/link_uefi_$(ARCH).ld
else ifeq ($(ARCH),64)
    LINKER_SCRIPT := src/link64.ld
else ifeq ($(ARCH),arm)
    LINKER_SCRIPT := src/arm32/link.ld
    ifeq ($(wildcard src/arm32/link.ld),)
        LINKER_SCRIPT := src/link.ld
    endif
else ifeq ($(ARCH),aarch64)
    LINKER_SCRIPT := src/aarch64/link.ld
    ifeq ($(wildcard src/aarch64/link.ld),)
        LINKER_SCRIPT := src/link.ld
    endif
else
    LINKER_SCRIPT := src/link.ld
endif

# Boot assembly selection
ifeq ($(ARCH),64)
    BOOT_ASM := src/boot64.asm
    BOOT_OBJ := boot64.o
else ifeq ($(ARCH),arm)
    # ARM32 boot stub (stub: use entry point from arm32 sources if available)
    BOOT_ASM :=
    BOOT_OBJ :=
else ifeq ($(ARCH),aarch64)
    # AArch64 boot stub
    BOOT_ASM :=
    BOOT_OBJ :=
else
    BOOT_ASM := src/boot.asm
    BOOT_OBJ := boot.o
endif

# Boot objects (must be first for multiboot header)
ifneq ($(BOOT_OBJ),)
    BOOT_OBJECTS := $(OBJDIR)/$(BOOT_OBJ)
else
    BOOT_OBJECTS :=
endif

LDFLAGS += -T $(LINKER_SCRIPT) --allow-multiple-definition

# NASM Assembly flags (NASM only used for x86/x86_64)
ifeq ($(ARCH),32)
    NASMFLAGS := -f elf32 -D__i386__
else ifeq ($(ARCH),64)
    NASMFLAGS := -f elf64 -D__x86_64__
else
    # ARM/AArch64: no NASM, GNU as is used via $(AS)
    NASMFLAGS :=
endif

# GNU as flags for .s files
ASFLAGS :=

# =============================================================================
# COLOR OUTPUT
# =============================================================================

NO_COLOR := \033[0m
OK_COLOR := \033[32;01m
ERROR_COLOR := \033[31;01m
WARN_COLOR := \033[33;01m
INFO_COLOR := \033[36;01m
