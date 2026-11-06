# Mediathek Downloader 2.0.1

Lädt Filme und Serien aus den Mediatheken von ZDF, ZDFneo, ZDFtivi, ZDFinfo, phoenix, ARD, ONE,
ARD alpha, tagesschau24, WDR, SWR, NDR, MDR, BR, hr, SR, rbb, KiKA, funk, 3sat, ARTE (auch auf
Englisch, Französisch, Spanisch, Italienisch und Polnisch), SRF, NRK und Netzkino – einzelne Folgen,
ganze Staffeln oder eine Serie mit allen Staffeln auf einmal. Als Programm mit grafischer Oberfläche
(`mediathek-downloader`, Aufbau und Optik wie beim AV1-Converter) und als Terminal-Version
(`mediathek-downloader-cli`, z. B. für lange Läufe in tmux). Es läuft unter Linux und Windows.

Bis Version 1.2 hieß das Programm ZDF-Downloader. Beim ersten Start zieht es Einstellungen, Verlauf,
Warteschlange und Zwischenspeicher aus `~/.zdf-downloader` nach `~/.mediathek-downloader` um.

## Installieren

### Linux

Über die Paketquelle [VPRepo](https://github.com/EureLordschaft008/VPRepo):

```bash
sudo apt install mediathek-downloader
```

Oder direkt aus der Paketdatei:

```bash
sudo apt install ./mediathek-downloader_2.0.1-1_amd64.deb
```

Das Programm steht danach im Startmenü unter **Unterhaltungsmedien**. Es braucht `curl`, `ffmpeg`
und für den eingebauten Player GStreamer; `apt` installiert alles mit.

### Windows

`mediathek-downloader-2.0.1-setup.exe` von der
[Release-Seite](https://github.com/EureLordschaft008/mediathek-downloader/releases/latest) laden
und starten. Der Installer braucht keine Administratorrechte und legt das Programm nach
`%LOCALAPPDATA%\Programs\Mediathek Downloader`, dazu einen Eintrag im Startmenü. GTK, GStreamer,
ffmpeg und curl sind dabei. Die Terminal-Version liegt als `mediathek-downloader-cli.exe` im
Unterordner `bin`. Windows SmartScreen kennt den Installer nicht und warnt beim ersten Start
(„Weitere Informationen“ → „Trotzdem ausführen“), weil er nicht signiert ist.

Einstellungen, Verlauf und Protokoll liegen unter Windows in `%USERPROFILE%\.mediathek-downloader`.
Deinstallieren über die Windows-Einstellungen (Apps); Einstellungen und Videos bleiben dabei.

### Updates

Kurz nach dem Start fragt das Programm bei GitHub nach einer neuen Version. Gibt es eine, erscheint
oben eine Leiste mit „Installieren“, „Was ist neu?“ und „Überspringen“. „Installieren“ lädt das
Paket für das eigene System, prüft es gegen die `SHA256SUMS` des Releases und installiert es: unter
Linux per `pkexec apt-get install` (fragt nach dem Passwort), unter Windows über den Installer, der
das Programm beendet, die Dateien ersetzt und es wieder starten kann. Die Suche lässt sich in den
Einstellungen abschalten; das Menü hat „Nach Updates suchen“. Wer VPRepo eingerichtet hat, bekommt
das Update unter Linux auch über `sudo apt upgrade`.

## Was es kann

- **Mediatheken** – die Vorschau läuft wahlweise über eine, mehrere oder alle Mediatheken. Aus
  „Alle“ heraus wählt ein Klick genau eine, weitere Klicks nehmen andere dazu. Die Auswahl bleibt
  gespeichert.
- **Vorschau** – die Startseite zeigt neue Filme und Serien als Kacheln mit Titelbild. Ein Häkchen
  merkt einen Film vor, bei einer Serie alle fehlenden Folgen. Ein Klick auf die Kachel öffnet
  Bild, Beschreibung und alle Staffeln und Folgen zum einzelnen Auswählen. „Herunterladen“ startet
  alles auf einmal. Das Feld oben filtert die geladenen Titel nach Namen, das Menü daneben nach
  Sprache der Tonspur. Fehlt die Seite eines Titels, kommt das Bild als Standbild aus dem Video.
- **Kategorien** – Knöpfe über den Kacheln filtern nach Genres wie Komödie, Drama, Krimi oder
  Thriller, dazu die Wahl zwischen Filmen und Serien. Ein Titel kann in mehreren Kategorien stehen,
  darum ist die Summe größer als die Zahl der Titel. Die Kategorien stammen von der Mediathek und
  von IMDb. „Ohne Kategorie“ sammelt, was keine von beiden kennt. Beim ersten Start sammelt das
  Programm alles im Hintergrund, das dauert einige Minuten und lädt einmalig etwa 230 MB von IMDb.
  Danach kommen sie aus dem Zwischenspeicher, nur neue Titel werden nachgeholt.
- **Details beim Überfahren** – ruht die Maus kurz auf einer Kachel, zeigt ein Popup Altersfreigabe,
  Länge, Datum, Kategorien, IMDb-Bewertung und Beschreibung. Dieselben Angaben stehen in der
  Detailansicht.
- **IMDb** – der gelbe Knopf an Filmen und Serien öffnet die IMDb-Seite im Browser. Passt kein
  Treffer sicher, öffnet sich die IMDb-Suche zum Titel, und es erscheint keine Bewertung. So zeigt
  das Programm nie die Bewertung eines falschen Titels. Das trifft vor allem deutsche Titel, die bei
  IMDb anders heißen.
- **Staffeln und Folgen** – die Detailansicht zeigt Serie → Staffel → Folge; jede Ebene hat ein
  Häkchen. Eine Serie oder Staffel wählt nur aus, was noch fehlt. Die Folgen kommen aus allen
  Mediatheken: Bietet die eine nur einen Teil einer Staffel, kommen die übrigen von einer anderen,
  auch wenn diese in der Vorschau gerade nicht ausgewählt ist. Eine schon geladene Folge lässt
  sich nicht aus Versehen doppelt laden. Ein ⚠ an der Staffel warnt, wenn Folgen in der Nummerierung
  fehlen. Fehlt nur das Ende einer Staffel, ist das nicht zu erkennen.
- **Was ist schon da?** – Jede Folge trägt ihren Stand: *Geladen*, *In Warteschlange*, *Angefangen*
  oder *Datei fehlt*. Serien und Staffeln zeigen „2 von 265 geladen“. Der Filter oben blendet alles
  außer den fehlenden oder den geladenen Titeln aus.
- **Neue Folgen** – die gleichnamige Seite listet alle Serien, von denen du schon etwas geladen hast
  und die seit dem letzten Download neue Folgen bekommen haben, auch wenn sie nicht in der Vorschau
  stehen. Ein Klick wählt alle aus, „Herunterladen“ startet sie. In Vorschau und Bibliothek markiert
  ein kleines grünes „!“ dieselben Serien. Während der Suche zeigt die Seite, welche Serie gerade
  dran ist und wie weit es ist; ein Spinner in der Titelleiste zeigt es auf allen Seiten. Mit
  „Neue Folgen automatisch laden“ (Einstellungen) kommen sie von selbst in die Warteschlange, mit
  allen Sprachen und Untertiteln; gesucht wird beim Start und danach alle sechs Stunden.
- **Meine Videos** – alles Geladene als Kacheln wie in einer Mediathek: oben „Weiterschauen“ und
  „Zuletzt geladen“ zum seitlichen Blättern, darunter Serien und Filme von A bis Z. Eine Serie
  öffnet sich mit ihren Staffeln und Folgen; der große Knopf spielt die angefangene oder die nächste
  ungesehene Folge. Bilder kommen von der Mediathek, sonst als Standbild aus der Datei. Ein Balken
  zeigt, wie weit eine Datei oder Serie geschaut ist („3 von 10 gesehen“). Die rechte Maustaste
  markiert als gesehen oder ungesehen, öffnet Ordner oder ein anderes Programm und verschiebt Filme,
  Folgen oder ganze Serien nach Rückfrage in den Papierkorb; leere Staffelordner verschwinden mit.
  Der Verlauf behält sie, damit die Suche nach neuen Folgen sie nicht wieder meldet.
- **Ordner prüfen** – gleicht den Speicherort mit dem Verlauf ab: Verschobene Dateien bekommen ihren
  neuen Ort, Dateien, die das Programm noch nicht kennt (etwa aus einer Sicherung), erscheinen in
  „Meine Videos“ und zählen als geladen, sofern ihr Name dem üblichen Schema folgt. Einträge ohne
  Datei lassen sich auf Wunsch aus dem Verlauf entfernen.
- **Statistik** – Sehzeit insgesamt, der letzten 7 Tage und je Tag, Wochentag und Uhrzeit, Tage am
  Stück, die meistgesehenen Titel, was wann geladen wurde, die meistgeladenen Serien, der Platz auf
  der Festplatte, wie viel davon schon gesehen ist, und die größten Dateien. Sehzeit zählt nur im
  eingebauten Player und nur, was wirklich lief; sie liegt in `~/.mediathek-downloader/sehzeit.tsv`
  und bleibt, auch wenn die Datei gelöscht wird.
- **Player** – spielt die Dateien im Hauptfenster, auch AV1; im Vollbild verschwindet die
  Titelleiste. Er merkt sich, wo eine Datei aufgehört hat, macht dort weiter und startet am Ende die
  nächste Folge. Tonspur und Untertitel lassen sich während der Wiedergabe wechseln bzw. an- und
  ausschalten. Der Zoom blendet schwarze Balken aus: „Schwarze Balken ausblenden“ sucht per ffmpeg
  nach Balken, die fest im Bild sind, und füllt das Fenster mit dem Inhalt. Tasten: Leertaste
  Pause, ←/→ 10 Sekunden, ↑/↓ Lautstärke, A Tonspur, S Untertitel, Z Balken aus/ein, +/− Zoom,
  0 ganzes Bild, M stumm, F oder Doppelklick Vollbild, N nächste Folge, Esc zurück. Springen,
  Lautstärke, Zoom und Spurwechsel blenden die Leiste nicht ein, sondern nur einen kurzen Hinweis.
  Ab 95 % gilt eine Datei als gesehen. Der Stand liegt in `~/.mediathek-downloader/wiedergabe.tsv`.
- **Fenstergröße** – die Kacheln füllen jede Breite, Leisten brechen um, und unter etwa 1000 Pixeln
  Breite werden die Reiter oben zu Symbolen (der Name steht im Tooltip). Das Fenster lässt sich so
  bis etwa 520 Pixel schmal machen.
- **Bibliothek** – alles Geladene nach Sendung, mit Größe, Datum, Abspielen und Ordner öffnen. Das
  Lupensymbol zeigt die fehlenden Folgen einer Serie in der Vorschau, sofern sie dort steht.
- **Sprachen und Untertitel** – Gibt es mehrere Tonspuren oder Untertitel, fragt das Programm vor
  dem Download. Standard: alles laden. Alles landet zusammen in einer MKV-Datei.
- **Pause** – einzeln oder alle. Auch nach dem Beenden oder einem Absturz geht es an derselben
  Stelle weiter; offene Downloads warten beim nächsten Start pausiert auf einen Klick. Downloads und
  Umwandlung lassen sich außerdem getrennt anhalten (Seite Downloads, „Steuerung“): Dann laden etwa
  die Downloads weiter, während die Umwandlung stillsteht.
- **Downloadliste** – oben steht, was gerade läuft, dann Fehlgeschlagenes, Wartendes und zuletzt
  Fertiges. Die Liste zeigt 100 Aufträge, „Weitere anzeigen“ holt die nächsten; so bleibt sie auch
  mit Tausenden Einträgen flüssig. „Fertige entfernen“ räumt Erledigtes weg, „Fehlgeschlagene
  entfernen“ alle Fehlschläge samt ihrer halbfertigen Dateien.
- **Tempolimit** – gilt für alle Downloads zusammen und sofort, auch mitten im Download.
- **AV1** – auf Wunsch wandelt das Programm jedes Video nach dem Download um, mit den Einstellungen
  des AV1-Converters. Schwarze Balken, die fest im Bild sind (etwa bei 4:3-Serien oder Kinofilmen),
  schneidet es dabei ab; ffmpeg prüft dafür fünf Stellen im Video, und im Zweifel bleibt das Bild
  ganz. Das Ergebnis wird geprüft; erst dann verschwindet die MP4-Datei.
- **Autostart** – auf Wunsch startet das Programm minimiert mit der Anmeldung (Linux:
  `~/.config/autostart`, Windows: Registry unter „Run“).
- **Speicherort** – frei wählbar, Standard ist `~/Videos/Mediathek` (Windows: `Videos\Mediathek`).
- **Zwischenspeicher** – Titelbilder, Kategorien, FSK und IMDb-Zuordnung bleiben dauerhaft liegen
  und werden nicht neu geladen; verkleinerte Titelbilder hält das Programm zusätzlich im Speicher,
  und hat ein Titel kein Bild, fragt es erst nach sieben Tagen wieder nach. Die Film- und
  Folgenlisten der Vorschau gelten sechs Stunden; der
  Knopf „Vorschau neu laden“ holt sie sofort frisch. Der Ordner ist frei wählbar, Standard ist
  `~/.mediathek-downloader/bilder`. Halbfertige Downloads (`.part`) bleiben beim Speicherort.

## Wo die Dateien landen

```
<Speicherort>/Serien/Löwenzahn mit Fritz Fuchs/Staffel 45/Löwenzahn mit Fritz Fuchs - S45E11 - Tierärzte - Hilfe für Keks.mkv
<Speicherort>/Filme/Air - Der große Wurf.mkv
```

Eine Folge gilt als geladen, sobald sie im Verlauf steht und die Datei noch da ist – egal in welcher
Qualität oder an welchem Speicherort. Wird eine Datei verschoben, zeigt das Programm *Datei fehlt*;
der Eintrag lässt sich in der Bibliothek entfernen, die Folge dann neu laden.

Einstellungen, Warteschlange, Verlauf und Protokoll liegen in `~/.mediathek-downloader`.

## Terminal-Version

```bash
mediathek-downloader-cli --serien --staffel 26 --nur-fehlende "Löwenzahn"
```

```bash
mediathek-downloader-cli --serien --staffel 26 --laden alle --ziel ~/Videos/Mediathek --limit 10 --av1 gpu "Löwenzahn"
```

Ohne `--laden` zeigt das Programm nur die Trefferliste mit dem Stand jedes Titels. Strg+C pausiert;
derselbe Befehl macht dort weiter. `--mediathek arte,srf` beschränkt die Suche auf einzelne
Mediatheken. Alle Optionen: `mediathek-downloader-cli --help`. Die Warteschlange
der Oberfläche nutzt die Terminal-Version nicht, den Verlauf schon.

## Qualität und AV1

| Stufe | Bild | Größe je 90 min |
|---|---|---|
| Hoch (Standard) | 1080p, 50 Bilder/s, H.264 | ca. 4,5 GB |
| Mittel | kleiner als 1080p | ca. 2,3 GB |
| Niedrig | für unterwegs | ca. 0,5 GB |

| | GPU (Standard) | CPU |
|---|---|---|
| Encoder | `av1_vaapi`, Qualität 64; Windows: `av1_nvenc`, `av1_qsv` oder `av1_amf` | SVT-AV1, Preset 6, RF 35 |
| Tempo | sehr schnell | sehr viel langsamer |
| Ton, Untertitel | unverändert | unverändert |

Änderungen an Encoder und Qualität gelten sofort für alle offenen Aufträge, nur eine laufende
Umwandlung läuft mit den alten Werten zu Ende. Scheitert die GPU, übernimmt die CPU. Wird das Ergebnis nicht kleiner, bleibt die MP4-Datei.

## Grenzen

- **Kein 4K.** Die Suche liefert höchstens 1080p. 4K-Fassungen gibt es nur für wenige Titel und
  nur über die Datenquelle des ZDF selbst, die das Programm nicht nutzt.
- **Geoblocking.** Manche Titel liegen auf Adressen, die nur aus Deutschland erreichbar sind.
- **Mediathek-Fristen.** Geladen werden kann nur, was gerade online steht. Die Serie zeigt also nur
  die Folgen, die es derzeit gibt.
- **Staffeln stimmen nur so weit wie die Angaben.** Ohne `(S18/E05)` im Titel steht eine Sendung
  unter „Einzelne Sendungen“.
- **Fassungen mit Gebärdensprache** sind eigene Einträge, weil die Einblendung fest im Bild ist.
  Sie sind ausgeblendet und lassen sich in den Einstellungen einschalten.
- **Titelbilder** kommen von den Seiten der Mediatheken. Eine Serie zeigt das Bild ihrer neuesten
  Folge. Viele ältere Titel haben keine Seite mehr, nur noch das Video; ihr Standbild kann
  unpassend sein.
- **Kategorien** stammen beim ZDF von dessen Seiten. Die anderen Mediatheken nennen keine; dort leitet das
  Programm sie aus Stichwörtern ab, zum Beispiel Krimi bei „Tatort“. IMDb ergänzt alle Mediatheken,
  aber nur bei sicherem Titeltreffer. Eine Serie übernimmt die Kategorien ihrer neuesten Folge.
- **Staffeln bei der ARD:** Nennt ein Titel nur „Folge 5“, zählt er zu Staffel 1. Serien ganz ohne
  Nummern wie der Tatort stehen unter „Einzelne Sendungen“.
- **ARTE und SRF** liefern Streams statt Dateien. Das Programm setzt sie mit ffmpeg zu einer
  MP4-Datei zusammen, bei ARTE mit allen Sprachfassungen als Tonspuren. Ein pausierter Stream
  beginnt beim Fortsetzen von vorn. Verschlüsselte Streams und Livesendungen lädt es nicht.
- **SRF weltweit:** Nur Titel, die auch außerhalb der Schweiz freigegeben sind, erscheinen. Bei
  ARTE und SRF stehen Staffel und Folge oft nur als „(3/6)“ im Titel.
- **NRK** liefert nur Serien, und nur Folgen, die die NRK weltweit freigibt (meist eigene
  Produktionen), auf Norwegisch. Die Liste kommt von den Serien der NRK-Startseite; über die Suche
  der Terminal-Version findet sich mehr.
- **ARTE intl.** fasst die anderen Sprachfassungen von ARTE zusammen; Titel und Untertitel sind in
  der jeweiligen Sprache.
- **Private Sender und Bezahldienste** gibt es nicht: Sie stehen nicht in der Datenquelle, ihre
  Player sind auf Kopierschutz ausgelegt, und das Programm umgeht keinen Kopierschutz.
- **Netzkino** liefert nur Filme, und nur solche ohne Kopierschutz und außerhalb von NetzkinoPlus.
  Länge und Größe nennt der Dienst nicht, die Qualitätsstufe wirkt dort nicht. Die Dateien sind oft
  sehr groß, teils über 10 GB. Netzkino finanziert sich über Werbung, die beim Download entfällt.
- **Titelbilder bei WDR, SWR, NDR und rbb** sind Standbilder aus dem Video, und FSK fehlt dort.
  Bei ARTE und SRF kommt das Bild nur von der Seite; ohne Seite bleibt der Titel ohne Bild.
- **Titellisten** kommen von [MediathekViewWeb](https://mediathekviewweb.de), nicht von den Sendern.
  Ist dieser Dienst nicht erreichbar, gibt es keine Titel.
- **Freie Suche** gibt es nur in der Terminal-Version. Die Vorschau zeigt die neuesten Titel, was
  dort nicht steht, findet nur `mediathek-downloader-cli`.
- **Windows:** Der Papierkorb, der Player und die GPU-Umwandlung laufen über die Windows-Fassungen
  von GTK, GStreamer und ffmpeg. Ohne NVIDIA-, Intel- oder AMD-Karte mit AV1-Encoder übernimmt die
  CPU. Die Fenster sehen nach GTK aus, nicht nach Windows.

## Rechtliches

Information courtesy of IMDb (https://www.imdb.com). Used with permission. Die Bewertungen stammen
aus der [offiziellen Datendatei](https://developer.imdb.com/non-commercial-datasets/) von IMDb, die
nur für den privaten, nicht kommerziellen Gebrauch freigegeben ist. Die Bewertungen werden wöchentlich
neu geladen und belegen etwa 25 MB im Zwischenspeicher, die Kategorien monatlich mit etwa 26 MB.

Das Programm lädt frei zugängliche Dateien ohne Kopierschutz; es umgeht keine technischen
Schutzmaßnahmen. Kopien zum privaten Gebrauch sind nach § 53 UrhG grundsätzlich erlaubt. Die Dateien
bleiben urheberrechtlich geschützt: nicht weitergeben, nicht veröffentlichen. Das ist keine
Rechtsberatung.

## Neue Version veröffentlichen

`VERSION` im Makefile und `MD_VERSION` in `src/md_core.h` erhöhen, committen und den Tag `v<Version>`
pushen. GitHub Actions baut dann das .deb und den Windows-Installer, legt beide mit `SHA256SUMS`
als Release an und bringt das .deb nach VPRepo. Das Release ist zugleich die Quelle der Updates im
Programm. Von Hand gestartet (`gh workflow run release.yml`) baut der Workflow beide Pakete nur als
Artefakte, ohne Release.

## Aus dem Quelltext bauen

Gebraucht werden `gcc`, `make`, `libgtk-3-dev`, `libgstreamer1.0-dev` und `dpkg-deb`.

```bash
make deb
```

Windows: In [MSYS2](https://www.msys2.org) (Umgebung UCRT64) die Pakete aus
`.github/workflows/release.yml` installieren und `windows/build.sh` aufrufen; das Ergebnis ist
`mediathek-downloader-<Version>-setup.exe`.

`make test` führt die Regressionstests und einen kurzen Fuzz-Lauf aus. Die Tests brauchen `curl` und
legen alles in einem temporären Ordner ab. `make test ASAN=1` nimmt AddressSanitizer dazu. Das geht
nur, wenn `/etc/ld.so.preload` nichts vorlädt.

| Datei | Aufgabe |
|---|---|
| `src/md_core.c` | Engine: Suche, Warteschlange, Download, Umwandlung, Verlauf |
| `src/md_gui.c` | grafische Oberfläche (GTK 3) |
| `src/md_player.c` | eingebauter Player (GStreamer) |
| `src/md_cli.c` | Terminal-Version |
| `src/md_stats.c` | Reiter Statistik |
| `data/` | Starter, Symbol, Paketbeschreibung |
| `windows/` | Build-Skript und Installer für Windows |

## Lizenz

Copyright (C) 2026 EureLordschaft008

Dieses Programm steht unter der GNU General Public License, Version 3 oder neuer
(GPL-3.0-or-later), siehe [LICENSE](LICENSE). Es wird ohne jede Gewähr bereitgestellt.
