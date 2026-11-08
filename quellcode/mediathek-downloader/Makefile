# Mediathek Downloader: grafische Oberfläche (mediathek-downloader) und Terminal-Version
# (mediathek-downloader-cli)
#
#   make            baut beide Programme
#   make deb        baut das Debian-Paket mediathek-downloader_<Version>_<Architektur>.deb
#   make install    installiert nach $(PREFIX) (Standard: /usr/local, braucht sudo)
#   make test       Regressionstests und kurzer Fuzz-Lauf (zusätzlich ASAN=1 für AddressSanitizer)

VERSION  := 2.4.0
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
GST_CFLAGS  ?= $(shell pkg-config --cflags gstreamer-1.0)
GST_LIBS    ?= $(shell pkg-config --libs gstreamer-1.0)

CORE := src/md_core.c src/md_core.h

all: mediathek-downloader mediathek-downloader-cli

mediathek-downloader: src/md_gui.c src/md_player.c src/md_player.h src/md_stats.c src/md_stats.h \
		$(CORE)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(GST_CFLAGS) -o $@ src/md_gui.c src/md_player.c src/md_stats.c \
		src/md_core.c \
		$(GTK_LIBS) $(GST_LIBS)

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
	install -Dm644 LICENSE build/deb/usr/share/doc/mediathek-downloader/copyright
	install -d build/deb/DEBIAN
	sed -e 's/@VERSION@/$(VERSION)-$(DEBREV)/' -e 's/@ARCH@/$(ARCH)/' \
		-e "s/@SIZE@/$$(du -sk --apparent-size build/deb/usr | cut -f1)/" \
		data/control.in > build/deb/DEBIAN/control
	dpkg-deb --root-owner-group --build build/deb mediathek-downloader_$(VERSION)-$(DEBREV)_$(ARCH).deb

# ASan läuft nicht, wenn /etc/ld.so.preload eine andere Bibliothek vorlädt. MALLOC_CHECK_ wirkt
# seit glibc 2.34 nur zusammen mit libc_malloc_debug und verträgt sich nicht mit ASan.
ifdef ASAN
TEST_DIR  := build/test-asan
SANITIZE  := -fsanitize=address,undefined
FUZZ_ENV  :=
else
TEST_DIR  := build/test
SANITIZE  := -fsanitize=undefined
FUZZ_ENV  := MALLOC_CHECK_=3 LD_PRELOAD=libc_malloc_debug.so.0
endif
FUZZ_SECONDS ?= 10
TEST_CFLAGS  := -O1 -g -Wall -Wextra -pthread -fno-omit-frame-pointer \
	$(SANITIZE) -fno-sanitize-recover=undefined $(GLIB_CFLAGS)

$(TEST_DIR)/%: tests/%.c tests/test_util.h $(CORE)
	mkdir -p $(TEST_DIR)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(GLIB_LIBS)

test: $(TEST_DIR)/test_core $(TEST_DIR)/fuzz_core
	$(TEST_DIR)/test_core
	$(FUZZ_ENV) FUZZ_SECONDS=$(FUZZ_SECONDS) $(TEST_DIR)/fuzz_core

clean:
	rm -rf build mediathek-downloader mediathek-downloader-cli

distclean: clean
	rm -f *.deb

.PHONY: all install uninstall deb test clean distclean
