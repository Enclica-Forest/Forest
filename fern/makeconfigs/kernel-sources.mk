# =============================================================================
# makeconfigs/kernel-sources.mk — Kernel source selection & object lists
# =============================================================================
# Owns the master source/object lists. Subsystem SRCS lists are built with
# $(wildcard) so missing files/dirs never break the build. Feature fragments
# (makeconfigs/features/*.mk) append to the shared EXCLUDED_* variables to
# gate sources when an ENABLE_* option is `no`.
#
# Shared variables feature fragments append to (do NOT redefine here, only
# initialize as empty so `+=` works):
#   EXCLUDED_CSOURCES, EXCLUDED_GRAPHICS_SRCS, EXCLUDED_INPUT_SRCS,
#   EXCLUDED_USB_SRCS, EXCLUDED_FS_SRCS, EXCLUDED_INTERRUPT_SRCS,
#   EXCLUDED_CANOPY_SRCS, KERN_EXTRA_OBJS.
# =============================================================================

# -----------------------------------------------------------------------------
# Shared exclusion lists (feature fragments append to these)
# -----------------------------------------------------------------------------
EXCLUDED_CSOURCES        :=
EXCLUDED_GRAPHICS_SRCS   :=
EXCLUDED_INPUT_SRCS      :=
EXCLUDED_USB_SRCS        :=
EXCLUDED_FS_SRCS         :=
EXCLUDED_INTERRUPT_SRCS  :=
EXCLUDED_CANOPY_SRCS     :=
KERN_EXTRA_OBJS          :=

# -----------------------------------------------------------------------------
# Always-excluded test / standalone sources (never compiled into the kernel)
# -----------------------------------------------------------------------------
ELF_TEST_CSOURCES := $(wildcard $(SRCDIR)/elf_test.c)

EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/bitmap_pmm_test.c) \
    $(wildcard $(SRCDIR)/enhanced_heap_test.c) \
    $(wildcard $(SRCDIR)/memory_corruption_test.c) \
    $(wildcard $(SRCDIR)/memory_tests.c) \
    $(wildcard $(SRCDIR)/pcie_test.c) \
    $(wildcard $(SRCDIR)/sync_test.c) \
    $(wildcard $(SRCDIR)/ssp_test.c) \
    $(wildcard $(SRCDIR)/libc_test.c) \
    $(wildcard $(SRCDIR)/libc_simple_test.c) \
    $(wildcard $(SRCDIR)/syscall_test.c) \
    $(wildcard $(SRCDIR)/libc_stdio.c) \
    $(ELF_TEST_CSOURCES)

