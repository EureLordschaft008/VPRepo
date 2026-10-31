# Duplikat-Finder – grafische Oberfläche (GTK 3), Terminal-Version und Hilfsprozess
#
#   make                 baut duplikat-finder (GUI), duplikat-finder-cli und den Hilfsprozess
#   make deb             baut das Paket duplikat-finder_<Version>_<Arch>.deb
#   make test            Regressionstests der Link-Aktionen und des Suchlaufs
#   make clean           entfernt Programme und Build-Ordner (Pakete bleiben)
#   make distclean       entfernt zusätzlich die gebauten .deb-Pakete
#   sudo make install    installiert nach /usr/local (ohne Paketverwaltung)
#   sudo make uninstall  entfernt diese Installation wieder
#
# Zum Bauen nötig: sudo apt install build-essential libgtk-3-dev libsqlite3-dev \
#     libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
#     libchromaprint-dev

VERSION = 1.0.4
DEBREV  = 1
PREFIX ?= /usr/local
CC     ?= gcc
CFLAGS ?= -O2 -march=x86-64-v2 -Wall -Wextra
override CFLAGS += -DDF_VERSION='"$(VERSION)"'
ARCH   := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
DEB     = duplikat-finder_$(VERSION)-$(DEBREV)_$(ARCH).deb

GTK_CFLAGS  := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS    := $(shell pkg-config --libs gtk+-3.0)
CORE_CFLAGS := $(shell pkg-config --cflags glib-2.0 gio-2.0 gio-unix-2.0 sqlite3)
CORE_LIBS   := $(shell pkg-config --libs glib-2.0 gio-2.0 gio-unix-2.0 sqlite3)
AV_CFLAGS   := $(shell pkg-config --cflags libavformat libavcodec libavutil libswscale libswresample libchromaprint)
AV_LIBS     := $(shell pkg-config --libs libavformat libavcodec libavutil libswscale libswresample libchromaprint)

CORE_SRC = src/df_util.c src/df_proc.c src/df_db.c src/df_scan.c src/df_result.c
CORE_HDR = src/df_core.h src/df_internal.h src/df_proto.h Makefile

all: duplikat-finder duplikat-finder-cli duplikat-finder-worker

duplikat-finder: src/df_gui.c src/df_gui_results.c src/df_gui.h $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -pthread $(GTK_CFLAGS) $(CORE_CFLAGS) -o $@ src/df_gui.c src/df_gui_results.c $(CORE_SRC) \
		$(GTK_LIBS) $(CORE_LIBS) -lm

duplikat-finder-cli: src/df_cli.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -pthread $(CORE_CFLAGS) -o $@ src/df_cli.c $(CORE_SRC) $(CORE_LIBS) -lm

duplikat-finder-worker: src/df_worker.c src/df_proto.h
	$(CC) $(CFLAGS) $(AV_CFLAGS) -o $@ src/df_worker.c $(AV_LIBS) -lm

install: all
	install -Dm755 duplikat-finder $(DESTDIR)$(PREFIX)/bin/duplikat-finder
	install -Dm755 duplikat-finder-cli $(DESTDIR)$(PREFIX)/bin/duplikat-finder-cli
	install -Dm755 duplikat-finder-worker \
		$(DESTDIR)$(PREFIX)/lib/duplikat-finder/duplikat-finder-worker
	install -Dm644 data/duplikat-finder.desktop \
		$(DESTDIR)$(PREFIX)/share/applications/duplikat-finder.desktop
	install -Dm644 data/duplikat-finder.svg \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/duplikat-finder.svg

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/duplikat-finder $(DESTDIR)$(PREFIX)/bin/duplikat-finder-cli \
		$(DESTDIR)$(PREFIX)/lib/duplikat-finder/duplikat-finder-worker \
		$(DESTDIR)$(PREFIX)/share/applications/duplikat-finder.desktop \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/duplikat-finder.svg
	-rmdir $(DESTDIR)$(PREFIX)/lib/duplikat-finder

deb: $(DEB)

$(DEB): all LICENSE data/control.in data/duplikat-finder.desktop data/duplikat-finder.svg
	rm -rf build/deb
	$(MAKE) --no-print-directory install DESTDIR=build/deb PREFIX=/usr
	strip --strip-unneeded build/deb/usr/bin/duplikat-finder build/deb/usr/bin/duplikat-finder-cli \
		build/deb/usr/lib/duplikat-finder/duplikat-finder-worker
	install -Dm644 LICENSE build/deb/usr/share/doc/duplikat-finder/copyright
	mkdir -p build/deb/DEBIAN
	sed -e 's/@VERSION@/$(VERSION)-$(DEBREV)/' -e 's/@ARCH@/$(ARCH)/' \
		-e "s/@SIZE@/$$(du -sk --apparent-size build/deb/usr | cut -f1)/" data/control.in \
		> build/deb/DEBIAN/control
	dpkg-deb --root-owner-group --build build/deb $@

build/test/test_actions: tests/test_actions.c $(CORE_SRC) $(CORE_HDR)
	mkdir -p build/test
	$(CC) $(CFLAGS) -pthread $(CORE_CFLAGS) -o $@ tests/test_actions.c $(CORE_SRC) $(CORE_LIBS) -lm

test: build/test/test_actions duplikat-finder-cli duplikat-finder-worker
	DF_TEST_CLI="$(CURDIR)/duplikat-finder-cli" build/test/test_actions

clean:
	rm -rf duplikat-finder duplikat-finder-cli duplikat-finder-worker build

distclean: clean
	rm -f *.deb

.PHONY: all install uninstall deb test clean distclean
