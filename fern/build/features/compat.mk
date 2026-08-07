# build/features/compat.mk
#
# Compatibility / platform detection gating. Most compat logic lives in
# syscall.c / system.c / tty.c (no dedicated source files), so several gates
# are no-op guards that activate if a dedicated file is added later.
# ROOT_AUTOLOGIN is NOT gated here — it's a process option handled by
# build/flags.mk as -DENABLE_ROOT_AUTOLOGIN. All paths use $(wildcard ...).

ifeq ($(ENABLE_LINUX_COMPAT),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/linux_compat.c $(SRCDIR)/atomic_compat.c)
endif

ifeq ($(ENABLE_UNIX_COMPAT),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/unix_compat.c)
endif

ifeq ($(ENABLE_POSIX_SIGNALS),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/posix_signal*.c $(SRCDIR)/signal*.c)
endif

ifeq ($(ENABLE_POSIX_TERMIOS),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/posix_termios*.c $(SRCDIR)/termios*.c)
endif

ifeq ($(ENABLE_PLATFORM_DETECTION),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/*platform*.c)
endif

ifeq ($(HYPERVISOR_DETECTION),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/*hypervisor*.c)
endif
