# build/features/filesystems.mk
#
# Filesystem source gating.
#
# Owned by Agent 13 of 30. Do not edit other build/features/*.mk fragments.
#
# PARENT GATE: ENABLE_VFS. When ENABLE_VFS=no the entire FS layer is off:
#   - top-level VFS core sources (vfs.c, fs.c, fs_internal.c, ustar.c) are
#     excluded from CSOURCES;
#   - every per-filesystem source (fat/exfat/iso9660/udf/lean/yaffs/jffs2/
#     ffs_amiga/zdsfs/procfs/sysfs/devfs/device_fs*/char_devices*/ramdisk)
#     is excluded from CSOURCES;
#   - the src/fs/ subdirectory is excluded via EXCLUDED_FS_SRCS.
#
# symlink.c is intentionally LEFT ALWAYS BUILT — see the ENABLE_SYMLINKS
# block at the bottom for the rationale.
#
# SHARED VARIABLES (defined in build/kernel-sources.mk; we only append):
#   EXCLUDED_CSOURCES — gates top-level $(SRCDIR)/*.c (consumed by CSOURCES).
#   EXCLUDED_FS_SRCS  — gates $(SRCDIR)/fs/*.c      (consumed by FS_SRCS).
#
# All file references use $(wildcard ...) so that a missing source file is
# a silent no-op rather than a build break (per build-spec gating rules).
# Existence was verified at authoring time; the wildcard keeps the fragment
# robust against future file removals.

# ---------------------------------------------------------------------------
# Parent gate: ENABLE_VFS. Disables the whole filesystem layer.
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_VFS),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/vfs.c) \
    $(wildcard $(SRCDIR)/fs.c) \
    $(wildcard $(SRCDIR)/fs_internal.c) \
    $(wildcard $(SRCDIR)/ustar.c) \
    $(wildcard $(SRCDIR)/fat.c) \
    $(wildcard $(SRCDIR)/exfat.c) \
    $(wildcard $(SRCDIR)/iso9660.c) \
    $(wildcard $(SRCDIR)/udf.c) \
    $(wildcard $(SRCDIR)/lean.c) \
    $(wildcard $(SRCDIR)/yaffs.c) \
    $(wildcard $(SRCDIR)/jffs2.c) \
    $(wildcard $(SRCDIR)/ffs_amiga.c) \
    $(wildcard $(SRCDIR)/zdsfs.c) \
    $(wildcard $(SRCDIR)/procfs.c) \
    $(wildcard $(SRCDIR)/sysfs.c) \
    $(wildcard $(SRCDIR)/devfs.c) \
    $(wildcard $(SRCDIR)/device_fs.c) \
    $(wildcard $(SRCDIR)/device_fs_complete.c) \
    $(wildcard $(SRCDIR)/char_devices.c) \
    $(wildcard $(SRCDIR)/char_devices_simple.c) \
    $(wildcard $(SRCDIR)/ramdisk.c)
# src/fs/ subdirectory (currently empty; wildcard keeps this safe if files
# are added later).
EXCLUDED_FS_SRCS += $(wildcard $(SRCDIR)/fs/*.c)
# NOTE: symlink.c is NOT excluded here. kernel-sources.mk appends
# `+ $(SRCDIR)/symlink.c` to CSOURCES AFTER the filter-out, so excluding
# it would be silently overridden. Symlink support stays compiled in and
# is runtime/compile gated via -DENABLE_SYMLINKS (see bottom of file).
endif

# ---------------------------------------------------------------------------
# Per-filesystem gates. These are granular controls that only matter when
# ENABLE_VFS=yes; they are redundant (but harmless) when VFS=no because the
# parent block above already excluded the same files.
# ---------------------------------------------------------------------------

# EXT2: no standalone ext2.c exists in the tree. The ext2 implementation is
# folded into fs.c (part of the VFS core), so there is no separate source
# to gate. Disabling ENABLE_EXT2 must be handled at the C level via
# -DENABLE_EXT2, not via source exclusion.
ifeq ($(ENABLE_EXT2),no)
# ext2 logic lives inside fs.c; no standalone source to exclude.
endif

ifeq ($(ENABLE_FAT32),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/fat.c)
endif

ifeq ($(ENABLE_EXFAT),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/exfat.c)
endif

ifeq ($(ENABLE_ISO9660),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/iso9660.c)
endif

ifeq ($(ENABLE_UDF),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/udf.c)
endif

ifeq ($(ENABLE_LEAN),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/lean.c)
endif

ifeq ($(ENABLE_YAFFS),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/yaffs.c)
endif

ifeq ($(ENABLE_JFFS2),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/jffs2.c)
endif

ifeq ($(ENABLE_FFS_AMIGA),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/ffs_amiga.c)
endif

ifeq ($(ENABLE_ZDSFS),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/zdsfs.c)
endif

ifeq ($(ENABLE_PROCFS),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/procfs.c)
endif

ifeq ($(ENABLE_SYSFS),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/sysfs.c)
endif

# DEVFS: gate the devfs core plus the device-fs framework files.
ifeq ($(ENABLE_DEVFS),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/devfs.c) \
    $(wildcard $(SRCDIR)/device_fs.c) \
    $(wildcard $(SRCDIR)/device_fs_complete.c) \
    $(wildcard $(SRCDIR)/char_devices.c) \
    $(wildcard $(SRCDIR)/char_devices_simple.c)
endif

# TMPFS: no standalone tmpfs.c exists in the tree. tmpfs is implemented
# inside the VFS core (fs.c), so there is no separate source to gate.
# Disabling ENABLE_TMPFS must be handled at the C level via -DENABLE_TMPFS.
ifeq ($(ENABLE_TMPFS),no)
# No tmpfs.c in the tree; tmpfs is handled in the VFS core (fs.c).
endif

ifeq ($(ENABLE_RAMDISK),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/ramdisk.c)
endif

# ---------------------------------------------------------------------------
# Symlinks: ENABLE_SYMLINKS.
#
# kernel-sources.mk builds CSOURCES as:
#   CSOURCES = $(filter-out $(EXCLUDED_CSOURCES) ...,$(wildcard ...)) \
#              $(SRCDIR)/symlink.c
# The trailing `+ $(SRCDIR)/symlink.c` is appended AFTER the filter-out, so
# appending $(SRCDIR)/symlink.c to EXCLUDED_CSOURCES here would have NO
# effect — it would be re-added unconditionally. Editing kernel-sources.mk
# to make that append conditional is out of scope (not this fragment's file).
#
# Decision: leave symlink.c always compiled in. Symlink behavior is gated
# at compile time via the -DENABLE_SYMLINKS feature define (emitted by
# build-config.mk when ENABLE_SYMLINKS=yes) and/or at runtime by the C
# code. This is the safest option and avoids a false sense of gating.
# ---------------------------------------------------------------------------
ifeq ($(ENABLE_SYMLINKS),no)
# symlink.c remains built unconditionally (kernel-sources.mk re-adds it
# after filter-out). Symlink behavior is compile/runtime-gated via
# -DENABLE_SYMLINKS in the C source, not via source exclusion.
endif
