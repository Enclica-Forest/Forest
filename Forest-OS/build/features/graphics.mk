# build/features/graphics.mk
#
# Graphics subsystem gating. Appends to EXCLUDED_GRAPHICS_SRCS (for
# src/graphics/** files) and EXCLUDED_CSOURCES (for top-level src/*.c files).
# ENABLE_GRAPHICS is the parent toggle. Every path uses $(wildcard ...) so a
# missing file never breaks the build.

ifeq ($(ENABLE_GRAPHICS),no)
EXCLUDED_GRAPHICS_SRCS += $(wildcard $(SRCDIR)/graphics/*.c) $(wildcard $(SRCDIR)/graphics/drivers/*.c)
EXCLUDED_CSOURCES += $(wildcard \
    $(SRCDIR)/framebuffer_dbuf.c \
    $(SRCDIR)/framebuffer_mmap.c \
    $(SRCDIR)/screen.c \
    $(SRCDIR)/render_layers.c \
    $(SRCDIR)/bmp.c \
    $(SRCDIR)/splash.c \
    $(SRCDIR)/splash_conf.c \
    $(SRCDIR)/display_manager.c \
    $(SRCDIR)/mode_state.c \
    $(SRCDIR)/hotkey.c \
    $(SRCDIR)/cgdm_integration.c \
    $(SRCDIR)/clipboard.c \
    $(SRCDIR)/dragdrop.c \
    $(SRCDIR)/displayport.c \
    $(SRCDIR)/panicui.c \
    $(SRCDIR)/panicui_colors.c \
    $(SRCDIR)/panicui_effects.c \
    $(SRCDIR)/panicui_gfx.c \
    $(SRCDIR)/panicui_input.c \
    $(SRCDIR)/panicui_wm.c \
    $(SRCDIR)/wayland_compositor.c \
    $(SRCDIR)/wayland_dmabuf.c \
    $(SRCDIR)/wayland_input.c \
    $(SRCDIR)/wayland_protocol.c \
    $(SRCDIR)/wayland_server.c \
    $(SRCDIR)/wayland_shell.c \
    $(SRCDIR)/wayland_xdg.c \
    $(SRCDIR)/x11_server.c \
    $(SRCDIR)/xdg.c)
endif

ifeq ($(ENABLE_VESA),no)
EXCLUDED_GRAPHICS_SRCS += $(wildcard $(SRCDIR)/graphics/drivers/vesa*.c)
endif

ifeq ($(ENABLE_FRAMEBUFFER),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/framebuffer_dbuf.c $(SRCDIR)/framebuffer_mmap.c)
endif

ifeq ($(ENABLE_VGA_TEXT),no)
EXCLUDED_GRAPHICS_SRCS += $(wildcard $(SRCDIR)/graphics/drivers/vga_text*.c $(SRCDIR)/graphics/drivers/vga_graphics*.c)
endif

ifeq ($(ENABLE_BOCHS_BGA),no)
EXCLUDED_GRAPHICS_SRCS += $(wildcard $(SRCDIR)/graphics/drivers/bochs*.c $(SRCDIR)/graphics/drivers/bga*.c)
endif

ifeq ($(ENABLE_VMWARE_SVGA),no)
EXCLUDED_GRAPHICS_SRCS += $(wildcard $(SRCDIR)/graphics/drivers/vmware*.c $(SRCDIR)/graphics/drivers/svga*.c)
endif

ifeq ($(ENABLE_INTEL_HD),no)
EXCLUDED_GRAPHICS_SRCS += $(wildcard $(SRCDIR)/graphics/drivers/intel*.c)
endif

ifeq ($(ENABLE_NVIDIA_GPU),no)
EXCLUDED_GRAPHICS_SRCS += $(wildcard $(SRCDIR)/graphics/drivers/nvidia*.c)
endif

ifeq ($(ENABLE_AMD_GPU),no)
EXCLUDED_GRAPHICS_SRCS += $(wildcard $(SRCDIR)/graphics/drivers/amd*.c $(SRCDIR)/graphics/drivers/ati*.c)
endif

ifeq ($(ENABLE_GPU_ACCEL),no)
EXCLUDED_GRAPHICS_SRCS += $(wildcard $(SRCDIR)/graphics/gpu_accel.c $(SRCDIR)/graphics/parallel_graphics.c)
endif

ifeq ($(ENABLE_FONT_RENDERER),no)
EXCLUDED_GRAPHICS_SRCS += $(wildcard $(SRCDIR)/graphics/font_renderer.c $(SRCDIR)/graphics/font8x8.c $(SRCDIR)/graphics/tty_font_renderer.c)
endif

ifeq ($(ENABLE_TRUETYPE),no)
EXCLUDED_GRAPHICS_SRCS += $(wildcard $(SRCDIR)/graphics/truetype.c $(SRCDIR)/graphics/truetype_raster.c)
endif

ifeq ($(ENABLE_DOUBLE_BUFFERING),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/framebuffer_dbuf.c)
endif

ifeq ($(ENABLE_SPLASH_SCREEN),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/splash.c $(SRCDIR)/splash_conf.c)
endif

ifeq ($(ENABLE_PANICUI),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/panicui.c $(SRCDIR)/panicui_*.c)
endif

ifeq ($(ENABLE_DISPLAY_MANAGER),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/display_manager.c $(SRCDIR)/mode_state.c $(SRCDIR)/hotkey.c $(SRCDIR)/cgdm_integration.c)
endif

ifeq ($(ENABLE_WAYLAND_SERVER),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/wayland_*.c)
endif

ifeq ($(ENABLE_X11_SERVER),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/x11_server.c $(SRCDIR)/xdg.c)
endif

ifeq ($(ENABLE_CLIPBOARD),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/clipboard.c)
endif

ifeq ($(ENABLE_DRAG_DROP),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/dragdrop.c)
endif

ifeq ($(ENABLE_HARDWARE_DETECT),no)
EXCLUDED_GRAPHICS_SRCS += $(wildcard $(SRCDIR)/graphics/hardware_detect.c)
endif

# ENABLE_WINDOW_MANAGER / ENABLE_CANOPY_DESKTOP / ENABLE_LEAFGFX / ENABLE_VSYNC:
# handled by canopy.mk (kernel) / userspace.mk (LeafGFX). VGA_TEXT/VSync have no
# dedicated top-level file today; guards above activate if one is added.
