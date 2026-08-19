.POSIX:

CC      = cc

# Version derived from `git describe` at build time so the binary reports
# the exact tag/commit it was built from; "dev" without git metadata.
VERSION != git describe --tags --always --dirty 2>/dev/null || echo dev

CFLAGS  = -std=c11 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L \
          -DHND_VERSION='"$(VERSION)"' -isystem vendor
LDLIBS  = -lX11 -lXrandr -ldbus-1 `pkg-config --libs xft`
BINDIR  = $(HOME)/.local/bin

all: hnd

hnd: hnd.c config.h vendor/stb_ds.h
	$(CC) $(CFLAGS) `pkg-config --cflags dbus-1 xft` -o $@ hnd.c $(LDLIBS)

install: hnd
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/hnd" $(BINDIR)/hnd

uninstall:
	rm -f $(BINDIR)/hnd

clean:
	rm -f hnd

.PHONY: all install uninstall clean
