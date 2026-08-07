# build/features/security.mk
#
# Security feature gating: SMEP/SMAP, stack protection (SSP), lock debugging,
# authentication, session management, kernel watchdog, fault prevention, and
# memory validation.
#
# Each block appends to the shared EXCLUDED_CSOURCES / KERN_EXTRA_OBJS
# variables defined by build/kernel-sources.mk. Source paths are wrapped in
# $(wildcard ...) so a missing file degrades to a no-op rather than breaking
# the build.
#
# Owned by Agent 23. Do not edit other feature fragments from here.
#
# Gating summary (each source verified to exist in $(SRCDIR)/):
#   ENABLE_SMEP_SMAP=no            -> exclude smep_smap.c
#   ENABLE_STACK_PROTECTION=no     -> exclude stack_protection.c ssp.c ssp_test.c
#   ENABLE_LOCK_DEBUGGING=no       -> exclude lock_debug.c
#   ENABLE_AUTH=no                 -> exclude auth.c
#   ENABLE_SESSION_MANAGEMENT=no   -> exclude session.c
#   ENABLE_KERNEL_WATCHDOG=no      -> (no kernel_watchdog.c in tree; nothing to gate)
#   ENABLE_FAULT_PREVENTION=yes    -> re-add $(OBJDIR)/fault_prevention.o via
#                                     KERN_EXTRA_OBJS (fault_prevention.c is in
#                                     the base EXCLUDED_CSOURCES list, so it is
#                                     excluded by default and must be explicitly
#                                     re-added when this feature is on)
#   ENABLE_MEMORY_VALIDATION=no    -> exclude memory_validation.c memory_safe_shim.c

# ---------------------------------------------------------------------------
# SMEP/SMAP (Supervisor Mode Execution/Access Prevention).
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_SMEP_SMAP),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/smep_smap.c)
endif

# ---------------------------------------------------------------------------
# Stack protection (SSP / stack canaries).
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_STACK_PROTECTION),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/stack_protection.c) \
    $(wildcard $(SRCDIR)/ssp.c) \
    $(wildcard $(SRCDIR)/ssp_test.c)
endif

# ---------------------------------------------------------------------------
# Lock debugging.
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_LOCK_DEBUGGING),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/lock_debug.c)
endif

# ---------------------------------------------------------------------------
# Authentication.
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_AUTH),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/auth.c)
endif

# ---------------------------------------------------------------------------
# Session management.
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_SESSION_MANAGEMENT),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/session.c)
endif

# ---------------------------------------------------------------------------
# Kernel watchdog.
# No kernel_watchdog.c exists in the source tree. watchdog_interrupt_support.c
# is already in the base EXCLUDED_CSOURCES list (kernel-sources.mk), and
# ps2_watchdog.c belongs to the input subsystem. There is therefore no file to
# gate here; this block is intentionally a no-op kept for documentation.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Fault prevention.
# fault_prevention.c is in the base EXCLUDED_CSOURCES list, so it is excluded
# from CSOURCES/COBJECTS by default. When ENABLE_FAULT_PREVENTION=yes, re-add
# the compiled object via KERN_EXTRA_OBJS so it is linked into the kernel
# (the generic $(OBJDIR)/%.o: $(SRCDIR)/%.c pattern rule builds it). Guarded
# by $(wildcard ...) so a missing source does not inject a dangling object.
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_FAULT_PREVENTION),yes)
ifneq ($(wildcard $(SRCDIR)/fault_prevention.c),)
KERN_EXTRA_OBJS += $(OBJDIR)/fault_prevention.o
endif
endif

# ---------------------------------------------------------------------------
# Memory validation.
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_MEMORY_VALIDATION),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/memory_validation.c) \
    $(wildcard $(SRCDIR)/memory_safe_shim.c)
endif
