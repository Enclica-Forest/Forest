# Forest OS - mkdir
# Creates directories

# Toolchain (inherit from parent or use defaults)
CC      ?= $(error Cross-compiler not found - build via userspace/Makefile)
LD      ?= i686-forestos-ld
STRIP   ?= i686-forestos-strip
CFLAGS  ?= -m32 -march=i386 -ffreestanding -nostdlib -fno-builtin \
           -fno-stack-protector -fno-pie -fno-pic -Wall -Wextra -g -O0 \
           -I$(CURDIR)/../include -I$(CURDIR)/../../libs/libc/include/libc \
           -I$(CURDIR)/../../libs/forestcore/include -DUSERSPACE_BUILD
LDFLAGS ?= -m elf_i386 -nostdlib

# Build paths
BINDIR  ?= build

# Objects
SRC     = mkdir.c
OBJ     = $(SRC:.c=.o)

.PHONY: all clean

all: $(BINDIR)/mkdir

$(OBJ): $(SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BINDIR)/mkdir: $(OBJ)
	@mkdir -p $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ $^ $(EXTRA_OBJS) $(LIBS)
	$(STRIP) $@

clean:
	rm -f $(OBJ) $(BINDIR)/mkdir
