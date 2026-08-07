# build/features/canopy.mk
#
# Kernel-side Canopy source gating (src/canopy/**). Userspace Canopy gating
# (userspace/canopydm.c, canopyde/, canopy_app_*) is owned by build/userspace.mk
# via CANOPY_DM_ENABLE / CANOPY_DE_ENABLE / USERSPACE_DESKTOP_*.
#
# CANOPY_DE_ENABLE=no empties CANOPY_SRCS in kernel-sources.mk; this fragment
# adds granular per-component gates via EXCLUDED_CANOPY_SRCS. The src/canopy/
# tree may be absent in some checkouts; every path uses $(wildcard ...) so a
# missing directory degrades to a no-op.

ifeq ($(CANOPY_DE_ENABLE),no)
EXCLUDED_CANOPY_SRCS += $(wildcard \
    $(SRCDIR)/canopy/*.c \
    $(SRCDIR)/canopy/render/*.c \
    $(SRCDIR)/canopy/compositor/*.c \
    $(SRCDIR)/canopy/wm/*.c \
    $(SRCDIR)/canopy/de/*.c \
    $(SRCDIR)/canopy/theme/*.c \
    $(SRCDIR)/canopy/widgets/*.c \
    $(SRCDIR)/canopy/apps/*.c)
endif

ifeq ($(CANOPY_COMPOSITOR_ENABLE),no)
EXCLUDED_CANOPY_SRCS += $(wildcard $(SRCDIR)/canopy/compositor/*.c)
endif

ifeq ($(CANOPY_WM_ENABLE),no)
EXCLUDED_CANOPY_SRCS += $(wildcard $(SRCDIR)/canopy/wm/*.c)
endif

ifeq ($(CANOPY_PANEL_ENABLE),no)
EXCLUDED_CANOPY_SRCS += $(wildcard $(SRCDIR)/canopy/de/*panel*.c)
endif

ifeq ($(CANOPY_DOCK_ENABLE),no)
EXCLUDED_CANOPY_SRCS += $(wildcard $(SRCDIR)/canopy/de/*dock*.c)
endif

ifeq ($(CANOPY_NOTIFICATIONS_ENABLE),no)
EXCLUDED_CANOPY_SRCS += $(wildcard $(SRCDIR)/canopy/de/canopy_notifications.c)
endif

ifeq ($(CANOPY_CONTROL_CENTER_ENABLE),no)
EXCLUDED_CANOPY_SRCS += $(wildcard $(SRCDIR)/canopy/de/canopy_control_center.c)
endif

ifeq ($(CANOPY_OVERVIEW_ENABLE),no)
EXCLUDED_CANOPY_SRCS += $(wildcard $(SRCDIR)/canopy/de/canopy_overview.c)
endif

ifeq ($(CANOPY_SLEEP_SCREEN_ENABLE),no)
EXCLUDED_CANOPY_SRCS += $(wildcard $(SRCDIR)/canopy/de/canopy_sleep_screen.c)
endif

ifeq ($(CANOPY_SOUNDS_ENABLE),no)
EXCLUDED_CANOPY_SRCS += $(wildcard $(SRCDIR)/canopy/de/canopy_sounds.c)
endif

ifeq ($(CANOPY_A11Y_ENABLE),no)
EXCLUDED_CANOPY_SRCS += $(wildcard $(SRCDIR)/canopy/de/canopy_a11y.c)
endif

# Per-app kernel gating (src/canopy/apps/*): when a specific
# CANOPY_APP_<NAME>_ENABLE is no, drop that app's kernel source. Userspace app
# gating is handled by build/userspace.mk. The src/canopy/apps/ tree may be
# absent; the wildcards above degrade to no-ops in that case.
