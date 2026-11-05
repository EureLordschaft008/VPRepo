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

### 5.6.1 (31.10.2026)
- Das Vorschaubild entsteht in einer sicher angelegten Temp-Datei statt unter einem vorhersagbaren
  Namen in `/tmp`.

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

### 1.0.4 (31.10.2026)
- Lizenz GPL-3.0-or-later.
- Ist „Symbolische Links nicht verfolgen“ aus, erscheint eine verlinkte Datei unter ihrem echten
  Pfad und nicht mehr als Duplikat ihrer selbst.
- Hardlink- und Symlink-Aktionen ersetzen eine Datei nicht mehr, wenn das Ziel fehlt oder auf sie
  selbst zeigt, und lassen keine `.df-link-*`-Datei mehr liegen.

### 1.0.3-2 (31.10.2026)
- Neue Versionen werden automatisch gebaut und kommen über die Paketquelle.

### 1.0.3 (17.10.2026)
- Erster Eintrag: Doppelte Videos und Bilder anhand des Inhalts finden.
- Hilfsprozesse mit CPU und GPU (VAAPI) parallel, Vergleich per pHash, SQLite-Datenbank.
- Teilclips per Chromaprint, Auswahlregeln und Dateiaktionen.
- Grafische Oberfläche (GTK 3), Terminal-Version und Debian-Paket.

## Mediathek Downloader

Bis Version 1.2 hieß das Programm ZDF-Downloader.

### 2.0.1 (06.11.2026)
- Neu für Windows: Installer ohne Administratorrechte von der Release-Seite auf GitHub, mit GTK,
  GStreamer, ffmpeg und curl. Die GPU-Umwandlung nutzt dort NVENC, QSV oder AMF.
- Updates: Kurz nach dem Start fragt das Programm bei GitHub nach einer neuen Version und
  installiert sie auf Wunsch, nach Prüfung der Prüfsumme. Abschaltbar in den Einstellungen.
- Eine Staffel lädt alle verfügbaren Folgen, auch wenn sie aus verschiedenen Mediatheken stammen.
- ORF, DW, Internet Archive und Wikimedia Commons entfallen.
- Version 2.0.0 wurde nie veröffentlicht.

### 1.9.0 (05.11.2026)
- Neuer Reiter „Statistik“: Sehzeit je Tag, Wochentag und Uhrzeit, meistgesehene und
  meistgeladene Titel, Downloads je Monat und Platz auf der Festplatte.
- „Meine Videos“: Filme, Folgen und ganze Serien in den Papierkorb verschieben, Fortschritt je
  Serie („3 von 10 gesehen“) und „Ordner prüfen“, das Speicherort und Verlauf abgleicht.
- Neue Quellen: ZDFinfo, phoenix, hr, SR, ARD alpha, tagesschau24, DW, funk, ARTE in fünf
  weiteren Sprachen, NRK (nur weltweit freigegebene Folgen), Internet Archive und Wikimedia Commons
  (gemeinfreie und frei lizenzierte Filme).

### 1.8.0 (05.11.2026)
- Sieben neue Mediatheken: ARTE, MDR, BR, SRF, ORF, KiKA und ZDFtivi.
- ARTE, SRF und ORF kommen als Stream und werden zu MP4 zusammengesetzt, bei ARTE mit allen
  Sprachfassungen als Tonspuren. Verschlüsselte Streams und Livesendungen lädt das Programm nicht.
- Bei SRF und ORF erscheinen nur weltweit freigegebene Titel.

### 1.7.0 (05.11.2026)
- Neuer Reiter „Meine Videos“: alles Geladene als Kacheln mit Bild, „Weiterschauen“ und „Zuletzt
  geladen“, Serien mit Staffeln und Folgen, gesehen/ungesehen per rechter Maustaste.
- Eingebauter Player (GStreamer): setzt an der letzten Stelle fort, spielt die nächste Folge,
  Ton- und Untertitelwahl, Vollbild und Tastensteuerung.
- Die Suche nach neuen Folgen zeigt deutlich, welche Serie gerade geprüft wird und wie weit sie
  ist, dazu ein Spinner in der Titelleiste.
- Schmale Fenster: Leisten brechen um, die Reiter werden zu Symbolen.

### 1.6.2 (31.10.2026)
- Adressen aus Antworten der Mediatheken werden geprüft, curl nutzt nur noch HTTP und HTTPS.
- Sehr lange Serien- und Folgentitel führen nicht mehr zu einem zu langen Dateinamen.
- Der Lizenztext liegt dem Paket bei.

### 1.6.1 (31.10.2026)
- Änderungen an Encoder und Qualität gelten sofort für alle offenen Aufträge.
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
