# Forest OS - dd
# Built via userspace/Makefile

CC      ?= $(error Cross-compiler not found - build via userspace/Makefile)
LD      ?= ld
STRIP   ?= strip
CFLAGS  ?= -m32 -march=i386 -ffreestanding -nostdlib -fno-builtin \
           -fno-stack-protector -fno-pie -fno-pic -Wall -Wextra -g -O0 \
           -mno-sse -mno-sse2 -mno-mmx
LDFLAGS ?= -m elf_i386 -nostdlib

BUILDDIR ?= build
BINDIR   ?= $(BUILDDIR)/bin

TARGET = dd
SRC    = dd.c
OBJ    = $(BUILDDIR)/$(TARGET).o
BIN    = $(BUILDDIR)/$(TARGET)

.PHONY: all clean

all: $(BIN)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(OBJ): $(SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^ $(EXTRA_OBJS) $(LIBS)
	$(STRIP) $@

clean:
	rm -rf $(BUILDDIR)
