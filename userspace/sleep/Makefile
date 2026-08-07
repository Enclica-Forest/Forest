# Forest OS - sleep
# Delay for a specified time

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
BUILDDIR?= build
BINDIR  ?= /usr/local/bin

# Objects
SRC     = sleep.c
OBJ     = $(SRC:.c=.o)

.PHONY: all clean install

all: $(BUILDDIR)/sleep

$(OBJ): $(SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/sleep: $(OBJ)
	@mkdir -p $(BUILDDIR)
	$(LD) $(LDFLAGS) -o $@ $^ $(EXTRA_OBJS) $(LIBS)
	$(STRIP) $@

install: $(BUILDDIR)/sleep
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BUILDDIR)/sleep $(DESTDIR)$(BINDIR)/sleep

clean:
	rm -f $(OBJ) $(BUILDDIR)/sleep
