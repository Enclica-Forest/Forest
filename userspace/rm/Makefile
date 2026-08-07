# Forest OS - rm userspace app
# Remove files and directories

# Toolchain (overridden by parent Makefile)
CC      ?= $(error Cross-compiler not found - build via userspace/Makefile)
LD ?= ld
STRIP ?= strip

# Directories
BUILDDIR ?= build
BINDIR ?= $(BUILDDIR)/bin

# Flags (appended to by parent Makefile)
CFLAGS ?= -m32 -march=i386 -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pie -fno-pic -Wall -Wextra -g -O0 \
    -mno-sse -mno-sse2 -mno-mmx
LDFLAGS ?= -m elf_i386 -nostdlib

# Include paths
USERSPACE_DIR := $(abspath $(CURDIR)/..)
CFLAGS += -I$(USERSPACE_DIR)/include

# Source and output
SRC := rm.c
OBJ := $(BUILDDIR)/rm.o
BIN := $(BINDIR)/rm

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ) | $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ $< $(EXTRA_OBJS) $(LIBS)

$(OBJ): $(SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

clean:
	rm -rf $(BUILDDIR)
