# Forest OS Userspace App: chmod
# Full POSIX-compatible chmod utility

# Toolchain (inherited from master Makefile)
CC      ?= $(error Cross-compiler not found - build via userspace/Makefile)
LD      ?= ld
STRIP   ?= strip

# Compiler and linker flags
CFLAGS  ?= -m32 -march=i386 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
           -fno-pie -fno-pic -Wall -Wextra -g -O0 \
           -I$(CURDIR)/../include \
           -I$(CURDIR)/../libs/libc/include/libc \
           -I$(CURDIR)/../libs/forestcore/include \
           -DUSERSPACE_BUILD \
           -mno-sse -mno-sse2 -mno-mmx
LDFLAGS ?= -m elf_i386 -nostdlib

# Build output
BUILDDIR ?= build
BINDIR   ?= $(BUILDDIR)/bin

# Source and target
TARGET   = chmod
SRCS     = chmod.c
OBJS     = $(SRCS:.c=.o)

.PHONY: all clean

all: $(BINDIR)/$(TARGET)

$(BINDIR)/$(TARGET): $(OBJS) | $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(EXTRA_OBJS) $(LIBS)
	$(STRIP) $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BINDIR):
	mkdir -p $(BINDIR)

clean:
	rm -f $(OBJS) $(BINDIR)/$(TARGET)
