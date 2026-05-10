PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
PKG_CONFIG ?= pkg-config
CC ?= cc

CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -O2
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags x11 cairo pangocairo x11-xcb xkbcommon-x11 libpipewire-0.3)
PKG_LIBS := $(shell $(PKG_CONFIG) --libs x11 cairo pangocairo x11-xcb xkbcommon-x11 libpipewire-0.3)
SRC := $(wildcard src/*.c)
OBJDIR := build
OBJ := $(SRC:src/%.c=$(OBJDIR)/%.o)
PIPEWIRE_OBJ := $(OBJDIR)/pipewire_volume.o

all: bspbar

bspbar: $(OBJ)
	$(CC) $(OBJ) -o $@ $(PKG_LIBS) -lm -pthread

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: src/%.c config.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -I. -c $< -o $@

$(PIPEWIRE_OBJ): CFLAGS += -Wno-pedantic

clean:
	rm -rf bspbar $(OBJDIR)

install: bspbar
	install -Dm755 bspbar $(DESTDIR)$(BINDIR)/bspbar

.PHONY: all clean install
