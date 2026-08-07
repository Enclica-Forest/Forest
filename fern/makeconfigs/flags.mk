# =============================================================================
# makeconfigs/flags.mk — Compiler / linker / assembler flags
# =============================================================================
# Owns: COMMON_CFLAGS, CFLAGS, LDFLAGS, INTERRUPT_CFLAGS, NASMFLAGS, ASFLAGS,
#       LINKER_SCRIPT, BOOT_ASM/BOOT_OBJ, BOOT_OBJECTS,
#       USER_CFLAGS, USER_LDFLAGS, USER_LINKER_SCRIPT, USER_ASM_FLAGS,
#       USER_CRT0_SRC, and every numeric -D define.
# Consumes: config.mk (ARCH/BOOT_MODE/BUILD_TYPE, FEATURE_FLAGS, ROOT_*,
#           numeric tunables, *_COLOR), dirs.mk (SRCDIR, USER_SRCDIR),
#           toolchain.mk (ARCH_FLAGS, ARCH_LDFLAGS).
# =============================================================================

# -----------------------------------------------------------------------------
# Optimization & debug flags per build type
# -----------------------------------------------------------------------------
ifeq ($(BUILD_TYPE),debug)
    OPTIMIZATION_LEVEL := 0
    DEBUG_FLAGS        := -g -DDEBUG
else ifeq ($(BUILD_TYPE),release)
    OPTIMIZATION_LEVEL := 2
    DEBUG_FLAGS        := -g -DNDEBUG
else ifeq ($(BUILD_TYPE),optimize)
    OPTIMIZATION_LEVEL := 3
    DEBUG_FLAGS        := -O3 -DNDEBUG
else
    OPTIMIZATION_LEVEL := 0
    DEBUG_FLAGS        := -g -DDEBUG
endif

# -----------------------------------------------------------------------------
# Linker script selection (arch + boot mode)
# -----------------------------------------------------------------------------
ifeq ($(BOOT_MODE),uefi)
    ifeq ($(ARCH),64)
        LINKER_SCRIPT := $(SRCDIR)/link_uefi_64.ld
    else
        LINKER_SCRIPT := $(SRCDIR)/link_uefi_32.ld
    endif
else
    ifeq ($(ARCH),64)
        LINKER_SCRIPT := $(SRCDIR)/link64.ld
    else ifeq ($(ARCH),aarch64)
        LINKER_SCRIPT := $(SRCDIR)/aarch64/link.ld
    else ifeq ($(ARCH),arm)
        LINKER_SCRIPT := $(SRCDIR)/arm32/link.ld
    else
        LINKER_SCRIPT := $(SRCDIR)/link.ld
    endif
endif

# -----------------------------------------------------------------------------
# Boot assembly entry (BIOS multiboot / UEFI handoff)
# -----------------------------------------------------------------------------
ifeq ($(ARCH),64)
    BOOT_ASM := $(SRCDIR)/boot64.asm
else
    BOOT_ASM := $(SRCDIR)/boot.asm
endif
BOOT_OBJ     := $(OBJDIR)/boot.o
BOOT_OBJECTS := $(BOOT_OBJ)

