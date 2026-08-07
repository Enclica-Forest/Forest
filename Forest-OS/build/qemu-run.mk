# =============================================================================
# QEMU RUN TARGETS (config-driven)
# =============================================================================
# Included after build/config.mk (every QEMU_* / ENABLE_* bool is "yes"/"no",
# QEMU_MEMORY is an integer MB value) and after build/iso.mk (so $(ISO)/$(IMG)
# and the iso/img targets exist).
#
# Does NOT redefine the shared QEMU_OPTS variable (owned by build/kernel-sources.mk;
# audio/network/usb feature fragments append extra options to it).

# ---------------------------------------------------------------------------
# Per-architecture QEMU binary / machine / cpu
# ---------------------------------------------------------------------------
ifeq ($(ARCH),32)
  QEMU_BIN     := qemu-system-i386
  QEMU_MACHINE :=
  QEMU_CPU     := -cpu qemu32
else ifeq ($(ARCH),64)
  QEMU_BIN     := qemu-system-x86_64
  QEMU_MACHINE := -machine q35
  QEMU_CPU     := -cpu qemu64
else ifeq ($(ARCH),arm)
  QEMU_BIN     := qemu-system-arm
  QEMU_MACHINE := -machine virt
  QEMU_CPU     := -cpu cortex-a15
else ifeq ($(ARCH),aarch64)
  QEMU_BIN     := qemu-system-aarch64
  QEMU_MACHINE := -machine virt
  QEMU_CPU     := -cpu cortex-a53
else
  QEMU_BIN     := qemu-system-$(ARCH)
  QEMU_MACHINE :=
  QEMU_CPU     :=
endif

# ---------------------------------------------------------------------------
# Guest memory (QEMU_MEMORY in MB; build/config.mk guarantees a value)
# ---------------------------------------------------------------------------
QEMU_MEMORY ?= 512
QEMU_MEM := $(QEMU_MEMORY)M

# ---------------------------------------------------------------------------
# KVM acceleration: only when requested AND /dev/kvm is a usable char device.
# $(wildcard ...) is the make-time gate, the shell test is the runtime gate.
# ---------------------------------------------------------------------------
ifeq ($(QEMU_ENABLE_KVM),yes)
  QEMU_KVM_OPT := $(if $(wildcard /dev/kvm),$(shell [ -c /dev/kvm ] 2>/dev/null && echo -enable-kvm),)
else
  QEMU_KVM_OPT :=
endif

# ---------------------------------------------------------------------------
# Networking: pointless if the kernel itself has no networking stack
# ---------------------------------------------------------------------------
ifeq ($(QEMU_NETWORK),yes)
  ifeq ($(ENABLE_NETWORKING),yes)
    QEMU_NET_OPT := -netdev user,id=net0 -device rtl8139,netdev=net0
  else
    QEMU_NET_OPT :=
  endif
else
  QEMU_NET_OPT :=
endif

# ---------------------------------------------------------------------------
# USB
# ---------------------------------------------------------------------------
ifeq ($(QEMU_USB),yes)
  QEMU_USB_OPT := -device usb-ehci
else
  QEMU_USB_OPT :=
endif

# ---------------------------------------------------------------------------
# Sound: first enabled device wins (SB16 -> AC97 -> HDA -> ENSONIQ -> OPL3)
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_AUDIO),no)
  QEMU_SOUND_OPT :=
else ifeq ($(ENABLE_SOUND_SB16),yes)
  QEMU_SOUND_OPT := -device sb16
else ifeq ($(ENABLE_SOUND_AC97),yes)
  QEMU_SOUND_OPT := -device ac97
else ifeq ($(ENABLE_SOUND_HDA),yes)
  QEMU_SOUND_OPT := -device intel-hda -device hda-duplex
else ifeq ($(ENABLE_SOUND_ENSONIQ),yes)
  QEMU_SOUND_OPT := -device es1370
else ifeq ($(ENABLE_SOUND_OPL3),yes)
  QEMU_SOUND_OPT := -device adlib
else
  QEMU_SOUND_OPT :=
endif

# ---------------------------------------------------------------------------
# Boot media + display, per arch / boot mode.
# Use recursive (`=`) so $(ISO)/$(IMG)/$(OUTPUT) resolve at recipe time
# regardless of where this fragment is included relative to their definitions.
# ---------------------------------------------------------------------------
ifeq ($(filter arm aarch64,$(ARCH)),)
  ifeq ($(BOOT_MODE),uefi)
    QEMU_BOOT_MEDIA = -bios /usr/share/ovmf/OVMF.fd -drive format=raw,file=$(IMG)
    QEMU_RUN_DEPS   := img
  else
    # $(ISO) is a GRUB El Torito hybrid image — boots fine as a CD.
    QEMU_BOOT_MEDIA = -cdrom $(ISO)
    QEMU_RUN_DEPS   := iso
  endif
  QEMU_DISPLAY_OPT := -vga std
