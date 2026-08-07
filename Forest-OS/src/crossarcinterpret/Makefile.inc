# =============================================================================
# Makefile.inc - Build rules for the crossarcinterpret subsystem
#
# Include this file from the top-level Forest OS Makefile with:
#   include $(SRCDIR)/crossarcinterpret/Makefile.inc
#
# This file appends the CAI object files to ALL_OBJECTS and adds the
# necessary compile rules for each source file in the subsystem.
#
# Design constraints
# ------------------
#  - CAI sources live in $(SRCDIR)/crossarcinterpret/
#  - Object files go into $(OBJDIR)/crossarcinterpret/
#  - All CAI sources use the same CFLAGS as the rest of the kernel,
#    except stb_vorbis-style heavy files which get -O2 for size/speed.
#  - No assembly stubs: pure C.
#  - The subsystem is always compiled; there is no optional flag to
#    exclude it (use linker GC via --gc-sections if not referenced).
# =============================================================================

# ---------------------------------------------------------------------------
# Directories
# ---------------------------------------------------------------------------

CAI_SRCDIR := $(SRCDIR)/crossarcinterpret
CAI_OBJDIR := $(OBJDIR)/crossarcinterpret

# ---------------------------------------------------------------------------
# Source file list
# ---------------------------------------------------------------------------

CAI_SRCS := \
    $(CAI_SRCDIR)/crossarcinterpret.c   \
    $(CAI_SRCDIR)/cai_memory.c          \
    $(CAI_SRCDIR)/cai_elf_loader.c      \
    $(CAI_SRCDIR)/cai_syscall_bridge.c  \
    $(CAI_SRCDIR)/cai_x86_32.c         \
    $(CAI_SRCDIR)/cai_x86_64.c         \
    $(CAI_SRCDIR)/cai_arm32.c           \
    $(CAI_SRCDIR)/cai_aarch64.c

# ---------------------------------------------------------------------------
# Object file list (mirrors source list, placed in CAI_OBJDIR)
# ---------------------------------------------------------------------------

CAI_OBJS := $(patsubst $(CAI_SRCDIR)/%.c,$(CAI_OBJDIR)/%.o,$(CAI_SRCS))

# ---------------------------------------------------------------------------
# Append to the master object list so the kernel link picks them up
# ---------------------------------------------------------------------------

ALL_OBJECTS += $(CAI_OBJS)

# ---------------------------------------------------------------------------
# Compile rules
# ---------------------------------------------------------------------------

# Ensure the output directory exists before any object file is built
$(CAI_OBJDIR):
	@mkdir -p $@

# Generic rule for all CAI C sources
$(CAI_OBJDIR)/%.o: $(CAI_SRCDIR)/%.c | $(CAI_OBJDIR)
	@echo "$(INFO_COLOR)  CAI  $<$(NO_COLOR)"
	@$(CC) $(CFLAGS) -I$(SRCDIR) -I$(SRCDIR)/include \
	       -c -o $@ $<

# ---------------------------------------------------------------------------
# Clean helper (hook into the parent Makefile's clean target if desired)
# ---------------------------------------------------------------------------

.PHONY: cai-clean
cai-clean:
	@rm -rf $(CAI_OBJDIR)
	@echo "Removed $(CAI_OBJDIR)"
