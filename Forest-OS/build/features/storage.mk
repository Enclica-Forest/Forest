# build/features/storage.mk
#
# Storage subsystem gating.
#
# When a storage controller / block-layer feature is disabled in
# build-config.mk (ENABLE_*=no), its corresponding source file is appended to
# EXCLUDED_CSOURCES so it is filtered out of the kernel C build (see
# build/kernel-sources.mk, CSOURCES filter-out).
#
# Mapping (each file verified to exist under $(SRCDIR)/):
#   ENABLE_ATA           -> ata.c
#   ENABLE_AHCI          -> ahci.c
#   ENABLE_NVME          -> nvme.c
#   ENABLE_SCSI          -> scsi.c
#   ENABLE_FDC           -> fdc.c
#   ENABLE_BLOCK_DEVICES -> block_devices.c
#   ENABLE_LOOP_DEVICES  -> (no loop device source file present in src/;
#                            gated out with a wildcard guard so the build
#                            stays correct if one is added later)
#
# Each gate uses $(wildcard ...) so a missing file does not break the build
# (per build-spec.md gating convention).

ifeq ($(ENABLE_ATA),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/ata.c)
endif

ifeq ($(ENABLE_AHCI),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/ahci.c)
endif

ifeq ($(ENABLE_NVME),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/nvme.c)
endif

ifeq ($(ENABLE_SCSI),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/scsi.c)
endif

ifeq ($(ENABLE_FDC),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/fdc.c)
endif

ifeq ($(ENABLE_BLOCK_DEVICES),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/block_devices.c)
endif

# ENABLE_LOOP_DEVICES: no loop device source file exists in $(SRCDIR)/ today
# (glob *loop* returned no matches). Gated with a wildcard guard so that if a
# loop device implementation is added later, flipping ENABLE_LOOP_DEVICES=no
# will drop it from the build automatically.
ifeq ($(ENABLE_LOOP_DEVICES),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/loop.c) $(wildcard $(SRCDIR)/loop_device.c) $(wildcard $(SRCDIR)/loopback_dev.c)
endif
