#!/usr/bin/env bash
# Mediathek Downloader für Windows bauen: Programm, alle Bibliotheken, GStreamer, ffmpeg und curl
# in einen Ordner, daraus den Installer mediathek-downloader-<Version>-setup.exe.
#
# Läuft in MSYS2 (Umgebung UCRT64), siehe .github/workflows/release.yml für die Pakete:
#   windows/build.sh
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^VERSION *:= *//p' Makefile)
WORK=build/windows
APP=$WORK/app
PREFIX=${MINGW_PREFIX:-/ucrt64}
rm -rf "$WORK"
mkdir -p "$APP/bin"

# --- Symbol (mediathek-downloader.ico ist aus data/mediathek-downloader.svg erzeugt) ---------
cp windows/mediathek-downloader.ico "$WORK/"
printf '1 ICON "mediathek-downloader.ico"\n' > "$WORK/app.rc"
windres -I "$WORK" "$WORK/app.rc" -O coff -o "$WORK/app.res.o"

# --- Programme -----------------------------------------------------------------------------
FLAGS="-O2 -Wall -Wextra -pthread"
# shellcheck disable=SC2046
gcc $FLAGS $(pkg-config --cflags gtk+-3.0 gstreamer-1.0) -mwindows \
    -o "$APP/bin/mediathek-downloader.exe" \
    src/md_gui.c src/md_player.c src/md_stats.c src/md_core.c "$WORK/app.res.o" \
    $(pkg-config --libs gtk+-3.0 gstreamer-1.0)
# shellcheck disable=SC2046
gcc $FLAGS $(pkg-config --cflags glib-2.0) -o "$APP/bin/mediathek-downloader-cli.exe" \
    src/md_cli.c src/md_core.c "$WORK/app.res.o" $(pkg-config --libs glib-2.0)
strip "$APP/bin/"*.exe

# --- Werkzeuge -----------------------------------------------------------------------------
for tool in curl ffmpeg ffprobe gspawn-win64-helper gspawn-win64-helper-console \
            gdk-pixbuf-query-loaders; do
    cp "$PREFIX/bin/$tool.exe" "$APP/bin/"
done

# curl sucht die Zertifikate relativ zu seinem Ordner
install -Dm644 "$PREFIX/etc/ssl/certs/ca-bundle.crt" "$APP/etc/ssl/certs/ca-bundle.crt"

# --- GStreamer: nur, was der Player braucht ------------------------------------------------
gst-inspect-1.0 gtksink > /dev/null      # ohne gtksink kein Bild
mkdir -p "$APP/lib/gstreamer-1.0" "$APP/libexec/gstreamer-1.0"
cp "$PREFIX/libexec/gstreamer-1.0/gst-plugin-scanner.exe" "$APP/libexec/gstreamer-1.0/"
for plugin in coreelements playback typefindfunctions videoconvertscale videoconvert videoscale \
              audioconvert audioresample volume autodetect wasapi wasapi2 directsound gtk libav \
              isomp4 matroska subparse audioparsers videoparsersbad d3d11 app pbtypes rawparse \
              id3demux icydemux overlaycomposition deinterlace; do
    file="$PREFIX/lib/gstreamer-1.0/libgst$plugin.dll"
    if [ -e "$file" ]; then cp "$file" "$APP/lib/gstreamer-1.0/"; fi
done

# --- GTK: Bildlader, Schemas, Symbole, deutsche Texte -------------------------------------
cp -r "$PREFIX/lib/gdk-pixbuf-2.0" "$APP/lib/"
mkdir -p "$APP/share/glib-2.0/schemas" "$APP/share/icons"
cp "$PREFIX/share/glib-2.0/schemas/"*.xml "$APP/share/glib-2.0/schemas/"
glib-compile-schemas "$APP/share/glib-2.0/schemas"
cp -r "$PREFIX/share/icons/Adwaita" "$PREFIX/share/icons/hicolor" "$APP/share/icons/"
install -Dm644 data/mediathek-downloader.svg \
    "$APP/share/icons/hicolor/scalable/apps/mediathek-downloader.svg"
gtk-update-icon-cache -q -f "$APP/share/icons/hicolor" || true
gtk-update-icon-cache -q -f "$APP/share/icons/Adwaita" || true
for domain in gtk30 gtk30-properties glib20 gst-plugins-base-1.0 gst-plugins-good-1.0 \
              gstreamer-1.0; do
    file="$PREFIX/share/locale/de/LC_MESSAGES/$domain.mo"
    if [ -e "$file" ]; then install -Dm644 "$file" "$APP/share/locale/de/LC_MESSAGES/$domain.mo"; fi
done
mkdir -p "$APP/etc/gtk-3.0"
printf '[Settings]\ngtk-font-name=Segoe UI 10\n' > "$APP/etc/gtk-3.0/settings.ini"

# --- DLLs: alles, was Programme und Plugins brauchen, bis nichts mehr dazukommt ----------
while :; do
    added=0
    while IFS= read -r dll; do
        name=$(basename "$dll")
        if [ ! -e "$APP/bin/$name" ]; then
            cp "$dll" "$APP/bin/"
            added=1
        fi
    done < <(find "$APP" -iname '*.exe' -o -iname '*.dll' | while IFS= read -r f; do ldd "$f"; done \
             | grep -io "$PREFIX/bin/[^ ]*\.dll" | sort -u)
    [ "$added" = 1 ] || break
done
# Liste der Bildlader; relative Pfade gelten unter Windows ab dem Programmordner
(cd "$APP" && bin/gdk-pixbuf-query-loaders.exe lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll \
    > lib/gdk-pixbuf-2.0/2.10.0/loaders.cache)
grep -m3 '^"' "$APP/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
cp LICENSE "$APP/LICENSE.txt"

# --- Prüfen und Installer ------------------------------------------------------------------
"$APP/bin/mediathek-downloader-cli.exe" --help > /dev/null
"$APP/bin/curl.exe" --version > /dev/null
"$APP/bin/ffmpeg.exe" -hide_banner -version > /dev/null
# Intro-Erkennung braucht Chromaprint in ffmpeg; fehlt es, gibt es keinen Knopf (Warnung im Log)
# (erst ganz lesen: grep -q bricht die Pipe sonst ab, und pipefail meldet das als Fehler)
muxers=$("$APP/bin/ffmpeg.exe" -hide_banner -muxers 2> /dev/null)
grep -q chromaprint <<< "$muxers" ||
    echo "::warning::ffmpeg ohne Chromaprint – Intros werden unter Windows nicht erkannt"
makensis -V2 -DVERSION="$VERSION" -DSRC="$(cygpath -aw "$APP")" \
    -DICON="$(cygpath -aw "$WORK/mediathek-downloader.ico")" \
    -DLICENSE="$(cygpath -aw LICENSE)" \
    -DOUTFILE="$(cygpath -aw "$PWD/mediathek-downloader-$VERSION-setup.exe")" \
    windows/installer.nsi
echo "fertig: mediathek-downloader-$VERSION-setup.exe ($(du -sh "$APP" | cut -f1) entpackt)"
