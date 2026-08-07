# build/features/usb.mk
#
# USB subsystem gating. ENABLE_USB is the parent. src/usb/*.c go to
# EXCLUDED_USB_SRCS; top-level src/*.c USB controller files go to
# EXCLUDED_CSOURCES. When ENABLE_USB=yes, files are built by the standard
# pattern rules (no KERN_EXTRA_OBJS needed). QEMU_USB appends to QEMU_OPTS.
# All paths use $(wildcard ...) so missing files never break the build.

ifeq ($(ENABLE_USB),no)
EXCLUDED_USB_SRCS += $(wildcard $(SRCDIR)/usb/*.c)
EXCLUDED_CSOURCES += $(wildcard \
    $(SRCDIR)/usb.c \
    $(SRCDIR)/usb_hid.c \
    $(SRCDIR)/usb_hub.c \
    $(SRCDIR)/ehci_hc.c \
    $(SRCDIR)/uhci_hc.c \
    $(SRCDIR)/ohci_hc.c \
    $(SRCDIR)/xhci_hc.c)
endif

ifeq ($(ENABLE_USB_UHCI),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/uhci_hc.c)
endif

ifeq ($(ENABLE_USB_OHCI),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/ohci_hc.c)
endif

ifeq ($(ENABLE_USB_EHCI),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/ehci_hc.c)
endif

ifeq ($(ENABLE_USB_XHCI),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/xhci_hc.c)
endif

ifeq ($(ENABLE_USB_HID),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/usb_hid.c)
endif

ifeq ($(ENABLE_USB_HUB),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/usb_hub.c)
endif

ifeq ($(ENABLE_USB_MASS_STORAGE),no)
EXCLUDED_USB_SRCS += $(wildcard $(SRCDIR)/usb/*mass*.c $(SRCDIR)/usb/*msd*.c)
endif

# QEMU USB emulation option (consumed by build/qemu-run.mk via QEMU_OPTS)
ifeq ($(QEMU_USB),yes)
QEMU_OPTS += -device usb-ehci
endif