# -----------------------------------------------------------------------------
# COMMON_CFLAGS — arch + freestanding + warnings + numeric -D + FEATURE_FLAGS
# -----------------------------------------------------------------------------
COMMON_CFLAGS := $(ARCH_FLAGS) \
    -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -fno-pic \
    -fno-exceptions -fno-rtti \
    -Wall -Wextra -Wno-unused-parameter -Wno-format \
    $(if $(OPTIMIZATION_LEVEL),-O$(OPTIMIZATION_LEVEL)) \
    $(DEBUG_FLAGS) \
    -I$(SRCDIR)/include -I$(SRCDIR) \
    -DVFS_MAX_PATH=$(VFS_MAX_PATH) \
    -DVFS_MAX_OPEN_FILES=$(VFS_MAX_OPEN_FILES) \
    -DVFS_MAX_MOUNTS=$(VFS_MAX_MOUNTS) \
    -DDISPLAY_DEFAULT_WIDTH=$(DISPLAY_DEFAULT_WIDTH) \
    -DDISPLAY_DEFAULT_HEIGHT=$(DISPLAY_DEFAULT_HEIGHT) \
    -DDISPLAY_DEFAULT_BPP=$(DISPLAY_DEFAULT_BPP) \
    -DNET_MAX_SOCKETS=$(NET_MAX_SOCKETS) \
    -DTCP_MAX_CONNECTIONS=$(TCP_MAX_CONNECTIONS) \
    -DTCP_WINDOW_SIZE=$(TCP_WINDOW_SIZE) \
    -DAUDIO_MAX_STREAMS=$(AUDIO_MAX_STREAMS) \
    -DAUDIO_RING_BUFFER_SIZE=$(AUDIO_RING_BUFFER_SIZE) \
    -DAUDIO_DEFAULT_SAMPLE_RATE=$(AUDIO_DEFAULT_SAMPLE_RATE) \
    -DSECURITY_AUTH_MAX_USERS=$(SECURITY_AUTH_MAX_USERS) \
    -DSECURITY_AUTH_MAX_GROUPS=$(SECURITY_AUTH_MAX_GROUPS) \
    -DSECURITY_MAX_TTY_SESSIONS=$(SECURITY_MAX_TTY_SESSIONS) \
    -DUSB_MAX_CONTROLLERS=$(USB_MAX_CONTROLLERS) \
    -DUSB_MAX_DEVICES=$(USB_MAX_DEVICES) \
    -DMAX_BLOCK_DEVICES=$(MAX_BLOCK_DEVICES) \
    -DIPC_MAX_CHANNELS=$(IPC_MAX_CHANNELS) \
    -DPOSIX_SHM_MAX_OBJECTS=$(POSIX_SHM_MAX_OBJECTS) \
    -DPIT_DEFAULT_FREQUENCY=$(PIT_DEFAULT_FREQUENCY) \
    -DDEBUG_LOG_LEVEL=$(DEBUG_LOG_LEVEL) \
    -DMAX_STACK_FRAMES=$(MAX_STACK_FRAMES) \
    -DPANIC_MAX_STACK_FRAMES=$(PANIC_MAX_STACK_FRAMES) \
    -DSERIAL_BAUD_RATE=$(SERIAL_BAUD_RATE) \
    -DTTY_MAX_VIRTUAL_TTYS=$(TTY_MAX_VIRTUAL_TTYS) \
    -DINTERRUPT_MAX_NESTING_DEPTH=$(INTERRUPT_MAX_NESTING_DEPTH) \
    -DSCHED_PRIORITY_LEVELS=$(SCHED_PRIORITY_LEVELS) \
    -DMAX_PROCESSES=$(MAX_PROCESSES) \
    -DUSER_STACK_PAGES=$(USER_STACK_PAGES) \
    -DMAX_PIPES=$(MAX_PIPES) \
    -DMAX_PTYS=$(MAX_PTYS) \
    -DCANOPY_DE_MAX_WINDOWS=$(CANOPY_DE_MAX_WINDOWS) \
    -DCANOPY_DE_FRAME_INTERVAL_MS=$(CANOPY_DE_FRAME_INTERVAL_MS) \
    -DCANOPY_WM_WORKSPACES=$(CANOPY_WM_WORKSPACES) \
    -DKERNEL_HEAP_INITIAL_SIZE=$(KERNEL_HEAP_INITIAL_SIZE) \
    -DKERNEL_HEAP_MAX_SIZE=$(KERNEL_HEAP_MAX_SIZE) \
    -DKERNEL_STACK_SIZE=$(KERNEL_STACK_SIZE) \
    -DGRUB_TIMEOUT=$(GRUB_TIMEOUT) \
    $(FEATURE_FLAGS) \
    $(ROOT_AUTOLOGIN_DEFINE)

# -----------------------------------------------------------------------------
# CFLAGS / per-subsystem flag variants
# -----------------------------------------------------------------------------
CFLAGS := $(COMMON_CFLAGS)

# interrupt.c / interrupt_handlers.c are compiled with the same flags but a
# separate rule (so the main Makefile can route them through INTERRUPT_CFLAGS).
INTERRUPT_CFLAGS := $(CFLAGS)

# -----------------------------------------------------------------------------
# LDFLAGS
# -----------------------------------------------------------------------------
LDFLAGS := $(ARCH_LDFLAGS) -nostdlib -nostdinc -T $(LINKER_SCRIPT) \
           -z noexecstack -z max-page-size=0x1000 --build-id=none

# -----------------------------------------------------------------------------
# NASM / GAS assembler flags
# -----------------------------------------------------------------------------
ifeq ($(ARCH),64)
    NASMFLAGS := -f elf64 -F dwarf
else
    NASMFLAGS := -f elf32 -F dwarf
endif
ASFLAGS := $(ARCH_FLAGS)

# =============================================================================
# USERSPACE FLAGS (kernel-internal libc + freestanding userspace binaries)
# =============================================================================
USER_CFLAGS := $(ARCH_FLAGS) \
    -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
    -fno-pie -fno-pic -Wall -Wextra -g -O0 \
    -I$(SRCDIR)/include -I$(LIBC_DIR)/include/libc -I$(USER_SRCDIR)/include \
    -I$(FORESTCORE_DIR)/include \
    -DUSERSPACE_BUILD \
    -mno-sse -mno-sse2 -mno-mmx -mno-3dnow \
    $(FEATURE_FLAGS)

USER_LINKER_SCRIPT := $(USER_SRCDIR)/link.ld
ifeq ($(ARCH),64)
    USER_LINKER_SCRIPT := $(USER_SRCDIR)/link64.ld
endif
USER_LDFLAGS := $(ARCH_LDFLAGS) -nostdlib -T $(USER_LINKER_SCRIPT)

ifeq ($(ARCH),64)
    USER_CRT0_SRC := $(USER_SRCDIR)/crt0_x86_64.S
else ifeq ($(ARCH),aarch64)
    USER_CRT0_SRC := $(USER_SRCDIR)/crt0_aarch64.S
else ifeq ($(ARCH),arm)
    USER_CRT0_SRC := $(USER_SRCDIR)/crt0_arm32.S
else
    USER_CRT0_SRC := $(USER_SRCDIR)/crt0_x86_32.S
endif
USER_ASM_FLAGS := $(ARCH_FLAGS)

# End of makeconfigs/flags.mk
