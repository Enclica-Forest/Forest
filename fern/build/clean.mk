# =============================================================================
# CLEANUP TARGETS  (extracted from Makefile lines 1759-1781)
# =============================================================================
# Uses OBJDIR/OUTDIR/DISTDIR from build/dirs.mk and OK_COLOR/NO_COLOR from
# build/flags.mk — all included earlier in the main Makefile. (Fern is
# kernel-only; there is no userspace object tree to clean.)
#
# CLEAN_BEFORE_BUILD hook:
# `maybe-clean-before-build` is a pre-build gate driven by the process option
# CLEAN_BEFORE_BUILD (yes|no) from build-config.mk. The main Makefile's `all`
# and `build` targets should depend on this target, e.g.
#     build: maybe-clean-before-build ensure-toolchain $(OUTPUT)
#     all:   maybe-clean-before-build ensure-toolchain show-config build
# When CLEAN_BEFORE_BUILD=yes, a full `make clean` runs before any compile.
# When CLEAN_BEFORE_BUILD!=yes (default), it is a silent no-op. This lets a
# user force a clean rebuild without remembering to run `make clean` by hand.
#
# Note: clean-all removes `obj/ build/ *.iso` verbatim (per original Makefile).
# build-config.mk is config, NOT removed. The `build/` path here is the
# original build-output tree; in the new layout OUTDIR lives under build/
# (e.g. build/32-bios-debug/), so clean-all also wipes build/*.mk fragments —
# they are source, not generated, so this is destructive. Kept verbatim per
# spec; orchestrator may later narrow to `rm -rf obj/ $(OUTDIR) *.iso`.

.PHONY: clean clean-all clean-kernel maybe-clean-before-build

clean:
	@echo "$(OK_COLOR)Cleaning build files for $(ARCH)-bit $(BOOT_MODE) $(BUILD_TYPE)...$(NO_COLOR)"
	@rm -rf $(OBJDIR) $(OUTDIR)

clean-all:
	@echo "$(OK_COLOR)Cleaning all build files...$(NO_COLOR)"
	@rm -rf obj/ build/ *.iso
	@rm -rf $(DISTDIR)

clean-kernel:
	@echo "$(OK_COLOR)Cleaning kernel objects...$(NO_COLOR)"
	@find $(OBJDIR) -name "*.o" -delete 2>/dev/null || true

# Pre-build clean gate. `all`/`build` in the main Makefile depend on this so
# CLEAN_BEFORE_BUILD=yes forces `make clean` before compiling; otherwise no-op.
maybe-clean-before-build:
ifeq ($(CLEAN_BEFORE_BUILD),yes)
	@echo "$(OK_COLOR)CLEAN_BEFORE_BUILD=yes: running clean before build...$(NO_COLOR)"
	@$(MAKE) clean
else
	@:
endif
