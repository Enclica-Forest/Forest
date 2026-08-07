# Forest OS Userspace - cat
# Builds the cat utility

CC      ?= $(error Cross-compiler not found - build via userspace/Makefile)
LD      ?= $(CC)
STRIP   ?= strip
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
BUILDDIR?= build
BINDIR  ?= /usr/local/bin

TARGET = cat
SRC    = cat.c
OBJ    = $(BUILDDIR)/$(TARGET).o
BIN    = $(BUILDDIR)/$(TARGET)

.PHONY: all clean install

all: $(BIN)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(OBJ): $(SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^ $(EXTRA_OBJS) $(LIBS)
	$(STRIP) $@
	mkdir -p $(BINDIR)
	cp $@ $(BINDIR)/$(TARGET)

clean:
	rm -rf $(BUILDDIR)
