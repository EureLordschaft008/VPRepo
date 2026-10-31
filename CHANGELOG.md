# Änderungsprotokoll

Die Einträge stammen aus der Commit-Historie der Programme. Die neueste Version steht jeweils oben.
Für Versionen vor dem ersten Eintrag gibt es keine Aufzeichnungen. Bei einer Angabe wie `5.6.0-3`
ist die letzte Zahl die Paketversion: Das Programm bleibt gleich, nur das Paket ist neu.

## AV1-Converter

### Noch ohne neues Paket
- Lizenz GPL-3.0-or-later.

### 1.4.0-2 (31.10.2026)
- Neue Versionen werden automatisch gebaut und kommen über die Paketquelle.

### 1.4.0 (17.10.2026)
- Erster Eintrag: Videos eines Ordners samt Unterordnern nach AV1 umwandeln, mit den Einstellungen
  des HandBrake-Profils „AV1 MKV 2160p60 4K“.
- Kodieren per CPU (HandBrake) oder GPU (ffmpeg mit VAAPI).
- HDR-Angaben per `mkvpropedit` im MKV-Container.
- Grafische Oberfläche (GTK 3) und Terminal-Version.

## Intro-Cutter

### 5.6.0-3 (31.10.2026)
- Text im Über-Dialog geändert.
- Lizenz GPL-3.0-or-later.

### 5.6.0-2 (31.10.2026)
- Neue Versionen werden automatisch gebaut und kommen über die Paketquelle.
- Dieses Paket wurde zurückgezogen und durch 5.6.0-3 ersetzt.

### 5.6.0 (17.10.2026)
- Erster Eintrag: Intro am Anfang, Outro am Ende oder automatische Erkennung ohne Referenz-Video,
  auch für mehrere verschiedene Intros, mit bildgenauem Schnitt je Video. Im Zweifel wird nicht
  geschnitten.
- Schnitt per Copy, CPU, GPU (VAAPI) oder GPU + CPU parallel.
- Intro-Ende bildgenau am Video festlegen, Restdauer der Bearbeitung, Hash-Cache ansehen und
  aufräumen.
- Grafische Oberfläche (GTK 3), Terminal-Version und Debian-Paket.

## Duplikat-Finder

### Noch ohne neues Paket
- Lizenz GPL-3.0-or-later.

### 1.0.3-2 (31.10.2026)
- Neue Versionen werden automatisch gebaut und kommen über die Paketquelle.

### 1.0.3 (17.10.2026)
- Erster Eintrag: Doppelte Videos und Bilder anhand des Inhalts finden.
- Hilfsprozesse mit CPU und GPU (VAAPI) parallel, Vergleich per pHash, SQLite-Datenbank.
- Teilclips per Chromaprint, Auswahlregeln und Dateiaktionen.
- Grafische Oberfläche (GTK 3), Terminal-Version und Debian-Paket.

## Mediathek Downloader

Bis Version 1.2 hieß das Programm ZDF-Downloader.

### Noch ohne neues Paket
- Lizenz GPL-3.0-or-later.

### 1.6.0 (31.10.2026)
- Netzkino als eigene Quelle. Dabei sind nur frei verfügbare Filme ohne Kopierschutz, NetzkinoPlus
  bleibt draußen. Bild, FSK und Genres kommen gleich mit der Filmliste.

### 1.5.0 (31.10.2026)
- Neue Seite „Neue Folgen“: Sie prüft jede Serie aus dem Verlauf, auch wenn sie nicht in der Vorschau
  steht.
- Neue Mediatheken: ZDFneo, ONE, WDR, SWR, NDR und rbb.
- Der Filter „Alle anzeigen“ entfällt.

### 1.4.2 (31.10.2026)
- Grünes Ausrufezeichen an Serien mit neuen Folgen, in Vorschau und Bibliothek. Es gilt für Serien,
  von denen schon etwas geladen ist. Eine Folge zählt erst ab ihrer Sendezeit als neu, weil Sender
  sie vorher online stellen.

### 1.4.1 (31.10.2026)
- Enthält die Änderungen von 1.4.0 mit korrekter Paketversion. 1.4.0 wurde nie veröffentlicht.

### 1.4.0 (31.10.2026, nicht veröffentlicht)
- Kategorien von IMDb, Sprachfilter und Warnzeichen für lückenhafte Staffeln.
- Vorübergehende Fehler beim Laden einer Seite gelten nicht mehr als dauerhaft. Einträge aus 1.3.0
  reparieren sich selbst.
- Titel ohne Seite bekommen ein Standbild aus dem Video.
- Gleiche Titel aus mehreren Mediatheken stehen nur noch einmal in der Liste.
- „Spielfilm“ ist keine Kategorie mehr, dafür gibt es „Ohne Kategorie“.
- Die Seite Suche entfällt.

### 1.3.0 (31.10.2026)
- ARD und 3sat kommen dazu, Suche und Vorschau laufen über mehrere Mediatheken.
- Umbenannt in Mediathek Downloader. Das Paket löst `zdf-downloader` ab, und `~/.zdf-downloader`
  zieht samt Warteschlange und Cacheordner-Einstellung nach `~/.mediathek-downloader` um.

### 1.2.0 (31.10.2026)
- Die Vorschau lässt sich nach den ZDF-Genres und nach Filmen oder Serien filtern.
- Ruht die Maus auf einer Kachel, zeigt ein Popup FSK, Länge, Datum, Kategorien, IMDb-Bewertung und
  Beschreibung. Die Bewertungen stammen aus der offiziellen Datendatei von IMDb.
- Listen und IMDb-Zuordnungen bleiben im Zwischenspeicher.
- Der IMDb-Knopf sitzt jetzt unter dem Bild.

### 1.1.1 (31.10.2026)
- Titelbilder vom zweiten ZDF-Bildserver werden geladen. Etwa jeder vierte Titel hat sein Bild
  dort, diese Titel galten bisher dauerhaft als „kein Bild“. Alte Merker gelten nicht mehr, neue
  laufen nach sieben Tagen ab.

### 1.1.0 (31.10.2026)
- Neue Startseite mit Kacheln für neue Filme und Serien, Detailansicht mit allen Staffeln.
- IMDb-Knopf an Filmen und Serien.
- Einstellbarer Ordner für die Titelbilder.

### 1.0.0 (31.10.2026)
- Erster Eintrag: Filme und Serien aus der ZDF-Mediathek laden, einzelne Folgen, Staffeln oder
  ganze Serien, mit Auswahl von Sprachen und Untertiteln.
- Pause, Tempolimit, Verlauf und auf Wunsch Umwandlung nach AV1.
- Grafische Oberfläche (GTK 3), Terminal-Version und Debian-Paket.
