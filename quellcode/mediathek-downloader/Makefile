# Mediathek Downloader: grafische Oberfläche (mediathek-downloader) und Terminal-Version
# (mediathek-downloader-cli)
#
#   make            baut beide Programme
#   make deb        baut das Debian-Paket mediathek-downloader_<Version>_<Architektur>.deb
#   make install    installiert nach $(PREFIX) (Standard: /usr/local, braucht sudo)

VERSION  := 1.6.1
DEBREV   := 1
ARCH     := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
PREFIX   ?= /usr/local
DESTDIR  ?=

CC       ?= gcc
CFLAGS   ?= -O2
CFLAGS   += -Wall -Wextra -pthread
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0)
GLIB_LIBS   := $(shell pkg-config --libs glib-2.0)

CORE := src/md_core.c src/md_core.h

all: mediathek-downloader mediathek-downloader-cli

mediathek-downloader: src/md_gui.c $(CORE)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ src/md_gui.c src/md_core.c $(GTK_LIBS)

mediathek-downloader-cli: src/md_cli.c $(CORE)
	$(CC) $(CFLAGS) $(GLIB_CFLAGS) -o $@ src/md_cli.c src/md_core.c $(GLIB_LIBS)

install: all
	install -Dm755 mediathek-downloader "$(DESTDIR)$(PREFIX)/bin/mediathek-downloader"
	install -Dm755 mediathek-downloader-cli "$(DESTDIR)$(PREFIX)/bin/mediathek-downloader-cli"
	install -Dm644 data/mediathek-downloader.desktop \
		"$(DESTDIR)$(PREFIX)/share/applications/mediathek-downloader.desktop"
	install -Dm644 data/mediathek-downloader.svg \
		"$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/mediathek-downloader.svg"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/mediathek-downloader" "$(DESTDIR)$(PREFIX)/bin/mediathek-downloader-cli"
	rm -f "$(DESTDIR)$(PREFIX)/share/applications/mediathek-downloader.desktop"
	rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/mediathek-downloader.svg"

deb: all
	rm -rf build/deb
	$(MAKE) install DESTDIR="$(CURDIR)/build/deb" PREFIX=/usr
	strip build/deb/usr/bin/mediathek-downloader build/deb/usr/bin/mediathek-downloader-cli
	install -d build/deb/DEBIAN
	sed -e 's/@VERSION@/$(VERSION)-$(DEBREV)/' -e 's/@ARCH@/$(ARCH)/' \
		-e "s/@SIZE@/$$(du -sk --apparent-size build/deb/usr | cut -f1)/" \
		data/control.in > build/deb/DEBIAN/control
	dpkg-deb --root-owner-group --build build/deb mediathek-downloader_$(VERSION)-$(DEBREV)_$(ARCH).deb

clean:
	rm -rf build mediathek-downloader mediathek-downloader-cli

distclean: clean
	rm -f *.deb

.PHONY: all install uninstall deb clean distclean
