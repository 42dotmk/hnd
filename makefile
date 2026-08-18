.POSIX:

CC      = cc
CFLAGS  = -std=c99 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L \
          -isystem vendor
LDLIBS  = -lX11 -lXrandr -ldbus-1 `pkg-config --libs xft`
BINDIR  = $(HOME)/.local/bin

all: hnd

hnd: hnd.c vendor/stb_ds.h
	$(CC) $(CFLAGS) `pkg-config --cflags dbus-1 xft` -o $@ hnd.c $(LDLIBS)

install: hnd
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/hnd" $(BINDIR)/hnd

uninstall:
	rm -f $(BINDIR)/hnd

clean:
	rm -f hnd

.PHONY: all install uninstall clean