# -----------------------------------------------------------------------------
# Root-level C sources (src/*.c) minus exclusions
# -----------------------------------------------------------------------------
CSOURCES := $(filter-out $(EXCLUDED_CSOURCES),$(wildcard $(SRCDIR)/*.c))
COBJECTS := $(CSOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# -----------------------------------------------------------------------------
# Graphics subsystem (src/graphics/*.c + drivers/*.c)
# -----------------------------------------------------------------------------
GRAPHICS_SRCS   := $(wildcard $(SRCDIR)/graphics/*.c) $(wildcard $(SRCDIR)/graphics/drivers/*.c)
GRAPHICS_SRCS   := $(filter-out $(EXCLUDED_GRAPHICS_SRCS),$(GRAPHICS_SRCS))
GRAPHICS_OBJECTS := $(GRAPHICS_SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# -----------------------------------------------------------------------------
# Panic UI (src/panicui*.c — flat in src/ root)
# -----------------------------------------------------------------------------
PANICUI_SRCS    := $(wildcard $(SRCDIR)/panicui*.c)
PANICUI_SRCS    := $(filter-out $(EXCLUDED_CSOURCES),$(PANICUI_SRCS))
PANICUI_OBJECTS := $(PANICUI_SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# -----------------------------------------------------------------------------
# Canopy desktop subsystem (src/canopy/**/*.c — may be absent)
# -----------------------------------------------------------------------------
CANOPY_SRCS     := $(wildcard $(SRCDIR)/canopy/*.c) \
                   $(wildcard $(SRCDIR)/canopy/render/*.c) \
                   $(wildcard $(SRCDIR)/canopy/compositor/*.c) \
                   $(wildcard $(SRCDIR)/canopy/wm/*.c) \
                   $(wildcard $(SRCDIR)/canopy/de/*.c) \
                   $(wildcard $(SRCDIR)/canopy/theme/*.c) \
                   $(wildcard $(SRCDIR)/canopy/widgets/*.c) \
                   $(wildcard $(SRCDIR)/canopy/apps/*.c)
CANOPY_SRCS     := $(filter-out $(EXCLUDED_CANOPY_SRCS),$(CANOPY_SRCS))
CANOPY_OBJECTS  := $(CANOPY_SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# -----------------------------------------------------------------------------
# Input subsystem (src/input/*.c + root input/ps2/keyboard/mouse files)
# -----------------------------------------------------------------------------
INPUT_SRCS      := $(wildcard $(SRCDIR)/input/*.c) \
                   $(wildcard $(SRCDIR)/kb.c) \
                   $(wildcard $(SRCDIR)/mouse.c) \
                   $(wildcard $(SRCDIR)/keyboard_layout.c) \
                   $(wildcard $(SRCDIR)/keyboard_interrupt_handler.c) \
                   $(wildcard $(SRCDIR)/mouse_interrupt_handler.c) \
                   $(wildcard $(SRCDIR)/ps2_controller.c) \
                   $(wildcard $(SRCDIR)/ps2_keyboard.c) \
                   $(wildcard $(SRCDIR)/ps2_watchdog.c) \
                   $(wildcard $(SRCDIR)/hotkey.c)
INPUT_SRCS      := $(filter-out $(EXCLUDED_INPUT_SRCS),$(INPUT_SRCS))
INPUT_OBJECTS   := $(INPUT_SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# -----------------------------------------------------------------------------
# USB subsystem (src/usb/*.c + root usb*.c + host controller drivers)
# -----------------------------------------------------------------------------
USB_CSOURCES    := $(wildcard $(SRCDIR)/usb/*.c) \
                   $(wildcard $(SRCDIR)/usb.c) \
                   $(wildcard $(SRCDIR)/usb_hid.c) \
                   $(wildcard $(SRCDIR)/usb_hub.c) \
                   $(wildcard $(SRCDIR)/ehci_hc.c) \
                   $(wildcard $(SRCDIR)/ohci_hc.c) \
                   $(wildcard $(SRCDIR)/uhci_hc.c) \
                   $(wildcard $(SRCDIR)/xhci_hc.c)
USB_CSOURCES    := $(filter-out $(EXCLUDED_USB_SRCS),$(USB_CSOURCES))
USB_OBJECTS     := $(USB_CSOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# -----------------------------------------------------------------------------
# Canopy Graphics Device Manager integration (src/cgdm*.c)
# -----------------------------------------------------------------------------
CGDM_SRCS       := $(wildcard $(SRCDIR)/cgdm*.c)
CGDM_OBJECTS    := $(CGDM_SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# -----------------------------------------------------------------------------
# Interrupt subsystem (src/interrupt*.c + irq/idt/ioapic/apic/pic/spurious)
# -----------------------------------------------------------------------------
INTERRUPT_SRCS  := $(wildcard $(SRCDIR)/interrupt*.c) \
                   $(wildcard $(SRCDIR)/irq_management.c) \
                   $(wildcard $(SRCDIR)/idt.c) \
                   $(wildcard $(SRCDIR)/idt64.c) \
                   $(wildcard $(SRCDIR)/ioapic.c) \
                   $(wildcard $(SRCDIR)/apic.c) \
                   $(wildcard $(SRCDIR)/pic_8259a.c) \
                   $(wildcard $(SRCDIR)/spurious_interrupt.c) \
                   $(wildcard $(SRCDIR)/exceptions.c) \
                   $(wildcard $(SRCDIR)/nmi.c) \
                   $(wildcard $(SRCDIR)/msi_support.c)
INTERRUPT_SRCS  := $(filter-out $(EXCLUDED_INTERRUPT_SRCS),$(INTERRUPT_SRCS))
INTERRUPT_OBJECTS := $(INTERRUPT_SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# -----------------------------------------------------------------------------
# Filesystem subsystem (src/fs*.c + per-FS implementations)
# -----------------------------------------------------------------------------
FS_SRCS         := $(wildcard $(SRCDIR)/fs*.c) \
                   $(wildcard $(SRCDIR)/vfs.c) \
                   $(wildcard $(SRCDIR)/devfs.c) \
                   $(wildcard $(SRCDIR)/procfs.c) \
                   $(wildcard $(SRCDIR)/sysfs.c) \
                   $(wildcard $(SRCDIR)/tmpfs.c) \
                   $(wildcard $(SRCDIR)/ramdisk.c) \
                   $(wildcard $(SRCDIR)/iso9660.c) \
                   $(wildcard $(SRCDIR)/exfat.c) \
                   $(wildcard $(SRCDIR)/fat.c) \
                   $(wildcard $(SRCDIR)/jffs2.c) \
                   $(wildcard $(SRCDIR)/lean.c) \
                   $(wildcard $(SRCDIR)/udf.c) \
                   $(wildcard $(SRCDIR)/ustar.c) \
                   $(wildcard $(SRCDIR)/yaffs.c) \
                   $(wildcard $(SRCDIR)/zdsfs.c) \
                   $(wildcard $(SRCDIR)/symlink.c)
FS_SRCS         := $(filter-out $(EXCLUDED_FS_SRCS),$(FS_SRCS))
FS_OBJECTS      := $(FS_SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# -----------------------------------------------------------------------------
# uACPI library (libs/uacpi/source/*.c — built via the uacpi_%.o pattern rule)
# -----------------------------------------------------------------------------
UACPI_SRCS      := $(wildcard $(UACPI_SRCDIR)/*.c)
UACPI_OBJECTS   := $(UACPI_SRCS:$(UACPI_SRCDIR)/%.c=$(OBJDIR)/uacpi_%.o)

# -----------------------------------------------------------------------------
# ARM32 / AArch64 / crossarcinterpret architecture sources
# -----------------------------------------------------------------------------
ARM32_SRCS      := $(wildcard $(SRCDIR)/arm32/*.c)
ARM32_OBJECTS   := $(ARM32_SRCS:$(SRCDIR)/arm32/%.c=$(OBJDIR)/arm32/%.o) \
                   $(patsubst $(SRCDIR)/arm32/%.S,$(OBJDIR)/arm32/%.o,$(wildcard $(SRCDIR)/arm32/*.S))

AARCH64_SRCS    := $(wildcard $(SRCDIR)/aarch64/*.c)
AARCH64_OBJECTS := $(AARCH64_SRCS:$(SRCDIR)/aarch64/%.c=$(OBJDIR)/aarch64/%.o) \
                   $(patsubst $(SRCDIR)/aarch64/%.S,$(OBJDIR)/aarch64/%.o,$(wildcard $(SRCDIR)/aarch64/*.S))

CROSSARC_SRCS   := $(wildcard $(SRCDIR)/crossarcinterpret/*.c)
CROSSARC_OBJECTS := $(CROSSARC_SRCS:$(SRCDIR)/crossarcinterpret/%.c=$(OBJDIR)/crossarcinterpret/%.o)

# -----------------------------------------------------------------------------
# Assembly sources (boot entry + low-level stubs)
# -----------------------------------------------------------------------------
ASM_SRCS := \
    $(wildcard $(SRCDIR)/boot.asm) \
    $(wildcard $(SRCDIR)/boot64.asm) \
    $(wildcard $(SRCDIR)/context_switch.asm) \
    $(wildcard $(SRCDIR)/cpu_ops.asm) \
    $(wildcard $(SRCDIR)/interrupt_stubs.asm) \
    $(wildcard $(SRCDIR)/interrupt_stubs.s) \
    $(wildcard $(SRCDIR)/syscall_stubs.asm) \
    $(wildcard $(SRCDIR)/syscall64_stubs.asm)

ASMOBJECTS := \
    $(if $(wildcard $(SRCDIR)/boot.asm),$(OBJDIR)/boot.o) \
    $(if $(wildcard $(SRCDIR)/boot64.asm),$(OBJDIR)/boot64.o) \
    $(if $(wildcard $(SRCDIR)/context_switch.asm),$(OBJDIR)/context_switch.o) \
    $(if $(wildcard $(SRCDIR)/cpu_ops.asm),$(OBJDIR)/cpu_ops.o) \
    $(if $(wildcard $(SRCDIR)/interrupt_stubs.asm)$(wildcard $(SRCDIR)/interrupt_stubs.s),$(OBJDIR)/interrupt_stubs.o) \
    $(if $(wildcard $(SRCDIR)/syscall_stubs.asm),$(OBJDIR)/syscall_stubs.o) \
    $(if $(wildcard $(SRCDIR)/syscall64_stubs.asm),$(OBJDIR)/syscall64_stubs.o)

# Arch-extra assembly (ARM/AArch64 startup & exception vectors)
ARCH_EXTRA_OBJECTS :=
ifeq ($(ARCH),arm)
    ARCH_EXTRA_OBJECTS += $(patsubst $(SRCDIR)/arm32/%.S,$(OBJDIR)/arm32/%.o,$(wildcard $(SRCDIR)/arm32/*.S))
endif
ifeq ($(ARCH),aarch64)
    ARCH_EXTRA_OBJECTS += $(patsubst $(SRCDIR)/aarch64/%.S,$(OBJDIR)/aarch64/%.o,$(wildcard $(SRCDIR)/aarch64/*.S))
endif

# -----------------------------------------------------------------------------
# Cross-architecture heap wrapper (src/arch/kheap.c)
# -----------------------------------------------------------------------------
ARCH_KHEAP_SRCS    := $(wildcard $(SRCDIR)/arch/kheap.c)
ARCH_KHEAP_OBJECTS := $(ARCH_KHEAP_SRCS:$(SRCDIR)/arch/%.c=$(OBJDIR)/arch/%.o)

# -----------------------------------------------------------------------------
# QR code generator library (libs/qrcodegen/qrcodegen.c)
# -----------------------------------------------------------------------------
QRCODEGEN_SRCS    := $(wildcard $(QRCODEGEN_DIR)/qrcodegen.c)
QRCODEGEN_OBJECTS := $(OBJDIR)/qrcodegen.o

# -----------------------------------------------------------------------------
# ALL_OBJECTS — everything the kernel links
# -----------------------------------------------------------------------------
ALL_OBJECTS := \
    $(COBJECTS) \
    $(GRAPHICS_OBJECTS) \
    $(PANICUI_OBJECTS) \
    $(CANOPY_OBJECTS) \
    $(INPUT_OBJECTS) \
    $(USB_OBJECTS) \
    $(CGDM_OBJECTS) \
    $(INTERRUPT_OBJECTS) \
    $(FS_OBJECTS) \
    $(UACPI_OBJECTS) \
    $(ARM32_OBJECTS) \
    $(AARCH64_OBJECTS) \
    $(CROSSARC_OBJECTS) \
    $(ARCH_KHEAP_OBJECTS) \
    $(ASMOBJECTS) \
    $(ARCH_EXTRA_OBJECTS) \
    $(BOOT_OBJECTS) \
    $(QRCODEGEN_OBJECTS) \
    $(KERN_EXTRA_OBJS)

# End of makeconfigs/kernel-sources.mk
