# initrd-builder - Forest OS Initial Ramdisk Image Builder
# Host tool for creating initrd images (not cross-compiled)
# Override CC/LD to ensure we build for the host, not the target

HOST_CC := gcc
HOST_CFLAGS := -O2 -Wall -Wextra -pedantic -std=c99
HOST_LDFLAGS :=

CC      := $(HOST_CC)
LD      := $(HOST_CC)
STRIP   := strip
CFLAGS  := $(HOST_CFLAGS)
LDFLAGS := $(HOST_LDFLAGS)

BUILDDIR ?= build
BINDIR   ?= /usr/local/bin

TARGET = initrd-builder

SRCS = initrd_builder.c
OBJS = $(SRCS:.c=.o)

all: $(BUILDDIR)/$(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/$(TARGET): $(OBJS) | $(BUILDDIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	$(STRIP) $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BUILDDIR)/$(TARGET)
	-rmdir $(BUILDDIR) 2>/dev/null || true

install: $(BUILDDIR)/$(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BUILDDIR)/$(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

.PHONY: all clean install uninstall
