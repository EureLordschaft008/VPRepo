# Intro-Cutter – grafische Oberfläche (GTK 3) und Terminal-Version
#
#   make                 baut intro-cutter (GUI) und intro-cutter-cli
#   make deb             baut das Paket intro-cutter_<Version>_<Arch>.deb
#   make clean           entfernt Programme und Build-Ordner (Pakete bleiben)
#   make distclean       entfernt zusätzlich die gebauten .deb-Pakete
#   sudo make install    installiert nach /usr/local (ohne Paketverwaltung)
#   sudo make uninstall  entfernt diese Installation wieder

VERSION = 5.6.0
DEBREV  = 3
PREFIX ?= /usr/local
CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
override CFLAGS += -DIC_VERSION='"$(VERSION)"'
ARCH   := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
DEB     = intro-cutter_$(VERSION)-$(DEBREV)_$(ARCH).deb

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)
CORE        = src/ic_core.c src/ic_core.h Makefile

all: intro-cutter intro-cutter-cli

intro-cutter: src/ic_gui.c $(CORE)
	$(CC) $(CFLAGS) -pthread $(GTK_CFLAGS) -o $@ src/ic_gui.c src/ic_core.c $(GTK_LIBS) -lm

intro-cutter-cli: src/ic_cli.c $(CORE)
	$(CC) $(CFLAGS) -pthread -o $@ src/ic_cli.c src/ic_core.c -lm

install: all
	install -Dm755 intro-cutter $(DESTDIR)$(PREFIX)/bin/intro-cutter
	install -Dm755 intro-cutter-cli $(DESTDIR)$(PREFIX)/bin/intro-cutter-cli
	install -Dm644 data/intro-cutter.desktop \
		$(DESTDIR)$(PREFIX)/share/applications/intro-cutter.desktop
	install -Dm644 data/intro-cutter.svg \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/intro-cutter.svg

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/intro-cutter $(DESTDIR)$(PREFIX)/bin/intro-cutter-cli \
		$(DESTDIR)$(PREFIX)/share/applications/intro-cutter.desktop \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/intro-cutter.svg

deb: $(DEB)

$(DEB): all LICENSE data/control.in data/intro-cutter.desktop data/intro-cutter.svg
	rm -rf build/deb
	$(MAKE) --no-print-directory install DESTDIR=build/deb PREFIX=/usr
	strip --strip-unneeded build/deb/usr/bin/intro-cutter build/deb/usr/bin/intro-cutter-cli
	install -Dm644 LICENSE build/deb/usr/share/doc/intro-cutter/copyright
	mkdir -p build/deb/DEBIAN
	sed -e 's/@VERSION@/$(VERSION)-$(DEBREV)/' -e 's/@ARCH@/$(ARCH)/' \
		-e "s/@SIZE@/$$(du -sk --apparent-size build/deb/usr | cut -f1)/" data/control.in \
		> build/deb/DEBIAN/control
	dpkg-deb --root-owner-group --build build/deb $@

clean:
	rm -rf intro-cutter intro-cutter-cli build

distclean: clean
	rm -f *.deb

.PHONY: all install uninstall deb clean distclean
