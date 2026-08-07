# build/features/ipc.mk
#
# IPC and sync primitives gating. Each disabled feature appends its source
# to EXCLUDED_CSOURCES. All paths use $(wildcard ...) so missing files never
# break the build. ENABLE_SIGNALFD / ENABLE_TIMERFD have no dedicated source
# files today (logic lives in syscall.c); their blocks are no-op guards.

ifeq ($(ENABLE_IPC),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/ipc.c)
endif

ifeq ($(ENABLE_DBUS),no)
EXCLUDED_CSOURCES += $(wildcard \
    $(SRCDIR)/dbus.c \
    $(SRCDIR)/dbus_bus.c \
    $(SRCDIR)/dbus_codec.c \
    $(SRCDIR)/dbus_session.c \
    $(SRCDIR)/dbus_system.c)
endif

ifeq ($(ENABLE_SEMAPHORES),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/semaphore.c)
endif

ifeq ($(ENABLE_BARRIERS),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/barrier.c)
endif

ifeq ($(ENABLE_SYSV_SEM),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/sysv_sem.c)
endif

ifeq ($(ENABLE_SYSV_MSG),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/sysv_msg.c)
endif

ifeq ($(ENABLE_POSIX_SHM),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/posix_shm.c)
endif

ifeq ($(ENABLE_EPOLL),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/epoll.c)
endif

ifeq ($(ENABLE_INOTIFY),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/inotify.c)
endif

ifeq ($(ENABLE_EVENTFD),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/eventfd.c)
endif

# ENABLE_SIGNALFD / ENABLE_TIMERFD: no dedicated source files exist today
# (logic folded into syscall.c/task.c). No-op guards kept for future-proofing.
