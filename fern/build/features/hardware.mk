# build/features/hardware.mk
#
# Hardware bus / device gating. Each disabled feature appends its source to
# EXCLUDED_CSOURCES. ENABLE_ACPI=no also signals SKIP_UACPI=yes so the main
# Makefile / kernel-sources.mk can drop uACPI objects from the link.
# All paths use $(wildcard ...) so missing files never break the build.

HW_CSOURCES :=

ifeq ($(ENABLE_PCI),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/pci.c $(SRCDIR)/pcie.c $(SRCDIR)/pcie_test.c)
endif

ifeq ($(ENABLE_PCIE),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/pcie.c $(SRCDIR)/pcie_test.c)
endif

ifeq ($(ENABLE_ACPI),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/acpi.c $(SRCDIR)/acpi_enhanced.c $(SRCDIR)/uacpi_port.c)
SKIP_UACPI := yes
endif

ifeq ($(ENABLE_SERIAL),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/serial_devices.c)
endif

ifeq ($(ENABLE_PARALLEL),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/parallelport.c)
endif

ifeq ($(ENABLE_A20),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/a20.c)
endif

ifeq ($(ENABLE_VIRTUALBOX_GUEST),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/virtualbox_guest.c)
endif

ifeq ($(ENABLE_CHAR_DEVICES),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/char_devices.c $(SRCDIR)/char_devices_simple.c)
endif

ifeq ($(ENABLE_TTY),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/tty.c $(SRCDIR)/tty_devices.c $(SRCDIR)/tty_render.c)
endif
