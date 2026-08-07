# build/features/input.mk
#
# Input subsystem gating. ENABLE_PS2 is the parent for PS/2 devices;
# ENABLE_INPUT_EVENT_SYSTEM is independent. Top-level src/*.c go to
# EXCLUDED_CSOURCES; src/input/*.c go to EXCLUDED_INPUT_SRCS.
# All paths use $(wildcard ...) so missing files never break the build.
# conf.sh forces PS2_KEYBOARD/PS2_MOUSE/PS2_WATCHDOG=no when ENABLE_PS2=no,
# so child gates fire too; duplicate entries are deduped by filter-out.

ifeq ($(ENABLE_PS2),no)
EXCLUDED_CSOURCES += $(wildcard \
    $(SRCDIR)/ps2_controller.c \
    $(SRCDIR)/ps2_keyboard.c \
    $(SRCDIR)/ps2_watchdog.c \
    $(SRCDIR)/kb.c \
    $(SRCDIR)/keyboard_layout.c \
    $(SRCDIR)/keyboard_interrupt_handler.c \
    $(SRCDIR)/mouse.c \
    $(SRCDIR)/mouse_interrupt_handler.c)
EXCLUDED_INPUT_SRCS += $(wildcard $(SRCDIR)/input/ps2*.c)
endif

ifeq ($(ENABLE_PS2_KEYBOARD),no)
EXCLUDED_CSOURCES += $(wildcard \
    $(SRCDIR)/ps2_keyboard.c \
    $(SRCDIR)/kb.c \
    $(SRCDIR)/keyboard_layout.c \
    $(SRCDIR)/keyboard_interrupt_handler.c)
endif

ifeq ($(ENABLE_PS2_MOUSE),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/mouse.c $(SRCDIR)/mouse_interrupt_handler.c)
EXCLUDED_INPUT_SRCS += $(wildcard $(SRCDIR)/input/*mouse*.c)
endif

ifeq ($(ENABLE_PS2_WATCHDOG),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/ps2_watchdog.c)
endif

ifeq ($(ENABLE_GAMEPORT),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/gameport.c)
endif

ifeq ($(ENABLE_INPUT_EVENT_SYSTEM),no)
EXCLUDED_INPUT_SRCS += $(wildcard $(SRCDIR)/input/*.c)
endif
