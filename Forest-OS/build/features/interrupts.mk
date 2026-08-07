# build/features/interrupts.mk
#
# Interrupt controller / IRQ management feature gating.
#
# Owned by agent 22. Gates the compile/link of the PIC, Local APIC, IOAPIC,
# MSI, NMI, and advanced interrupt-management sources based on the
# ENABLE_* booleans supplied by build-config.mk (every bool is guaranteed
# "yes" or "no" by build/config.mk).
#
# Two gating strategies are used, matching where each source lives relative
# to the base EXCLUDED_CSOURCES list defined in build/kernel-sources.mk:
#
#   1. CORE CONTROLLERS (built by default; gated OFF when =no):
#        pic_8259a.c, apic.c, ioapic.c, nmi.c
#      These are NOT in the base EXCLUDED_CSOURCES list, so the wildcard in
#      kernel-sources.mk picks them up and they compile/link by default.
#      When their ENABLE_X is "no" we append the source to EXCLUDED_CSOURCES,
#      which drops it from CSOURCES/COBJECTS (and thus the kernel link).
#
#   2. ADVANCED INTERRUPT FEATURES (excluded by default; re-included when =yes):
#        msi_support.c
#        interrupt_priority.c
#        interrupt_statistics.c
#        interrupt_vector_allocation.c
#        interrupt_eoi_management.c
#      These ARE in the base EXCLUDED_CSOURCES list (msi_support.c is listed
#      explicitly; the interrupt_*.c ones are listed explicitly AND matched by
#      the $(wildcard $(SRCDIR)/interrupt_*.c) entry). They are therefore
#      filtered out of CSOURCES/COBJECTS/INTERRUPT_OBJECTS by default. When
#      their ENABLE_X is "yes" we add the corresponding object to
#      KERN_EXTRA_OBJS, which build/kernel-sources.mk folds into ALL_OBJECTS
#      so the standard `$(OBJDIR)/%.o: $(SRCDIR)/%.c` pattern rule builds and
#      links it. No double-link risk: the source remains in
#      EXCLUDED_CSOURCES, so it never appears in COBJECTS or
#      INTERRUPT_OBJECTS; KERN_EXTRA_OBJS is the sole link path.
#
# Existence guards: every addition is wrapped in $(wildcard ...) so a missing
# source file never breaks the build (per the shared-spec gating convention).
#
# Mapping summary:
#   ENABLE_PIC_8259A=no                  -> exclude pic_8259a.c
#   ENABLE_LOCAL_APIC=no                 -> exclude apic.c
#   ENABLE_IOAPIC=no                     -> exclude ioapic.c
#   ENABLE_NMI_HANDLER=no                -> exclude nmi.c
#   ENABLE_MSI=yes                       -> extra obj msi_support.o
#   ENABLE_INTERRUPT_PRIORITY=yes        -> extra obj interrupt_priority.o
#   ENABLE_INTERRUPT_STATISTICS=yes      -> extra obj interrupt_statistics.o
#   ENABLE_INTERRUPT_VECTOR_ALLOCATION=yes -> extra obj interrupt_vector_allocation.o
#   ENABLE_INTERRUPT_EOI_MANAGEMENT=yes  -> extra obj interrupt_eoi_management.o

# ---------------------------------------------------------------------------
# Core interrupt controllers: built by default; exclude when disabled.
# ---------------------------------------------------------------------------

ifeq ($(ENABLE_PIC_8259A),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/pic_8259a.c)
endif

ifeq ($(ENABLE_LOCAL_APIC),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/apic.c)
endif

ifeq ($(ENABLE_IOAPIC),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/ioapic.c)
endif

ifeq ($(ENABLE_NMI_HANDLER),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/nmi.c)
endif

# ---------------------------------------------------------------------------
# Advanced interrupt features: base-excluded by default; re-include the
# object via KERN_EXTRA_OBJS when enabled. The $(if $(wildcard ...),...)
# guard yields the object path only when the source exists.
# ---------------------------------------------------------------------------

ifeq ($(ENABLE_MSI),yes)
KERN_EXTRA_OBJS += $(if $(wildcard $(SRCDIR)/msi_support.c),$(OBJDIR)/msi_support.o)
endif

ifeq ($(ENABLE_INTERRUPT_PRIORITY),yes)
KERN_EXTRA_OBJS += $(if $(wildcard $(SRCDIR)/interrupt_priority.c),$(OBJDIR)/interrupt_priority.o)
endif

ifeq ($(ENABLE_INTERRUPT_STATISTICS),yes)
KERN_EXTRA_OBJS += $(if $(wildcard $(SRCDIR)/interrupt_statistics.c),$(OBJDIR)/interrupt_statistics.o)
endif

ifeq ($(ENABLE_INTERRUPT_VECTOR_ALLOCATION),yes)
KERN_EXTRA_OBJS += $(if $(wildcard $(SRCDIR)/interrupt_vector_allocation.c),$(OBJDIR)/interrupt_vector_allocation.o)
endif

ifeq ($(ENABLE_INTERRUPT_EOI_MANAGEMENT),yes)
KERN_EXTRA_OBJS += $(if $(wildcard $(SRCDIR)/interrupt_eoi_management.c),$(OBJDIR)/interrupt_eoi_management.o)
endif
