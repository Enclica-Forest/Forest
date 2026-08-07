# LeafGFX Library - Build Configuration
# This file is included by the main Makefile to build the LeafGFX library
# for userspace applications.

# LeafGFX source files
LEAFGFX_DIR := libs/leafgfx
LEAFGFX_SRCS := \
    $(LEAFGFX_DIR)/leafgfx.c \
    $(LEAFGFX_DIR)/leafgfx_bmp.c \
    $(LEAFGFX_DIR)/leafgfx_image.c \
    $(LEAFGFX_DIR)/leafgfx_font.c \
    $(LEAFGFX_DIR)/leafgfx_input.c

# LeafGFX object files (built into userspace object directory)
LEAFGFX_OBJECTS := $(LEAFGFX_SRCS:$(LEAFGFX_DIR)/%.c=$(USER_OBJDIR)/leafgfx_%.o)

# LeafGFX include path
LEAFGFX_INCLUDE := -I$(LEAFGFX_DIR)

# Applications that depend on LeafGFX
LEAFGFX_APPS := canopydm canopyde

# Rule for compiling LeafGFX sources
$(USER_OBJDIR)/leafgfx_%.o: $(LEAFGFX_DIR)/%.c
	@mkdir -p $(USER_OBJDIR)
	@echo "$(OK_COLOR)Compiling LeafGFX: $<...$(NO_COLOR)"
	@$(CC) $(USER_CFLAGS) $(LEAFGFX_INCLUDE) -o $@ $<
