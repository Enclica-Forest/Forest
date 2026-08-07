# Forest OS Userspace - touch Makefile
# Accepts: CC, LD, STRIP, CFLAGS, LDFLAGS, BUILDDIR, BINDIR

CC      ?= $(error Cross-compiler not found - build via userspace/Makefile)
LD ?= ld
STRIP ?= strip

CFLAGS ?= -m32 -march=i386 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
    -fno-pie -fno-pic -Wall -Wextra -g -O0 \
    -I$(CURDIR)/../include \
    -I$(CURDIR)/../libs/libc/include/libc \
    -I$(CURDIR)/../libs/forestcore/include \
    -DUSERSPACE_BUILD \
    -mno-sse -mno-sse2 -mno-mmx

LDFLAGS ?= -m elf_i386 -nostdlib

BUILDDIR ?= build
BINDIR ?= $(BUILDDIR)/bin

TARGET := touch
SRC := touch.c
OBJ := $(BUILDDIR)/$(TARGET).o
BIN := $(BINDIR)/$(TARGET)

.PHONY: all clean

all: $(BIN)

$(BUILDDIR):
	mkdir -p $(BUILDDIR) $(BINDIR)

$(OBJ): $(SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $< $(EXTRA_OBJS) $(LIBS)
	$(STRIP) $@

clean:
	rm -rf $(BUILDDIR)
