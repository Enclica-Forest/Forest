FORESTOS_TOOLCHAIN_DIR ?= $(abspath $(CURDIR)/../../forestos-toolchain)
FORESTOS_TOOLCHAIN_PREFIX ?= i686-forestos

ifeq ($(wildcard $(FORESTOS_TOOLCHAIN_DIR)/install/bin/$(FORESTOS_TOOLCHAIN_PREFIX)-gcc),)
    CC := gcc
    LD := ld
    STRIP := strip
    ARCH_FLAGS := -m32 -march=i386
    ARCH_LDFLAGS := -m elf_i386
else
    CC := $(FORESTOS_TOOLCHAIN_DIR)/install/bin/$(FORESTOS_TOOLCHAIN_PREFIX)-gcc
    LD := $(FORESTOS_TOOLCHAIN_DIR)/install/bin/$(FORESTOS_TOOLCHAIN_PREFIX)-ld
    STRIP := $(FORESTOS_TOOLCHAIN_DIR)/install/bin/$(FORESTOS_TOOLCHAIN_PREFIX)-strip
    ARCH_FLAGS := -m32 -march=i386
    ARCH_LDFLAGS := -m elf_i386
endif

CFLAGS ?= $(ARCH_FLAGS) \
    -O2 -Wall -Wextra -std=c11 -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pie -fno-pic \
    -I$(CURDIR)/../../libs/libc/include/libc \
    -I$(CURDIR)/../../forestos-toolchain/sysroot-skeleton/usr/include \
    -DUSERSPACE_BUILD -mno-sse -mno-sse2 -mno-mmx
LDFLAGS ?= $(ARCH_LDFLAGS) -nostdlib
BUILDDIR?= build

LIBC_SRC = libc_wrappers.c
LIBC_OBJ = libc_wrappers.o

SRCS = main.c sys.c x11_atoms.c x11_protocol.c x11_socket.c \
       x11_window.c x11_gc.c x11_pixmap.c x11_events.c x11_draw.c \
       x11_property.c x11_font.c x11_color.c mem.c
OBJS = $(SRCS:.c=.o) $(LIBC_OBJ)
TARGET = forest-x11

all: $(BUILDDIR)/$(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

LIBS = $(FORESTOS_TOOLCHAIN_DIR)/sysroot/lib/libc.a

$(BUILDDIR)/$(TARGET): $(OBJS) | $(BUILDDIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(EXTRA_OBJS) $(LIBS)
	$(STRIP) $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR) $(OBJS)

.PHONY: all clean
