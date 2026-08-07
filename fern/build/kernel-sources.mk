# build/kernel-sources.mk
#
# Kernel source selection: base EXCLUDED_* lists, CSOURCES/COBJECTS, subsystem
# source/object lists, and the ALL_OBJECTS aggregation.
#
# This file is included by the main Makefile BEFORE build/features/*.mk.
# Feature fragments APPEND to the shared variables defined here
# (EXCLUDED_CSOURCES, EXCLUDED_*_SRCS, KERN_EXTRA_OBJS, QEMU_OPTS) to gate
# sources when a feature is "no".
#
# DEFERRED-EXPANSION DESIGN:
#   CSOURCES, COBJECTS, the subsystem *_SRCS/*_OBJECTS lists, and ALL_OBJECTS
#   use recursively-expanded (`=`) assignment rather than simple (`:=`).
#   Because feature fragments are included AFTER this file, a `:=` here would
#   freeze their value before the feature appends land. With `=`, the value is
#   recomputed at expansion time (i.e. at rule-evaluation), by which point all
#   build/features/*.mk appends have been applied. EXCLUDED_CSOURCES itself is
#   `:=` for its BASE always-excluded list; feature fragments use `+=` to
#   append, and `+=` on a simple variable appends at the point of the +=, so
#   the deferred consumers above see the complete list.
#
# OWNERSHIP:
#   Feature-specific exclusions (audio, networking, usb, storage, input,
#   graphics, filesystems, interrupts, canopy, etc.) are NOT defined here.
#   They live in build/features/*.mk. This file provides only:
#     * the always-excluded base list (advanced interrupt_*.c, stb_vorbis.c,
#       sound_pcm_device.c), and
#     * the aggregation logic.

# ---------------------------------------------------------------------------
# Shared variables that feature fragments append to (initialized empty).
# EXCLUDED_CSOURCES is initialized to the base always-excluded list below.
# ---------------------------------------------------------------------------
EXCLUDED_GRAPHICS_SRCS  :=
EXCLUDED_INPUT_SRCS     :=
EXCLUDED_USB_SRCS       :=
EXCLUDED_FS_SRCS        :=
EXCLUDED_INTERRUPT_SRCS :=
KERN_EXTRA_OBJS         :=
QEMU_OPTS               :=

# ---------------------------------------------------------------------------
# Base always-excluded kernel sources.
#
# Advanced interrupt_*.c files (the long-form advanced implementations) are
# excluded by default; the three core ones (interrupt.c, interrupt_handlers.c,
# interrupt_utils.c) are re-added via filter-out below so the kernel keeps a
# working interrupt core. Also excluded by default: stb_vorbis.c and
# sound_pcm_device.c.
#
# NOT in this base list (owned by build/features/*.mk):
#   * usb.c, usb_hid.c, usb_hub.c, ehci/uhci/ohci/xhci_hc.c  -> usb.mk
#   * sound_*.c (other than sound_pcm_device.c)              -> audio.mk
#   * net.c                                                  -> networking.mk
# The inline `ifeq ($(ENABLE_AUDIO/NETWORKING/USB),no)` blocks that used to
# live in the Makefile have been removed here and relocated to those fragments.
# ---------------------------------------------------------------------------
EXCLUDED_CSOURCES := \
    $(SRCDIR)/interrupt_priority.c \
    $(SRCDIR)/interrupt_statistics.c \
    $(SRCDIR)/interrupt_driven_io.c \
    $(SRCDIR)/interrupt_coalescing.c \
    $(SRCDIR)/interrupt_latency_optimization.c \
    $(SRCDIR)/interrupt_load_balancing.c \
    $(SRCDIR)/interrupt_context_switching.c \
    $(SRCDIR)/interrupt_affinity_control.c \
    $(SRCDIR)/interrupt_memory_sync.c \
    $(SRCDIR)/interrupt_eoi_management.c \
    $(SRCDIR)/interrupt_mask_primitives.c \
    $(SRCDIR)/interrupt_profiling.c \
    $(SRCDIR)/interrupt_replay_mechanism.c \
    $(SRCDIR)/interrupt_throttling.c \
    $(SRCDIR)/interrupt_vector_allocation.c \
    $(SRCDIR)/interrupt_controller_abstraction.c \
    $(SRCDIR)/cgdm_integration.c \
    $(wildcard $(SRCDIR)/interrupt_*.c) \
    $(SRCDIR)/acpi_interrupt_routing.c \
    $(SRCDIR)/fault_prevention.c \
    $(SRCDIR)/ipi_smp_coordination.c \
    $(SRCDIR)/irq_management.c \
    $(SRCDIR)/msi_support.c \
    $(SRCDIR)/smp_interrupt_distribution.c \
    $(SRCDIR)/spurious_interrupt.c \
    $(SRCDIR)/watchdog_interrupt_support.c \
    $(SRCDIR)/stb_vorbis.c \
    $(SRCDIR)/sound_pcm_device.c

