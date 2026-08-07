# Forest OS Userspace - cp
# POSIX file copy utility

# Toolchain (inherit from parent Makefile or use defaults)
CC      ?= $(error Cross-compiler not found - build via userspace/Makefile)
LD      ?= ld
STRIP   ?= strip
CFLAGS  ?= -m32 -march=i386 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
           -fno-pie -fno-pic -Wall -Wextra -g -O0 \
           -I../include -DUSERSPACE_BUILD -mno-sse -mno-sse2 -mno-mmx
LDFLAGS ?= -m elf_i386 -nostdlib

# Output
BUILDDIR ?= build
BINDIR   ?= $(BUILDDIR)/bin
TARGET   := $(BINDIR)/cp

.PHONY: all clean

all: $(TARGET)

$(TARGET): cp.c
	@mkdir -p $(BUILDDIR)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -c -o $(BUILDDIR)/cp.o $<
	$(LD) $(LDFLAGS) -o $@ $(BUILDDIR)/cp.o $(EXTRA_OBJS) $(LIBS)
	$(STRIP) $@

clean:
	rm -rf $(BUILDDIR)
