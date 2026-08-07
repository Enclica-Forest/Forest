# build/features/timers.mk
#
# Timer subsystem gating.
#
# When a timer / time-keeping feature is disabled in build-config.mk
# (ENABLE_*=no), its corresponding source file is appended to
# EXCLUDED_CSOURCES so it is filtered out of the kernel C build (see
# build/kernel-sources.mk, CSOURCES filter-out).
#
# Mapping (each file verified to exist under $(SRCDIR)/):
#   ENABLE_PIT               -> pit.c
#   ENABLE_HPET              -> hpet.c
#   ENABLE_APIC_TIMER        -> apic_timer.c
#   ENABLE_CMOS_RTC          -> cmos_rtc.c, rtc.c
#   ENABLE_TSC               -> tsc_calibration.c, timer_calibration.c
#   ENABLE_TIMER_ABSTRACTION -> timer_abstraction.c, timer_dev.c,
#                               time_enhanced.c   (see NOTE below)
#   ENABLE_EPOCH_TIME        -> epoch.c
#
# NOTE on ENABLE_TIMER_ABSTRACTION: the spec mapping also lists timer.c
# under this gate, but timer.c is the CORE timer interrupt handler
# (timer_handler / timer_sleep_ms / timer_shutdown — IRQ0 entry used by
# the scheduler). Excluding it breaks the kernel build. We therefore KEEP
# timer.c and only exclude the optional abstraction/enhancement layer:
# timer_abstraction.c, timer_dev.c, time_enhanced.c. time_enhanced.c is
# the "Enhanced Time Management" layer (network time sync, alarms,
# multiple time sources) and is safe to drop.
#
# Each gate uses $(wildcard ...) so a missing file does not break the
# build (per build-spec.md gating convention).

# ---------------------------------------------------------------------------
# ENABLE_PIT: Programmable Interval Timer (8253/8254).
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_PIT),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/pit.c)
endif

# ---------------------------------------------------------------------------
# ENABLE_HPET: High Precision Event Timer.
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_HPET),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/hpet.c)
endif

# ---------------------------------------------------------------------------
# ENABLE_APIC_TIMER: Local APIC timer (per-CPU timer for SMP scheduling).
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_APIC_TIMER),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/apic_timer.c)
endif

# ---------------------------------------------------------------------------
# ENABLE_CMOS_RTC: CMOS / real-time clock.
# cmos_rtc.c is the hardware driver; rtc.c is the RTC core interface.
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_CMOS_RTC),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/cmos_rtc.c) $(wildcard $(SRCDIR)/rtc.c)
endif

# ---------------------------------------------------------------------------
# ENABLE_TSC: TSC-based timekeeping / calibration.
# tsc_calibration.c calibrates the TSC against a reference; timer_calibration.c
# is the generic timer-calibration helper.
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_TSC),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/tsc_calibration.c) $(wildcard $(SRCDIR)/timer_calibration.c)
endif

# ---------------------------------------------------------------------------
# ENABLE_TIMER_ABSTRACTION: unified timer-abstraction layer + enhanced time.
# timer.c is intentionally NOT excluded here — it is the core IRQ0 handler.
# See the header NOTE for rationale.
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_TIMER_ABSTRACTION),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/timer_abstraction.c) \
    $(wildcard $(SRCDIR)/timer_dev.c) \
    $(wildcard $(SRCDIR)/time_enhanced.c)
endif

# ---------------------------------------------------------------------------
# ENABLE_EPOCH_TIME: UNIX-epoch time conversion / wall-clock support.
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_EPOCH_TIME),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/epoch.c)
endif
