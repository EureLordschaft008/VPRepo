# AV1-Converter – grafische Oberfläche (GTK 3) und Terminal-Version
#
#   make                 baut av1-converter (GUI) und av1-converter-cli
#   make deb             baut das Paket av1-converter_<Version>_<Arch>.deb
#   sudo make install    installiert nach /usr/local (ohne Paketverwaltung)
#   sudo make uninstall  entfernt diese Installation wieder

VERSION = 1.4.0
DEBREV  = 2
PREFIX ?= /usr/local
CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
override CFLAGS += -DAC_VERSION='"$(VERSION)"'
ARCH   := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
DEB     = av1-converter_$(VERSION)-$(DEBREV)_$(ARCH).deb

GIO_CFLAGS := $(shell pkg-config --cflags gio-unix-2.0)
GIO_LIBS   := $(shell pkg-config --libs gio-unix-2.0)
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 gio-unix-2.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0 gio-unix-2.0)
CORE        = src/ac_core.c src/ac_core.h Makefile

all: av1-converter av1-converter-cli

av1-converter: src/ac_gui.c $(CORE)
	$(CC) $(CFLAGS) -pthread $(GTK_CFLAGS) -o $@ src/ac_gui.c src/ac_core.c $(GTK_LIBS) -lm

av1-converter-cli: src/ac_cli.c $(CORE)
	$(CC) $(CFLAGS) -pthread $(GIO_CFLAGS) -o $@ src/ac_cli.c src/ac_core.c $(GIO_LIBS) -lm

install: all
	install -Dm755 av1-converter $(DESTDIR)$(PREFIX)/bin/av1-converter
	install -Dm755 av1-converter-cli $(DESTDIR)$(PREFIX)/bin/av1-converter-cli
	install -Dm644 data/av1-converter.desktop \
		$(DESTDIR)$(PREFIX)/share/applications/av1-converter.desktop
	install -Dm644 data/av1-converter.svg \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/av1-converter.svg

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/av1-converter $(DESTDIR)$(PREFIX)/bin/av1-converter-cli \
		$(DESTDIR)$(PREFIX)/share/applications/av1-converter.desktop \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/av1-converter.svg

deb: $(DEB)

$(DEB): all LICENSE data/control.in data/av1-converter.desktop data/av1-converter.svg
	rm -rf build/deb
	$(MAKE) --no-print-directory install DESTDIR=build/deb PREFIX=/usr
	strip --strip-unneeded build/deb/usr/bin/av1-converter build/deb/usr/bin/av1-converter-cli
	install -Dm644 LICENSE build/deb/usr/share/doc/av1-converter/copyright
	mkdir -p build/deb/DEBIAN
	sed -e 's/@VERSION@/$(VERSION)-$(DEBREV)/' -e 's/@ARCH@/$(ARCH)/' \
		-e "s/@SIZE@/$$(du -sk --apparent-size build/deb/usr | cut -f1)/" data/control.in \
		> build/deb/DEBIAN/control
	dpkg-deb --root-owner-group --build build/deb $@

clean:
	rm -rf av1-converter av1-converter-cli build *.deb

.PHONY: all install uninstall deb clean