else
  QEMU_BOOT_MEDIA  = -kernel $(OUTPUT)
  QEMU_RUN_DEPS    := build
  QEMU_DISPLAY_OPT := -nographic
endif

# ---------------------------------------------------------------------------
# Debug stub (gdbserver on :1234, halt until attached) — enabled by `make debug`
# ---------------------------------------------------------------------------
ifdef QEMU_DEBUG
  QEMU_DEBUG_OPT := -s -S
else
  QEMU_DEBUG_OPT :=
endif

# ---------------------------------------------------------------------------
# Assembled QEMU command. $(QEMU_OPTS) carries extras from feature fragments.
# Recursive (`=`) for late binding of $(QEMU_BOOT_MEDIA) and $(QEMU_OPTS).
# ---------------------------------------------------------------------------
QEMU_CMD = $(QEMU_BIN) $(QEMU_MACHINE) $(QEMU_CPU) -m $(QEMU_MEM) \
           $(QEMU_KVM_OPT) $(QEMU_NET_OPT) $(QEMU_USB_OPT) $(QEMU_SOUND_OPT) \
           $(QEMU_OPTS) $(QEMU_BOOT_MEDIA) -serial stdio -no-shutdown \
           $(QEMU_DISPLAY_OPT) $(QEMU_DEBUG_OPT)

# ---------------------------------------------------------------------------
# Internal launcher: build the needed image, then run QEMU with current config.
# ---------------------------------------------------------------------------
.PHONY: qemu-launch
qemu-launch: $(QEMU_RUN_DEPS)
	@echo "$(OK_COLOR)Running $(ARCH_DIR_SUFFIX) $(BOOT_MODE) kernel in QEMU ($(QEMU_MEM))...$(NO_COLOR)"
	@$(QEMU_CMD)

# ---------------------------------------------------------------------------
# Public run targets — each forces the relevant ARCH/BOOT_MODE via recursion so
# the per-arch QEMU_* variables are recomputed for the chosen target.
# ---------------------------------------------------------------------------
.PHONY: run run-bios run-uefi run32 run64 runarm runaarch64 debug

ifeq ($(BOOT_MODE),uefi)
run: run-uefi
else
run: run-bios
endif

run-bios:
	@$(MAKE) ARCH=$(ARCH) BOOT_MODE=bios BUILD_TYPE=$(BUILD_TYPE) qemu-launch

run-uefi:
	@$(MAKE) ARCH=$(ARCH) BOOT_MODE=uefi BUILD_TYPE=$(BUILD_TYPE) qemu-launch

run32:
	@$(MAKE) ARCH=32 BOOT_MODE=bios BUILD_TYPE=$(BUILD_TYPE) qemu-launch

run64:
	@$(MAKE) ARCH=64 BOOT_MODE=bios BUILD_TYPE=$(BUILD_TYPE) qemu-launch

runarm:
	@$(MAKE) ARCH=arm BOOT_MODE=bios BUILD_TYPE=$(BUILD_TYPE) qemu-launch

runaarch64:
	@$(MAKE) ARCH=aarch64 BOOT_MODE=bios BUILD_TYPE=$(BUILD_TYPE) qemu-launch

debug:
	@$(MAKE) ARCH=$(ARCH) BOOT_MODE=$(BOOT_MODE) BUILD_TYPE=debug qemu-launch QEMU_DEBUG=yes

# ---------------------------------------------------------------------------
# Boot test (only when ENABLE_TESTING=yes): 60s timeout, scan serial output
# for a boot signature. Non-interactive (-display none).
# ---------------------------------------------------------------------------
QEMU_BOOT_SIGNATURE ?= Forest
QEMU_TEST_LOG       ?= /tmp/forestos-test-boot.log

ifeq ($(ENABLE_TESTING),yes)
.PHONY: test-boot
test-boot: $(QEMU_RUN_DEPS)
	@echo "$(OK_COLOR)Running boot test (60s timeout, signature=\"$(QEMU_BOOT_SIGNATURE)\")...$(NO_COLOR)"
	@rm -f $(QEMU_TEST_LOG)
	@timeout 60 $(QEMU_CMD) -display none 2>&1 | tee $(QEMU_TEST_LOG) | grep -m1 -q "$(QEMU_BOOT_SIGNATURE)" \
	    && echo "$(OK_COLOR)Boot test PASSED$(NO_COLOR)" \
	    || { echo "$(ERROR_COLOR)Boot test FAILED (see $(QEMU_TEST_LOG))$(NO_COLOR)"; exit 1; }
endif