# Re-include the three core interrupt files so the kernel keeps a working
# interrupt core despite the wildcard exclude above.
EXCLUDED_CSOURCES := $(filter-out \
    $(SRCDIR)/interrupt.c \
    $(SRCDIR)/interrupt_handlers.c \
    $(SRCDIR)/interrupt_utils.c,$(EXCLUDED_CSOURCES))

# ---------------------------------------------------------------------------
# Explicit subsystem source lists (no wildcard; not feature-gated here).
# ---------------------------------------------------------------------------
CGDM_CSOURCES     := $(SRCDIR)/display_manager.c $(SRCDIR)/mode_state.c $(SRCDIR)/hotkey.c
ELF_TEST_CSOURCES := $(SRCDIR)/elf_test.c

# ---------------------------------------------------------------------------
# QR Code generator library.
# ---------------------------------------------------------------------------
QRCODEGEN_DIR     := libs/qrcodegen
QRCODEGEN_SRCS    := $(QRCODEGEN_DIR)/qrcodegen.c
QRCODEGEN_OBJECTS := $(OBJDIR)/qrcodegen.o

# ---------------------------------------------------------------------------
# Kernel C sources / objects.
# Deferred (`=`) so EXCLUDED_* appends from build/features/*.mk are visible.
# The filter combines the base EXCLUDED_CSOURCES with the subsystem exclusion
# lists whose paths may overlap $(SRCDIR)/*.c (notably EXCLUDED_INTERRUPT_SRCS;
# the graphics/input/canopy lists are subdir paths and overlap is harmless).
# ---------------------------------------------------------------------------
CSOURCES = $(filter-out \
    $(EXCLUDED_CSOURCES) \
    $(EXCLUDED_GRAPHICS_SRCS) \
    $(EXCLUDED_INPUT_SRCS) \
    $(EXCLUDED_INTERRUPT_SRCS),$(wildcard $(SRCDIR)/*.c)) $(SRCDIR)/symlink.c
COBJECTS = $(CSOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# ---------------------------------------------------------------------------
# Subsystem source / object lists.
# Lists that consume an EXCLUDED_*_SRCS shared var use deferred (`=`) expansion
# so feature-fragment appends are picked up. Lists with no shared exclusion var
# use simple (`:=`) expansion.
# ---------------------------------------------------------------------------

# Graphics subsystem (src/graphics/, src/graphics/drivers/).
GRAPHICS_SRCS = $(filter-out $(EXCLUDED_GRAPHICS_SRCS),\
    $(wildcard $(SRCDIR)/graphics/*.c) $(wildcard $(SRCDIR)/graphics/drivers/*.c))
GRAPHICS_OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(GRAPHICS_SRCS))

# Panic UI (top-level src/panicui*.c). Filtered by EXCLUDED_CSOURCES so
# build/features/graphics.mk (ENABLE_PANICUI=no / ENABLE_GRAPHICS=no) can drop it.
PANICUI_SRCS    = $(filter-out $(EXCLUDED_CSOURCES),$(wildcard $(SRCDIR)/panicui*.c))
PANICUI_OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(PANICUI_SRCS))

# Canopy desktop environment removed: Fern is a kernel only, it does not bundle
# the Canopy desktop. No desktop sources enter the kernel link.

# Input subsystem (src/input/).
INPUT_SRCS = $(filter-out $(EXCLUDED_INPUT_SRCS),$(wildcard $(SRCDIR)/input/*.c))
INPUT_OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(INPUT_SRCS))

# USB subsystem (src/usb/). ENABLE_USB gating is owned by
# build/features/usb.mk (it appends to EXCLUDED_USB_SRCS, or empties
# USB_CSOURCES, when ENABLE_USB=no). Here we only filter EXCLUDED_USB_SRCS.
USB_CSOURCES = $(filter-out $(EXCLUDED_USB_SRCS),$(wildcard $(SRCDIR)/usb/*.c))

# CGDM display manager removed: Fern is kernel-only, no display manager links.
# (The sole src/cgdm*.c file, cgdm_integration.c, is already in the base
# EXCLUDED_CSOURCES list, so this expands empty regardless.)
CGDM_SRCS =
CGDM_OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,\
    $(filter-out $(SRCDIR)/cgdm_integration.c,$(CGDM_SRCS)))

# Interrupt sources (top-level src/interrupt*.c). Filtered by the base
# EXCLUDED_CSOURCES plus feature-fragment EXCLUDED_INTERRUPT_SRCS appends.
INTERRUPT_SRCS = $(wildcard $(SRCDIR)/interrupt*.c)
INTERRUPT_OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,\
    $(filter-out $(EXCLUDED_CSOURCES) $(EXCLUDED_INTERRUPT_SRCS),$(INTERRUPT_SRCS)))

# Filesystem subsystem (src/fs/).
FS_SRCS = $(filter-out $(EXCLUDED_FS_SRCS),$(wildcard $(SRCDIR)/fs/*.c))
FS_OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(FS_SRCS))

# uACPI library. Dropped when ENABLE_ACPI=no (signaled by SKIP_UACPI=yes from
# build/features/hardware.mk, included after this file). Deferred expansion so
# SKIP_UACPI is visible at expansion time.
UACPI_SRCS    = $(wildcard $(UACPI_SRCDIR)/*.c)
UACPI_OBJECTS = $(if $(filter yes,$(SKIP_UACPI)),,\
    $(patsubst $(UACPI_SRCDIR)/%.c,$(OBJDIR)/uacpi_%.o,$(UACPI_SRCS)))

# ARM32-specific sources (src/arm32/).
ARM32_SRCS        := $(wildcard $(SRCDIR)/arm32/*.c) $(wildcard $(SRCDIR)/arm32/*.S)
ARM32_C_OBJECTS   := $(patsubst $(SRCDIR)/arm32/%.c,$(OBJDIR)/arm32/%.o,$(filter %.c,$(ARM32_SRCS)))
ARM32_S_OBJECTS   := $(patsubst $(SRCDIR)/arm32/%.S,$(OBJDIR)/arm32/%.o,$(filter %.S,$(ARM32_SRCS)))
ARM32_OBJECTS     := $(ARM32_C_OBJECTS) $(ARM32_S_OBJECTS)

# AArch64-specific sources (src/aarch64/).
AARCH64_SRCS      := $(wildcard $(SRCDIR)/aarch64/*.c) $(wildcard $(SRCDIR)/aarch64/*.S)
AARCH64_C_OBJECTS := $(patsubst $(SRCDIR)/aarch64/%.c,$(OBJDIR)/aarch64/%.o,$(filter %.c,$(AARCH64_SRCS)))
AARCH64_S_OBJECTS := $(patsubst $(SRCDIR)/aarch64/%.S,$(OBJDIR)/aarch64/%.o,$(filter %.S,$(AARCH64_SRCS)))
AARCH64_OBJECTS   := $(AARCH64_C_OBJECTS) $(AARCH64_S_OBJECTS)

# Cross-architecture interpreter (src/crossarcinterpret/).
CROSSARC_SRCS    := $(wildcard $(SRCDIR)/crossarcinterpret/*.c)
CROSSARC_OBJECTS := $(patsubst $(SRCDIR)/crossarcinterpret/%.c,$(OBJDIR)/crossarcinterpret/%.o,$(CROSSARC_SRCS))

# ASM objects (from .s and .asm files) -- x86/x86_64 only.
ifeq ($(filter arm aarch64,$(ARCH)),)
ASM_SRCS := $(wildcard $(SRCDIR)/*.s) $(wildcard $(SRCDIR)/*.asm)
ASMOBJECTS := $(patsubst $(SRCDIR)/%.s,$(OBJDIR)/%.o,$(filter $(SRCDIR)/%.s,$(ASM_SRCS))) \
              $(patsubst $(SRCDIR)/%.asm,$(OBJDIR)/%.o,$(filter $(SRCDIR)/%.asm,$(ASM_SRCS)))
else
ASM_SRCS :=
ASMOBJECTS :=
endif

# Architecture-specific extra objects included in the kernel link.
ifeq ($(ARCH),arm)
    ARCH_EXTRA_OBJECTS := $(ARM32_OBJECTS)
else ifeq ($(ARCH),aarch64)
    ARCH_EXTRA_OBJECTS := $(AARCH64_OBJECTS)
else
    ARCH_EXTRA_OBJECTS :=
endif

# ---------------------------------------------------------------------------
# ALL_OBJECTS aggregation.
# Deferred (`=`) so it picks up:
#   * KERN_EXTRA_OBJS appends from build/features/*.mk (e.g. usb.mk adds USB
#     host-controller objects when ENABLE_USB=yes), and
#   * USB_CSOURCES / HW_CSOURCES computed by feature fragments.
# HW_CSOURCES is owned by build/features/hardware.mk; it is empty when that
# fragment has not populated it (the substitution on an empty var yields empty).
# ELF_TEST_OBJECTS is intentionally left referenced-but-undefined here to
# preserve existing behavior (the original Makefile never defined it).
# ---------------------------------------------------------------------------
ALL_OBJECTS = $(COBJECTS) $(GRAPHICS_OBJECTS) $(PANICUI_OBJECTS) \
              $(INPUT_OBJECTS) $(CGDM_OBJECTS) $(BOOT_OBJECTS) $(ASMOBJECTS) \
              $(INTERRUPT_OBJECTS) $(UACPI_OBJECTS) $(ELF_TEST_OBJECTS) \
              $(FS_OBJECTS) $(USB_CSOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o) \
              $(HW_CSOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o) $(QRCODEGEN_OBJECTS) \
              $(ARCH_EXTRA_OBJECTS) $(KERN_EXTRA_OBJS)
