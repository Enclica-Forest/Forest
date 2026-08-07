# Forest OS Init - Makefile
# System initialization process (PID 1)

CC      ?= $(error Cross-compiler not found - build via userspace/Makefile)
LD ?= gcc
STRIP ?= strip

CFLAGS ?= -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs
LDFLAGS ?= -static -nostartfiles -nodefaultlibs

BUILDDIR ?= build
BINDIR ?= /sbin

SRC = init.c
OBJ = $(BUILDDIR)/init.o
BIN = $(BUILDDIR)/init

.PHONY: all clean install uninstall

all: $(BIN)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(OBJ): $(SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $< $(EXTRA_OBJS) $(LIBS)
	$(STRIP) $@

clean:
	rm -rf $(BUILDDIR)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/init

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/init