# =============================================================================
# build/userspace.mk — Userspace build (config-driven)
# Included AFTER build/features/*.mk so USERSPACE_EXCLUDED is fully populated.
# Gates on USERSPACE_* and CANOPY_* options (yes/no).
# =============================================================================

# -----------------------------------------------------------------------------
# Userspace paths and libc objects
# -----------------------------------------------------------------------------
USER_OBJDIR := $(OBJDIR)/userspace
USER_LIBC_SRCS := $(wildcard userspace/libc/*.c)
USER_LIBC_OBJECTS := $(USER_LIBC_SRCS:userspace/libc/%.c=$(USER_OBJDIR)/libc_%.o)
USER_SUPPORT_OBJECTS := $(USER_LIBC_OBJECTS)

# -----------------------------------------------------------------------------
# Userspace app source selection
#   1. Start from all userspace/*.c
#   2. Drop crt0.c (asm entry) and session_config.c (library, not an app)
#   3. Apply USERSPACE_EXCLUDED (appended by feature fragments for
#      USERSPACE_* options)
#   4. Apply existing canopy/userspace gating conditional blocks
# -----------------------------------------------------------------------------
USER_APP_SRCS := $(filter-out userspace/crt0.c userspace/session_config.c,$(wildcard $(USER_SRCDIR)/*.c))
USER_APP_SRCS := $(filter-out $(USERSPACE_EXCLUDED),$(USER_APP_SRCS))

# Conditionally exclude Canopy components based on build-config.mk flags
ifeq ($(CANOPY_DM_ENABLE),no)
  USER_APP_SRCS := $(filter-out userspace/canopydm.c,$(USER_APP_SRCS))
endif
ifeq ($(CANOPY_DE_ENABLE),no)
  USER_APP_SRCS := $(filter-out userspace/canopyde.c userspace/canopywm.c userspace/canopyctl.c,$(USER_APP_SRCS))
  USER_APP_SRCS := $(filter-out $(wildcard userspace/canopy_app_*.c),$(USER_APP_SRCS))
endif
ifeq ($(USERSPACE_DESKTOP_ENV),no)
  USER_APP_SRCS := $(filter-out userspace/canopydm.c userspace/canopyde.c userspace/canopywm.c userspace/canopyctl.c,$(USER_APP_SRCS))
  USER_APP_SRCS := $(filter-out $(wildcard userspace/canopy_app_*.c),$(USER_APP_SRCS))
endif
ifeq ($(USERSPACE_DESKTOP_APPS),no)
  USER_APP_SRCS := $(filter-out $(wildcard userspace/canopy_app_*.c),$(USER_APP_SRCS))
endif

USER_APPS := $(basename $(notdir $(USER_APP_SRCS)))
USER_APP_OBJECTS := $(USER_APPS:%=$(USER_OBJDIR)/%.o)

# Apps that use LeafGFX and need special linking
LEAFGFX_APPS := canopydm canopyde test_mouse test_3d_acceleration
USER_ELFS := $(filter-out $(LEAFGFX_APPS:%=$(USER_OBJDIR)/%.elf),$(USER_APPS:%=$(USER_OBJDIR)/%.elf))
USER_PRIMARY_APP := shell
USER_PRIMARY_ELF := $(USER_OBJDIR)/$(USER_PRIMARY_APP).elf
USER_ELF_BIN := $(OBJDIR)/$(USER_PRIMARY_APP)_elf.o
# Installed binaries in the initrd carry no extension (matches Unix
# convention); the internal linked .elf artifacts above are renamed on
# install by the $(INITRD_BIN_DIR)/%: $(USER_OBJDIR)/%.elf rule below.
USER_APP_BINARIES := $(USER_APPS:%=$(INITRD_BIN_DIR)/%)

# Conditionally include GUI required binaries
GUI_REQUIRED_BINARIES :=
ifneq ($(CANOPY_DM_ENABLE),no)
  GUI_REQUIRED_BINARIES += $(INITRD_BIN_DIR)/canopydm
endif
ifneq ($(CANOPY_DE_ENABLE),no)
  GUI_REQUIRED_BINARIES += $(INITRD_BIN_DIR)/canopyde
endif
GUI_REQUIRED_ASSETS := \
	$(INITRD_DIR)/usr/share/images/background/login.bmp \
	$(INITRD_DIR)/usr/share/images/bootup/logo.png \
	$(INITRD_DIR)/usr/share/desktop/defaults.conf \
	$(INITRD_DIR)/usr/share/sysconf/sys.conf

# Additional test ELF binary for advanced testing
USER_ELF_TEST := $(USER_OBJDIR)/elf_test.elf

# -----------------------------------------------------------------------------
# Userspace compiler flags
# Userspace uses same cross-compiler as kernel for consistency.
# Note: sysroot include path is listed FIRST so <forestos/syscalls.h> resolves
#       to the sysroot version, avoiding conflicts with src/include/syscall.h
# -----------------------------------------------------------------------------
USER_CFLAGS := $(ARCH_FLAGS) -c -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
               -Wall -Wextra -g -O0 \
               -I$(FORESTOS_TOOLCHAIN_DIR)/sysroot/usr/include \
               -I$(SRCDIR)/include -I$(LIBC_DIR)/include -I$(USER_SRCDIR)/include \
               -Ilibs/leafgfx \
               -fno-pic -fno-pie -DUSERSPACE_BUILD -mno-sse -mno-sse2 -mno-mmx -mno-3dnow

# -----------------------------------------------------------------------------
# LeafGFX userspace graphics library
# -----------------------------------------------------------------------------
LEAFGFX_DIR := libs/leafgfx
LEAFGFX_SRCS := $(LEAFGFX_DIR)/leafgfx.c $(LEAFGFX_DIR)/leafgfx_bmp.c $(LEAFGFX_DIR)/leafgfx_image.c \
                $(LEAFGFX_DIR)/leafgfx_font.c $(LEAFGFX_DIR)/leafgfx_input.c \
                $(LEAFGFX_DIR)/leafgfx_ttf.c \
                $(LEAFGFX_DIR)/leafgfx_ttf_raster.c $(LEAFGFX_DIR)/leafgfx_anim.c
LEAFGFX_OBJECTS := $(LEAFGFX_SRCS:$(LEAFGFX_DIR)/%.c=$(USER_OBJDIR)/leafgfx_%.o)

# -----------------------------------------------------------------------------
# Userspace linker script selection (per ARCH)
# -----------------------------------------------------------------------------
ifeq ($(ARCH),32)
    USER_LINKER_SCRIPT := userspace/link.ld
else ifeq ($(ARCH),64)
    USER_LINKER_SCRIPT := userspace/link64.ld
else ifeq ($(ARCH),arm)
    USER_LINKER_SCRIPT := userspace/link.ld
else ifeq ($(ARCH),aarch64)
    USER_LINKER_SCRIPT := userspace/link64.ld
endif

# Userspace uses cross-compiler linker with proper sysroot
USER_LDFLAGS := $(ARCH_LDFLAGS) -nostdlib -T $(USER_LINKER_SCRIPT) \
                --sysroot=$(FORESTOS_TOOLCHAIN_DIR)/sysroot

# =============================================================================
# USERSPACE BUILD RULES
# =============================================================================

$(USER_OBJDIR)/%.o: $(USER_SRCDIR)/%.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling userspace $<...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -o $@ $<

$(USER_OBJDIR)/libc_%.o: userspace/libc/%.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling userspace libc $<...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -I$(SRCDIR)/include -o $@ $<

# Assembly-based crt0 for minimal, correct userspace entry
# Note: Uses USER_ASM_FLAGS instead of ARCH_FLAGS to avoid -mcmodel=kernel
ifeq ($(ARCH),64)
    USER_ASM_FLAGS := -m64 -ffreestanding -nostdlib
    USER_CRT0_SRC := userspace/crt0_64.S
else ifeq ($(ARCH),arm)
    USER_ASM_FLAGS := $(ARCH_FLAGS) -ffreestanding -nostdlib
    USER_CRT0_SRC := userspace/crt0.S
else ifeq ($(ARCH),aarch64)
    USER_ASM_FLAGS := $(ARCH_FLAGS) -ffreestanding -nostdlib
    USER_CRT0_SRC := userspace/crt0.S
else
    USER_ASM_FLAGS := -m32 -ffreestanding -nostdlib
    USER_CRT0_SRC := userspace/crt0_x86_32.S
endif

$(USER_OBJDIR)/crt0.o: $(USER_CRT0_SRC)
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Assembling userspace crt0 ($(ARCH)-bit)...$(NO_COLOR)"
	@$(CC) $(USER_ASM_FLAGS) -c -o $@ $<

# LeafGFX library compilation
$(USER_OBJDIR)/leafgfx_%.o: $(LEAFGFX_DIR)/%.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling LeafGFX: $<...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -o $@ $<

# Session config library (shared between canopydm and canopyde)
$(USER_OBJDIR)/session_config.o: userspace/session_config.c userspace/session_config.h
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling session_config...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -o $@ $<

# CanopyDM modular sources (conditional)
ifneq ($(CANOPY_DM_ENABLE),no)
CANOPYDM_DIR := userspace/canopydm
CANOPYDM_SRCS := $(CANOPYDM_DIR)/canopydm.c
CANOPYDM_OBJECTS := $(CANOPYDM_SRCS:$(CANOPYDM_DIR)/%.c=$(USER_OBJDIR)/canopydm_%.o)
endif

# CanopyDE modular sources (LeafGFX build, conditional)
ifneq ($(CANOPY_DE_ENABLE),no)
CANOPYDE_DIR := userspace/canopyde
CANOPYDE_SRCS := $(CANOPYDE_DIR)/canopy.c \
                 $(CANOPYDE_DIR)/compositor/canopy_compositor.c \
                 $(CANOPYDE_DIR)/compositor/canopy_blur.c \
                 $(CANOPYDE_DIR)/render/canopy_render.c \
                 $(CANOPYDE_DIR)/render/canopy_shadow.c \
                 $(CANOPYDE_DIR)/theme/canopy_theme_tokens.c \
                 $(CANOPYDE_DIR)/de/canopy_de.c \
                 $(CANOPYDE_DIR)/de/canopy_panel.c \
                 $(CANOPYDE_DIR)/de/canopy_dock.c \
                 $(CANOPYDE_DIR)/de/canopy_overview.c \
                 $(CANOPYDE_DIR)/de/canopy_notifications.c \
                 $(CANOPYDE_DIR)/de/canopy_control_center.c \
                 $(CANOPYDE_DIR)/de/canopy_sleep_screen.c \
                 $(CANOPYDE_DIR)/de/canopy_sounds.c \
                 $(CANOPYDE_DIR)/de/canopy_hybrid_de.c \
                 $(CANOPYDE_DIR)/de/canopy_hybrid_panel.c \
                 $(CANOPYDE_DIR)/de/canopy_hybrid_dock.c \
                 $(CANOPYDE_DIR)/de/canopy_a11y.c \
                 $(CANOPYDE_DIR)/de/canopy_settings.c \
                 $(CANOPYDE_DIR)/wm/canopy_wm.c \
                 $(CANOPYDE_DIR)/wm/canopy_decorations.c \
                 $(CANOPYDE_DIR)/wm/canopy_snap.c \
                 $(CANOPYDE_DIR)/widgets/canopy_widget.c \
                 $(CANOPYDE_DIR)/widgets/canopy_button.c \
                 $(CANOPYDE_DIR)/widgets/canopy_label.c \
                 $(CANOPYDE_DIR)/widgets/canopy_slider.c \
                 $(CANOPYDE_DIR)/widgets/canopy_toggle.c \
                 $(CANOPYDE_DIR)/widgets/canopy_textinput.c \
                 $(CANOPYDE_DIR)/apps/canopy_apps.c \
                 $(CANOPYDE_DIR)/apps/canopy_launcher.c
CANOPYDE_OBJECTS := $(patsubst $(CANOPYDE_DIR)/%.c,$(USER_OBJDIR)/canopyde_%.o,$(subst /,_,$(CANOPYDE_SRCS)))
endif

# Compile CanopyDM modules (conditional)
ifneq ($(CANOPY_DM_ENABLE),no)
$(USER_OBJDIR)/canopydm_%.o: $(CANOPYDM_DIR)/%.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling CanopyDM module: $<...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -o $@ $<
endif

# Compile CanopyDE modules (conditional)
ifneq ($(CANOPY_DE_ENABLE),no)
$(USER_OBJDIR)/canopyde_canopy.o: $(CANOPYDE_DIR)/canopy.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling CanopyDE: canopy.c...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -o $@ $<

$(USER_OBJDIR)/canopyde_compositor_%.o: $(CANOPYDE_DIR)/compositor/%.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling CanopyDE compositor: $<...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -o $@ $<

$(USER_OBJDIR)/canopyde_render_%.o: $(CANOPYDE_DIR)/render/%.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling CanopyDE render: $<...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -o $@ $<

$(USER_OBJDIR)/canopyde_theme_%.o: $(CANOPYDE_DIR)/theme/%.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling CanopyDE theme: $<...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -o $@ $<

$(USER_OBJDIR)/canopyde_de_%.o: $(CANOPYDE_DIR)/de/%.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling CanopyDE de: $<...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -o $@ $<

$(USER_OBJDIR)/canopyde_wm_%.o: $(CANOPYDE_DIR)/wm/%.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling CanopyDE wm: $<...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -o $@ $<

$(USER_OBJDIR)/canopyde_widgets_%.o: $(CANOPYDE_DIR)/widgets/%.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling CanopyDE widget: $<...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -o $@ $<

$(USER_OBJDIR)/canopyde_apps_%.o: $(CANOPYDE_DIR)/apps/%.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling CanopyDE apps: $<...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) -o $@ $<
endif

# CanopyDE object list (explicit, conditional)
ifneq ($(CANOPY_DE_ENABLE),no)
CANOPYDE_OBJ_LIST := $(USER_OBJDIR)/canopyde_canopy.o \
                     $(USER_OBJDIR)/canopyde_compositor_canopy_compositor.o \
                     $(USER_OBJDIR)/canopyde_compositor_canopy_blur.o \
                     $(USER_OBJDIR)/canopyde_render_canopy_render.o \
                     $(USER_OBJDIR)/canopyde_render_canopy_shadow.o \
                     $(USER_OBJDIR)/canopyde_theme_canopy_theme_tokens.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_de.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_panel.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_dock.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_overview.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_notifications.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_control_center.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_sleep_screen.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_sounds.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_hybrid_de.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_hybrid_panel.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_hybrid_dock.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_a11y.o \
                     $(USER_OBJDIR)/canopyde_de_canopy_settings.o \
                     $(USER_OBJDIR)/canopyde_wm_canopy_wm.o \
                     $(USER_OBJDIR)/canopyde_wm_canopy_decorations.o \
                     $(USER_OBJDIR)/canopyde_wm_canopy_snap.o \
                     $(USER_OBJDIR)/canopyde_widgets_canopy_widget.o \
                     $(USER_OBJDIR)/canopyde_widgets_canopy_button.o \
                     $(USER_OBJDIR)/canopyde_widgets_canopy_label.o \
                     $(USER_OBJDIR)/canopyde_widgets_canopy_slider.o \
                     $(USER_OBJDIR)/canopyde_widgets_canopy_toggle.o \
                     $(USER_OBJDIR)/canopyde_widgets_canopy_textinput.o \
                     $(USER_OBJDIR)/canopyde_apps_canopy_apps.o \
                     $(USER_OBJDIR)/canopyde_apps_canopy_launcher.o
endif

# Special linking rules for LeafGFX-dependent apps (conditional)
ifneq ($(CANOPY_DM_ENABLE),no)
$(USER_OBJDIR)/canopydm.elf: $(USER_OBJDIR)/canopydm.o $(CANOPYDM_OBJECTS) $(USER_OBJDIR)/session_config.o $(USER_SUPPORT_OBJECTS) $(LEAFGFX_OBJECTS) $(USER_OBJDIR)/crt0.o $(USER_LINKER_SCRIPT)
	@echo "$(OK_COLOR)Linking LeafGFX app: canopydm.elf...$(NO_COLOR)"
	@$(LD) $(USER_LDFLAGS) -o $@ \
	       $(USER_OBJDIR)/crt0.o $(USER_OBJDIR)/canopydm.o $(CANOPYDM_OBJECTS) $(USER_OBJDIR)/session_config.o $(LEAFGFX_OBJECTS) $(USER_SUPPORT_OBJECTS)
endif

ifneq ($(CANOPY_DE_ENABLE),no)
# Special linking rules for canopyDE (LeafGFX build)
$(USER_OBJDIR)/canopyde.elf: $(USER_OBJDIR)/canopyde.o $(CANOPYDE_OBJ_LIST) $(USER_OBJDIR)/session_config.o $(USER_SUPPORT_OBJECTS) $(LEAFGFX_OBJECTS) $(USER_OBJDIR)/crt0.o $(USER_LINKER_SCRIPT)
	@echo "$(OK_COLOR)Linking LeafGFX app: canopyde.elf...$(NO_COLOR)"
	@$(LD) $(USER_LDFLAGS) -o $@ \
	       $(USER_OBJDIR)/crt0.o $(USER_OBJDIR)/canopyde.o $(CANOPYDE_OBJ_LIST) $(USER_OBJDIR)/session_config.o $(LEAFGFX_OBJECTS) $(USER_SUPPORT_OBJECTS)

# Special linking rule for Canopy desktop apps
$(USER_OBJDIR)/canopy_app_%.elf: $(USER_OBJDIR)/canopy_app_%.o $(USER_OBJDIR)/session_config.o $(USER_SUPPORT_OBJECTS) $(LEAFGFX_OBJECTS) $(USER_OBJDIR)/crt0.o $(USER_LINKER_SCRIPT)
	@echo "$(OK_COLOR)Linking LeafGFX app: $(@F)...$(NO_COLOR)"
	@$(LD) $(USER_LDFLAGS) -o $@ \
	       $(USER_OBJDIR)/crt0.o $(USER_OBJDIR)/canopy_app_$*.o $(USER_OBJDIR)/session_config.o $(LEAFGFX_OBJECTS) $(USER_SUPPORT_OBJECTS)
endif

$(USER_OBJDIR)/test_mouse.elf: $(USER_OBJDIR)/test_mouse.o $(USER_SUPPORT_OBJECTS) $(LEAFGFX_OBJECTS) $(USER_OBJDIR)/crt0.o $(USER_LINKER_SCRIPT)
	@echo "$(OK_COLOR)Linking LeafGFX app: test_mouse.elf...$(NO_COLOR)"
	@$(LD) $(USER_LDFLAGS) -o $@ \
	       $(USER_OBJDIR)/crt0.o $(USER_OBJDIR)/test_mouse.o $(LEAFGFX_OBJECTS) $(USER_SUPPORT_OBJECTS)

$(USER_OBJDIR)/test_3d_acceleration.elf: $(USER_OBJDIR)/test_3d_acceleration.o $(USER_SUPPORT_OBJECTS) $(LEAFGFX_OBJECTS) $(USER_OBJDIR)/crt0.o $(USER_LINKER_SCRIPT)
	@echo "$(OK_COLOR)Linking LeafGFX app: test_3d_acceleration.elf...$(NO_COLOR)"
	@$(LD) $(USER_LDFLAGS) -o $@ \
	       $(USER_OBJDIR)/crt0.o $(USER_OBJDIR)/test_3d_acceleration.o $(LEAFGFX_OBJECTS) $(USER_SUPPORT_OBJECTS)

$(USER_ELFS): $(USER_SUPPORT_OBJECTS) $(USER_OBJDIR)/crt0.o
$(USER_OBJDIR)/%.elf: $(USER_OBJDIR)/%.o $(USER_SUPPORT_OBJECTS) $(USER_OBJDIR)/crt0.o $(USER_LINKER_SCRIPT)
	@echo "$(OK_COLOR)Linking userspace ELF $(@F)...$(NO_COLOR)"
	@$(LD) $(USER_LDFLAGS) -o $@ \
	       $(USER_OBJDIR)/crt0.o $(USER_OBJDIR)/$*.o $(USER_SUPPORT_OBJECTS)

$(USER_ELF_BIN): $(USER_PRIMARY_ELF)
	@echo "$(OK_COLOR)Embedding $(USER_PRIMARY_APP) ELF into kernel...$(NO_COLOR)"
	@$(LD) $(ARCH_LDFLAGS) -r -b binary -o $@ $<

$(USER_ELF_TEST): $(USER_OBJDIR)/elf_test.o $(USER_SUPPORT_OBJECTS) $(USER_OBJDIR)/crt0.o
	@echo "$(OK_COLOR)Building test ELF binary...$(NO_COLOR)"
	@$(LD) $(USER_LDFLAGS) -o $@ \
	       $(USER_OBJDIR)/crt0.o $(USER_OBJDIR)/elf_test.o $(USER_SUPPORT_OBJECTS)

# =============================================================================
# INITRD AND LIBRARY BUILD RULES (userspace-initrd)
# =============================================================================

.PHONY: refresh-libc refresh-forestcore prepare-canopy-icons

refresh-libc: refresh-forestcore
	@echo "$(OK_COLOR)Refreshing exported libc sources...$(NO_COLOR)"
	@rm -rf $(LIBC_DIR)/include/libc
	@mkdir -p $(LIBC_DIR)/include
	@cp -r $(SRCDIR)/include/libc $(LIBC_DIR)/include/

refresh-forestcore:
	@echo "$(OK_COLOR)Refreshing ForestCore runtime exports...$(NO_COLOR)"
	@mkdir -p $(FORESTCORE_DIR)/src $(FORESTCORE_DIR)/include
	@rm -f $(FORESTCORE_DIR)/src/*.c $(FORESTCORE_DIR)/include/*.h
	@cp $(SRCDIR)/string.c $(SRCDIR)/util.c $(SRCDIR)/system.c $(SRCDIR)/audio.c $(FORESTCORE_DIR)/src/
	@cp $(SRCDIR)/include/types.h $(SRCDIR)/include/util.h $(SRCDIR)/include/string.h \
	    $(SRCDIR)/include/system.h $(SRCDIR)/include/net.h $(SRCDIR)/include/driver.h $(FORESTCORE_DIR)/include/

prepare-canopy-icons:
	@echo "$(OK_COLOR)Preparing Canopy desktop icons...$(NO_COLOR)"
	@./tools/prepare-canopy-icons.sh

.PHONY: verify-gui-runtime
verify-gui-runtime: $(GUI_REQUIRED_BINARIES) prepare-canopy-icons
	@echo "$(OK_COLOR)Verifying GUI runtime binaries/assets...$(NO_COLOR)"
ifneq ($(CANOPY_DM_ENABLE),no)
	@test -f $(INITRD_BIN_DIR)/canopydm
	@grep -Eq '^(DM|dm)=/bin/canopydm$$' $(INITRD_DIR)/usr/share/sysconf/sys.conf
endif
ifneq ($(CANOPY_DE_ENABLE),no)
	@test -f $(INITRD_BIN_DIR)/canopyde
	@grep -Eq '^(DE|de|desktop)=/bin/canopyde$$' $(INITRD_DIR)/usr/share/sysconf/sys.conf
endif
	@test -f $(INITRD_DIR)/usr/share/images/background/login.bmp
	@test -f $(INITRD_DIR)/usr/share/images/bootup/logo.png
	@test -f $(INITRD_DIR)/usr/share/images/icons/canopy/files.bmp
	@test -f $(INITRD_DIR)/usr/share/images/icons/canopy/terminal.bmp
	@test -f $(INITRD_DIR)/usr/share/images/icons/canopy/settings.bmp
	@test -f $(INITRD_DIR)/usr/share/images/icons/canopy/notes.bmp
	@test -f $(INITRD_DIR)/usr/share/images/icons/canopy/monitor.bmp

$(INITRD): refresh-libc prepare-canopy-icons verify-gui-runtime $(OUTPUT) $(INITRD_FILES) $(USER_APP_BINARIES)
	@mkdir -p $(OUTDIR)/boot
	@echo "$(OK_COLOR)Copying libc into initrd...$(NO_COLOR)"
	@rm -rf $(INITRD_DIR)/usr/libc
	@mkdir -p $(INITRD_DIR)/usr/libc
	@cp -r $(LIBC_DIR)/. $(INITRD_DIR)/usr/libc/
	@echo "$(OK_COLOR)Building initrd tar archive...$(NO_COLOR)"
	@tar --format=ustar --exclude='.gitkeep' -cf $@ -C $(INITRD_DIR) .

$(INITRD_BIN_DIR)/%: $(USER_OBJDIR)/%.elf
	@mkdir -p $(INITRD_BIN_DIR) $(INITRD_USR_BIN_DIR)
	@echo "$(OK_COLOR)Installing $(@F) into initrd...$(NO_COLOR)"
	@cp $< $(INITRD_BIN_DIR)/$*
	@cp $< $(INITRD_USR_BIN_DIR)/$*
