# build/features/scheduler.mk
#
# Scheduler / process / SMP gating. Core controllers built by default are
# gated OFF via EXCLUDED_CSOURCES when =no. Base-excluded advanced files
# (smp_interrupt_distribution.c, ipi_smp_coordination.c) are re-added via
# KERN_EXTRA_OBJS when their feature =yes. All paths use $(wildcard ...).

ifeq ($(ENABLE_ELF_LOADER),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/elf.c $(SRCDIR)/elf_test.c)
endif

ifeq ($(ENABLE_LDSO),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/ldso.c)
endif

ifeq ($(ENABLE_JOB_CONTROL),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/job_control.c)
endif

# ENABLE_SIGNALS: no dedicated signals.c (logic in syscall.c/task.c); no-op.
# ENABLE_IDLE_TASK: no idle_task.c; no-op.
# ENABLE_FPU: handled in arch code; no top-level file to gate.

ifeq ($(ENABLE_SMP),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/smp.c)
# smp_interrupt_distribution.c and ipi_smp_coordination.c are base-excluded;
# only re-add them when ENABLE_SMP=yes (block below).
endif

# NOTE: smp_interrupt_distribution.c and ipi_smp_coordination.c are advanced
# SMP interrupt-routing sources that reference helpers which are not yet
# implemented anywhere in the tree (local_apic_write/read/enable/disable,
# io_apic_configure_entry_extended, select_numa_optimal_cpu). Linking them
# breaks the kernel with undefined references, so they stay quarantined in
# the base EXCLUDED_CSOURCES list and are NOT re-added here even when
# ENABLE_SMP=yes. The SMP core (smp.c) links fine and is kept. Re-enable the
# blocks below only after those helpers gain real implementations.
#
# ifeq ($(ENABLE_SMP),yes)
# ifneq ($(wildcard $(SRCDIR)/smp_interrupt_distribution.c),)
# KERN_EXTRA_OBJS += $(OBJDIR)/smp_interrupt_distribution.o
# endif
# ifneq ($(wildcard $(SRCDIR)/ipi_smp_coordination.c),)
# KERN_EXTRA_OBJS += $(OBJDIR)/ipi_smp_coordination.o
# endif
# endif

ifeq ($(ENABLE_X86_IST),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/x86_64_ist_handling.c)
endif

ifeq ($(ENABLE_CROSSARC_INTERPRETER),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/crossarcinterpret/*.c)
endif
