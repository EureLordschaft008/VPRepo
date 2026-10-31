# Duplikat-Finder 1.0.3

Findet doppelte Videos – und auf Wunsch Bilder – anhand ihres **Inhalts**: auch wenn sie
anders kodiert (H.264 ↔ HEVC ↔ VP9 ↔ AV1), verkleinert, beschnitten, gespiegelt oder mit
schwarzen Balken versehen sind. Gekürzte Fassungen (Teilclips) findet er über einen
Ton-Fingerabdruck. Grafische Oberfläche (`duplikat-finder`) im Stil von Intro-Cutter und
AV1-Converter, dazu eine Terminal-Version (`duplikat-finder-cli`).

Neubau des „Video Duplicate Finder“ (VDF, C#/Avalonia) mit allen seinen Funktionen – aber
ohne dessen Probleme:

| Video Duplicate Finder | Duplikat-Finder |
|---|---|
| dekodiert im eigenen Prozess (natives ffmpeg) – eine defekte Datei oder ein hängender Treiber reißt das ganze Programm mit | dekodiert in **eigenen Hilfsprozessen** mit Zeit- und Speichergrenze; stürzt einer ab, wird er neu gestartet und nur diese Datei als „nicht lesbar“ gemerkt |
| eine einzige große Datenbank, die am Ende komplett gespeichert wird | **SQLite** (WAL), ~1 KB je Datei (100 000 Videos ≈ 100 MB), wird laufend in kleinen Paketen geschrieben – ein Abbruch verliert nichts |
| CPU *oder* GPU | **CPU und GPU gleichzeitig** aus einer Warteschlange; was die GPU nicht kann, übernimmt die CPU |
| bremst den PC aus | niedrige Priorität, und CPU-Prozesse pausieren automatisch, wenn andere Programme die CPU brauchen oder der Speicher knapp wird |
| Vergleich langsam bei vielen Dateien | 100 000 Videos = 5 Milliarden Paare in wenigen Sekunden (alle Kerne, 64-Bit-pHash) |

## Installation

Einmalig die Paketquelle VPRepo einbinden und den Duplikat-Finder installieren:

```bash
sudo install -d -m 755 /etc/apt/keyrings
curl -fsSL https://eurelordschaft008.github.io/VPRepo/vprepo.gpg | sudo tee /etc/apt/keyrings/vprepo.gpg >/dev/null
echo 'deb [signed-by=/etc/apt/keyrings/vprepo.gpg] https://eurelordschaft008.github.io/VPRepo stable main' | sudo tee /etc/apt/sources.list.d/vprepo.list
sudo apt update
sudo apt install duplikat-finder
```

Neue Versionen erscheinen danach automatisch in der Aktualisierungsverwaltung von Linux Mint.

Danach steht „Duplikat-Finder“ im Anwendungsmenü. Abhängigkeiten (GTK 3, SQLite, die
ffmpeg-Bibliotheken von Ubuntu 24.04, Chromaprint) holt `apt` automatisch.

Selbst bauen: `make` bzw. `make deb` (benötigt `build-essential libgtk-3-dev libsqlite3-dev
libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
libchromaprint-dev`).

## Neue Version veröffentlichen

`VERSION` (bei reinen Paketänderungen `DEBREV`) im `Makefile` erhöhen, committen und einen
passenden Tag pushen, z. B. `v1.0.4` bzw. `v1.0.4-2`. GitHub Actions baut dann das Paket,
legt es als Release an und stellt es über VPRepo bereit. Eine Versionsnummer lässt sich nur
einmal veröffentlichen.

## Bedienung

1. **Ordner** hinzufügen (Knopf oder per Drag & Drop aus dem Dateimanager, auch mehrere) und
   bei Bedarf Ordner zum **Überspringen** angeben.
2. **Duplikate suchen.** Die Fortschrittsseite zeigt Tempo (GPU und CPU getrennt), Restzeit,
   CPU-Last, die gerade gelesenen Dateien und wie viele CPU-Prozesse wegen anderer Programme
   pausieren. Pausieren und Abbrechen sind jederzeit möglich – Gelesenes bleibt gemerkt.
3. **Trefferliste:** Gruppen ähnlicher Dateien mit Vorschaubildern; je Datei Laufzeit,
   Auflösung, Bildrate, Bitrate, Codecs, Ton, Größe und Datum – die besten Werte der Gruppe
   sind grün. Doppelklick spielt ab, Rechtsklick (oder ⋮) öffnet das Menü der Datei.

### Trefferliste

- **Auswählen:** niedrigere Qualität markieren (beste behalten – nach der einstellbaren
  Reihenfolge der Merkmale Laufzeit, Auflösung, Bildrate, Bitrate, Ton-Bitrate, Größe, Datum,
  Pfadlänge), ältere bzw. neuere markieren, 100 % gleiche markieren (auch: nur bei gleicher
  Größe), Teilclips markieren, Pfad enthält / enthält nicht …, nach Ähnlichkeit (von–bis),
  umkehren, nichts. Regeln wirken nur auf die gerade angezeigten (gefilterten) Gruppen und
  lassen in jeder Gruppe mindestens eine Datei unmarkiert.
- **Markierte:** in den Papierkorb (Entf), endgültig löschen (Umschalt+Entf), verschieben,
  kopieren, durch **Hardlinks** oder **symbolische Links** auf die beste Datei ersetzen, aus
  der Liste entfernen (auch dauerhaft ignorieren), beim nächsten Suchlauf neu lesen, Pfade
  kopieren. Sind in einer Gruppe *alle* Dateien markiert, warnt das Programm vorher.
- **Je Gruppe:** „Beste behalten“, „Vergleichen“ (alle Vergleichsbilder der Dateien
  nebeneinander, mit Übereinstimmung je Bild, Bildgröße per Regler) und „Kein Duplikat“ (das
  Paar wird nie wieder gemeldet).
- **Je Datei:** öffnen, im Ordner zeigen, umbenennen, Pfad kopieren, „diese behalten“.
- **Filter:** Suche in Pfaden (Strg+F), nur Videos/Bilder/Teilclips/gespiegelte/100 % gleiche/
  unter 100 %/mit markierten; **Sortierung** nach Ähnlichkeit, Platzersparnis, Anzahl,
  Laufzeit, Pfad.
- **Exportieren:** Ergebnis speichern (JSON, lässt sich wieder öffnen), Tabelle (CSV für
  LibreOffice/Excel), Webseite (HTML).
- Markierungen und entfernte Einträge werden laufend gespeichert – nach einem Neustart öffnet
  „Letztes Ergebnis öffnen“ genau diesen Stand.

### Menü

Ergebnis öffnen, **Zwei Dateien vergleichen** (Diagnose: Abstand Bild für Bild, ob es mit den
aktuellen Einstellungen ein Duplikat wäre), **Geplanter Suchlauf** (täglich zur gewählten
Uhrzeit per systemd-Benutzer-Timer, auch ohne geöffnetes Fenster, mit Benachrichtigung),
**Datenbank** (bearbeiten/suchen, aufräumen, nicht lesbare erneut versuchen, Pfade ändern nach
umbenanntem Laufwerk, exportieren/importieren, leeren), ignorierte Dateien,
„Kein Duplikat“-Markierungen löschen, Lauf-Protokoll, Datenordner.

## Einstellungen

| Einstellung | Standard | Bedeutung |
|---|---|---|
| Unterordner, versteckte überspringen, Symlinks nicht verfolgen, Hardlinks nur einmal | an | |
| Auch Bilder vergleichen | aus | JPEG, PNG, WebP, GIF, AVIF, HEIC …; nur mit Bildern verglichen |
| Mindest-/Höchstgröße | – | in MB |
| Nur Pfade mit … / Pfade überspringen mit … | – | Texte durch `;` getrennt, `*` und `?` als Platzhalter |
| Mindestähnlichkeit | 90 % | 100 % = Bild für Bild gleich; neu kodierte Kopien 93–100 %, fremde Videos ~50 % |
| Vergleichsbilder je Video | 6 | gleichmäßig über die Laufzeit (bei 14 %, 29 %, … 86 %); ein abweichendes Bild ist erlaubt |
| Laufzeit darf abweichen um | 20 % | 100 = egal (langsamerer Vergleich) |
| Gespiegelte Videos erkennen | an | |
| Auch weiße Balken abschneiden | aus | schwarze Balken werden immer ignoriert |
| Ordner | alle | nur im selben / nur aus verschiedenen Ordnern |
| Mit der ganzen Datenbank vergleichen | aus | auch Videos früherer Suchläufe in anderen Ordnern |
| Teilclips suchen | aus | Ton jedes Videos wird einmal ganz gelesen; Mindestlänge 10 %, Ton 80 %, Bildprüfung 85 % |
| GPU-Prozesse | 2 | mehr bringen meist kaum etwas |
| CPU-Prozesse | Threads − 2 | je ein Video mit einem Thread |
| Anderen Programmen Vorrang lassen | an | siehe unten |

Eine Änderung von „Vergleichsbilder je Video“ oder „weiße Balken“ liest alle Videos neu ein.

## So funktioniert die Erkennung

- Je Video werden *k* Bilder an festen Stellen der Laufzeit gelesen (Sprung zum nächsten
  Keyframe, dann bildgenau weiterdekodiert; B-Frames auf dem Weg übersprungen, Entblocken aus –
  für ein 32×32-Bild unnötig, spart ~25 %).
- Jedes Bild wird auf 64×64 Graustufen verkleinert, schwarze (optional weiße) Balken werden
  abgeschnitten, der Rest auf 32×32 gebracht. Daraus entsteht ein **pHash** (DCT, 64 Bit) –
  unempfindlich gegen Neukodierung, Skalierung, Helligkeit; zusätzlich der pHash des
  gespiegelten Bilds. Einfarbige Bilder (Schwarzbild, Abblende) zählen nicht.
- Ähnlichkeit zweier Videos = 100 % − mittlerer Bitabstand/64 über alle vergleichbaren Bilder,
  das am stärksten abweichende Bild wird nicht gezählt (z. B. anderes Logo am Ende).
- Verglichen wird jedes Video mit jedem (nach Laufzeit sortiert, alle Kerne, früher Abbruch).
  Gemessen: 160 000 Videos in 4 s (Laufzeit ±20 %) bzw. 26 s (Laufzeit egal, 12,8 Mrd. Paare) –
  auf voll ausgelastetem PC.
- Ähnliche Paare bilden Gruppen.
- **Teilclips:** Chromaprint-Fingerabdruck des Tons (~8 Werte/s). Ein Index aus jedem 16.
  Wert findet Kandidaten mit gleichem Versatz, die dann Bit für Bit geprüft werden – und auf
  Wunsch an den Bildern an genau dieser Stelle der Quelle. Angezeigt wird, ab wo der Clip in der
  Quelle beginnt.

## Robustheit und Rücksicht

- Alles Dekodieren passiert im Hilfsprozess `duplikat-finder-worker` (/usr/lib/duplikat-finder).
  Ein Prozess bleibt für viele Dateien am Leben (kein Programmstart je Datei). Je Datei gilt
  eine Zeitgrenze (150 s) und eine Speichergrenze (2,5 GB, GPU 5 GB): wird sie überschritten
  oder stürzt der Prozess ab, wird er beendet und neu gestartet, die Datei einmal auf der
  anderen Hardware versucht und sonst als „nicht lesbar“ (mit Grund) gemerkt. Nicht lesbare
  Dateien werden bis zu einer Änderung nicht erneut versucht (abschaltbar).
- Kann die GPU ein Video nicht dekodieren (z. B. MPEG-4 ASP, WMV, 10-Bit-H.264), meldet der
  Prozess das sofort, und ein CPU-Prozess übernimmt.
- Alle Hilfsprozesse laufen mit niedriger CPU- und Platten-Priorität. Mit „Anderen Programmen
  Vorrang lassen“ misst das Programm alle 2 s die CPU-Last *ohne* die eigenen Prozesse: je
  ausgelastetem Kern eines anderen Programms pausiert ein CPU-Prozess (nach der aktuellen Datei),
  bei unter 1,2 GB freiem Speicher beginnen keine neuen Dateien.
- **Kein Standby während der Arbeit:** Solange ein Suchlauf läuft (auch der geplante, ohne
  geöffnetes Fenster) oder markierte Dateien verschoben/gelöscht werden, sperrt das Programm
  Standby und automatisches Ruhen – bei systemd-logind (`systemd-inhibit --list` zeigt den
  Eintrag) und in der Oberfläche zusätzlich über die Sitzung (auch das Abmelden). Danach wird
  die Sperre sofort aufgehoben; bei einem Absturz fällt sie von selbst weg. Bildschirmsperre
  und Bildschirmschoner bleiben unberührt.
- Die Datenbank wird alle 2 s geschrieben; Abbrechen, Absturz oder Strom weg kosten höchstens
  diese Sekunden.

## Gemessen (8-Kern-CPU, Grafikkarte mit VAAPI, Ubuntu 24.04)

- Testlauf mit rund 90 000 Dateien: Ordner lesen 6 s.
- Erstes Lesen (1080p/4K H.264, neben einer laufenden Videoumwandlung): je GPU-Prozess
  ~0,7 s je Datei, je CPU-Prozess ~3 s; zusammen 4–9 Dateien/s, je nach Last.
- Danach aus der Datenbank: 3 022 Dateien in 0,15 s (83 MB Arbeitsspeicher).
- CPU- und GPU-Fingerabdrücke derselben Datei weichen höchstens um 0–4 von 64 Bit ab – beide
  sind beliebig mischbar.

## Terminal-Version

```bash
duplikat-finder-cli [Optionen] ORDNER [ORDNER ...]
```

Wichtigste Optionen: `-a 90` (Ähnlichkeit), `-b 6` (Vergleichsbilder), `-l 20` (Laufzeit),
`-x ORDNER` (überspringen), `--fotos`, `--teilclips`, `--ganze-datenbank`, `--cpu N`,
`--gpu N`, `--volle-leistung`, `--json/--csv/--html DATEI`, `--test A B` (zwei Dateien
vergleichen), `--db-info`, `--db-aufraeumen`, `--db-verschieben ALT NEU`, `--zeitplan` (mit den
Einstellungen der Oberfläche; so ruft der geplante Suchlauf das Programm auf).
Mit JSON gespeicherte Ergebnisse öffnet die Oberfläche über „Ergebnis öffnen …“ oder
`duplikat-finder ergebnis.json`.

## Dateien

- `~/.duplikat-finder/dateien.db` – Datenbank (gelesene Dateien, Ton-Fingerabdrücke,
  „kein Duplikat“, ignorierte Dateien)
- `~/.duplikat-finder/gui.ini` – Einstellungen
- `~/.duplikat-finder/letztes-ergebnis.json` – aktuelle Trefferliste mit Markierungen
- `~/.duplikat-finder/geplantes-ergebnis.json` – Ergebnis des geplanten Suchlaufs
- `~/.duplikat-finder/lauf.log` – Protokoll des letzten Suchlaufs
- `~/.cache/duplikat-finder/vorschau/` – Vorschaubilder (darf jederzeit gelöscht werden)

## Quelltext

- `src/df_worker.c` – Hilfsprozess: libavformat/libavcodec (CPU und VAAPI), pHash, Chromaprint
- `src/df_proc.c` – Hilfsprozesse starten, befragen, überwachen; GPU-Erkennung; Vorschaubilder
- `src/df_scan.c` – Suchlauf: Ordner, Prozess-Pool mit Lastregelung, Vergleich, Teilclips, Gruppen
- `src/df_db.c` – SQLite-Datenbank
- `src/df_result.c` – Auswahlregeln, Dateiaktionen, Ergebnis speichern/laden, CSV/HTML, JSON
- `src/df_util.c` – Pfade, Protokoll, Formate, Systemwerte
- `src/df_gui.c`, `src/df_gui_results.c` – Oberfläche (GTK 3)
- `src/df_cli.c` – Terminal-Version

## Lizenz

Copyright (C) 2026 EureLordschaft008

Dieses Programm steht unter der GNU General Public License, Version 3 oder neuer
(GPL-3.0-or-later), siehe [LICENSE](LICENSE). Es wird ohne jede Gewähr bereitgestellt.
