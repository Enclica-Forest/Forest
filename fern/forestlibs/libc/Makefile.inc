# Makefile.inc - Build configuration for Forest OS libc
#
# This file is meant to be included by the main Forest OS Makefile.
# It provides variables and rules for building the libc.

# Directory containing this Makefile.inc
FORESTLIBC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

# Include directories
FORESTLIBC_INCLUDES := -I$(FORESTLIBC_DIR)include

# Source files
FORESTLIBC_SOURCES := \
    $(FORESTLIBC_DIR)src/syscalls.c \
    $(FORESTLIBC_DIR)src/string.c \
    $(FORESTLIBC_DIR)src/stdlib.c \
    $(FORESTLIBC_DIR)src/stdio.c \
    $(FORESTLIBC_DIR)src/errno.c \
    $(FORESTLIBC_DIR)src/pthread.c \
    $(FORESTLIBC_DIR)src/signal.c \
    $(FORESTLIBC_DIR)src/dirent.c

# Object files (will be placed in the build directory)
FORESTLIBC_OBJECTS := $(patsubst $(FORESTLIBC_DIR)src/%.c,$(OBJDIR)/forestlibc/%.o,$(FORESTLIBC_SOURCES))

# Compiler flags for libc
FORESTLIBC_CFLAGS := \
    -ffreestanding \
    -fno-builtin \
    -nostdinc \
    -nostdlib \
    $(FORESTLIBC_INCLUDES)

# Additional flags for userspace builds
ifeq ($(BUILD_TARGET),userspace)
FORESTLIBC_CFLAGS += -DUSERSPACE_BUILD
endif

# Rule to compile libc source files
$(OBJDIR)/forestlibc/%.o: $(FORESTLIBC_DIR)src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FORESTLIBC_CFLAGS) -c $< -o $@

# Create a static library
$(OBJDIR)/libforest.a: $(FORESTLIBC_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# Clean target
.PHONY: clean-forestlibc
clean-forestlibc:
	rm -rf $(OBJDIR)/forestlibc
	rm -f $(OBJDIR)/libforest.a

# Phony target to build just the libc
.PHONY: forestlibc
forestlibc: $(OBJDIR)/libforest.a
	@echo "Built Forest OS libc: $<"

# Export variables for use in parent Makefile
LIBC_INCLUDES := $(FORESTLIBC_INCLUDES)
LIBC_LIBRARY := $(OBJDIR)/libforest.a
