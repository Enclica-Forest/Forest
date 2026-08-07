# Forest OS Userspace Applications - Master Makefile
# Builds all userspace apps + initrd builder
#
# Requires the Forest-OS cross-toolchain. Host gcc is NOT used.
# Set FORESTOS_TOOLCHAIN_DIR to override the default toolchain location.

# Toolchain detection — Forest-OS cross-compiler only
FORESTOS_TOOLCHAIN_DIR ?= $(abspath $(CURDIR)/../forestos-toolchain)

# Architecture: defaults to 32, override with ARCH=64
ARCH ?= 32

ifeq ($(ARCH),64)
    FORESTOS_TOOLCHAIN_PREFIX := x86_64-forestos
    ARCH_FLAGS := -m64 -march=x86-64
    ARCH_LDFLAGS := -m elf_x86_64
    LINK_SCRIPT := $(CURDIR)/link64.ld
else
    FORESTOS_TOOLCHAIN_PREFIX := i686-forestos
    ARCH_FLAGS := -m32 -march=i386
    ARCH_LDFLAGS := -m elf_i386
    LINK_SCRIPT := $(CURDIR)/link.ld
endif

TOOLCHAIN_BIN := $(FORESTOS_TOOLCHAIN_DIR)/install/bin

# Require the Forest-OS cross-compiler — do NOT fall back to host gcc
ifeq ($(wildcard $(TOOLCHAIN_BIN)/$(FORESTOS_TOOLCHAIN_PREFIX)-gcc),)
    $(error Forest-OS cross-toolchain not found at $(TOOLCHAIN_BIN)/$(FORESTOS_TOOLCHAIN_PREFIX)-gcc. \
            Build it first: cd forestos-toolchain && ./build-toolchain.sh --arch $(ARCH))
endif

CC   := $(TOOLCHAIN_BIN)/$(FORESTOS_TOOLCHAIN_PREFIX)-gcc
LD   := $(TOOLCHAIN_BIN)/$(FORESTOS_TOOLCHAIN_PREFIX)-ld
STRIP := $(TOOLCHAIN_BIN)/$(FORESTOS_TOOLCHAIN_PREFIX)-strip

# Common flags
CFLAGS := $(ARCH_FLAGS) \
    -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
    -fno-pie -fno-pic -Wall -Wextra -g -O0 \
    -I$(CURDIR)/include \
    -I$(CURDIR)/../libs/libc/include/libc \
    -isystem $(FORESTOS_TOOLCHAIN_DIR)/sysroot/usr/include \
    -isystem $(CURDIR)/../libs/forestcore/include \
    -DUSERSPACE_BUILD \
    -mno-sse -mno-sse2 -mno-mmx

# Build directory
BUILDDIR := build
BINDIR := $(BUILDDIR)/bin

# C runtime startup object
CRT0 := $(BUILDDIR)/crt0.o

# Stubs for missing libc functions
STUBS_OBJ := $(BUILDDIR)/forest_stubs.o
STUBS_SRC := include/forest_stubs.c

# Linker flags: linker script only (objects and libs appended by sub-Makefiles)
SYSROOT_LIB := $(FORESTOS_TOOLCHAIN_DIR)/sysroot/lib
LDFLAGS := $(ARCH_LDFLAGS) -nostdlib --allow-multiple-definition -T $(abspath $(LINK_SCRIPT))
EXTRA_OBJS := $(abspath $(CRT0)) $(abspath $(STUBS_OBJ))
LIBS := -L$(SYSROOT_LIB) -lc $(abspath $(FORESTOS_TOOLCHAIN_DIR)/install/lib/gcc/$(FORESTOS_TOOLCHAIN_PREFIX)/13.2.0/libgcc.a)

# All app directories (cross-compiled)
APPS := \
    cat echo ls cp mv rm mkdir rmdir touch chmod chown ln pwd basename dirname \
    grep find sort wc head tail dd df du \
    fdisk mkfs mount umount blkid losetup fsck \
    ps kill init shutdown reboot hostname uname date sleep id \
    su sudo \
    forest-shell forest-x11

# All binaries produced
APP_BINS := $(addprefix $(BINDIR)/,$(APPS))

# Host tools
HOST_CC := gcc
HOST_CFLAGS := -Wall -Wextra -O2
INITRD_BUILDER_SRC := initrd-builder/initrd_builder.c
INITRD_BUILDER := $(BUILDDIR)/initrd-builder

.PHONY: all clean install initrd initrd-builder verify $(APPS)

all: $(APPS) initrd-builder

# Build C runtime startup
$(CRT0): crt0.S
	@mkdir -p $(BUILDDIR)
	$(CC) $(ARCH_FLAGS) -c $< -o $@

# Build stubs for missing libc functions
$(STUBS_OBJ): $(STUBS_SRC)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build each cross-compiled app
$(APPS): $(CRT0) $(STUBS_OBJ)
	@mkdir -p $(BINDIR)
	$(MAKE) -C $(CURDIR)/$@ CC="$(CC)" LD="$(LD)" STRIP="$(STRIP)" \
		CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" CRT0="$(CRT0)" \
		EXTRA_OBJS="$(EXTRA_OBJS)" LIBS="$(LIBS)" \
		BUILDDIR="$(CURDIR)/$(BUILDDIR)/$@" BINDIR="$(BINDIR)"

# Build initrd builder (host tool — separate from cross-compiled apps)
initrd-builder: $(INITRD_BUILDER)

$(INITRD_BUILDER): $(INITRD_BUILDER_SRC)
	@mkdir -p $(BUILDDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $<

# Verify all binaries are Forest ELF binaries
verify: all
	@echo "=== Verifying binaries ==="
	@fail=0; \
	for bin in $(APP_BINS); do \
		if [ -f "$$bin" ]; then \
			file "$$bin" | grep -q "ELF" && echo "  OK: $$bin" || { echo "  FAIL: $$bin (not ELF)"; fail=1; }; \
		fi; \
	done; \
	if [ $$fail -eq 1 ]; then echo "=== Some binaries failed verification ==="; exit 1; fi; \
	echo "=== All binaries verified ==="

clean:
	rm -rf $(BUILDDIR)
	rm -f $(INITRD_BUILDER)
	for app in $(APPS); do \
		$(MAKE) -C $(CURDIR)/$$app clean 2>/dev/null || true; \
	done

# Install all apps into initrd staging area
install: all
	@mkdir -p $(BUILDDIR)/initrd/bin
	@mkdir -p $(BUILDDIR)/initrd/sbin
	@mkdir -p $(BUILDDIR)/initrd/etc
	@mkdir -p $(BUILDDIR)/initrd/dev
	@mkdir -p $(BUILDDIR)/initrd/proc
	@mkdir -p $(BUILDDIR)/initrd/sys
	@mkdir -p $(BUILDDIR)/initrd/tmp
	@mkdir -p $(BUILDDIR)/initrd/usr/bin
	@mkdir -p $(BUILDDIR)/initrd/usr/sbin
	@cp $(BINDIR)/* $(BUILDDIR)/initrd/bin/ 2>/dev/null || true
	@echo "All apps installed into $(BUILDDIR)/initrd/"

# Build userspace and copy binaries to kernel initrd
initrd: all
	@mkdir -p ../fern/initrd/bin
	@cp $(BINDIR)/* ../fern/initrd/bin/ 2>/dev/null || true
	@echo "Apps installed to fern/initrd/bin/"
