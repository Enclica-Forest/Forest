# Forest OS Shutdown - Makefile
# Usage: make [CC=<compiler>] [LD=<linker>] [STRIP=<strip>] [BUILDDIR=<dir>] [BINDIR=<dir>]

CC      ?= $(error Cross-compiler not found - build via userspace/Makefile)
LD      ?= ld
STRIP   ?= strip

CFLAGS  ?= -m32 -march=i386 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
           -fno-pie -fno-pic -Wall -Wextra -g -O0 -I../include -DUSERSPACE_BUILD \
           -mno-sse -mno-sse2 -mno-mmx
LDFLAGS ?= -m elf_i386 -nostdlib

BUILDDIR ?= build
BINDIR   ?= $(BUILDDIR)/bin

TARGET   = shutdown
SRC      = shutdown.c
OBJ      = $(BUILDDIR)/$(TARGET).o
BIN      = $(BINDIR)/$(TARGET)

.PHONY: all clean install

all: $(BIN)

$(BUILDDIR)/$(TARGET).o: $(SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN): $(BUILDDIR)/$(TARGET).o | $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ $< $(EXTRA_OBJS) $(LIBS)
	$(STRIP) $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

clean:
	rm -rf $(BUILDDIR)
