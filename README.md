# VPRepo

APT-Paketquelle für Linux Mint 22 und Ubuntu 24.04 (amd64) mit vier Programmen rund um
Videosammlungen:

| Programm | Paket | Aufgabe |
|---|---|---|
| [AV1-Converter](#av1-converter) | `av1-converter` | Videos nach AV1 umwandeln und Speicherplatz sparen |
| [Intro-Cutter](#intro-cutter) | `intro-cutter` | Wiederkehrende Intros und Outros erkennen und herausschneiden |
| [Duplikat-Finder](#duplikat-finder) | `duplikat-finder` | Doppelte Videos und Bilder anhand des Inhalts finden |
| [Mediathek Downloader](#mediathek-downloader) | `mediathek-downloader` | Filme und Serien aus den Mediatheken von ZDF, ARD und weiteren Sendern laden |

Alle vier sind in C geschrieben und haben eine grafische Oberfläche (GTK 3) sowie eine
Terminal-Version. Dieses Repository enthält die fertigen Pakete und unter
[`quellcode/`](quellcode) den Quellcode aller vier Programme. Wie sie sich selbst bauen lassen,
steht jeweils in deren README. Was sich je Version geändert hat, steht in
[`CHANGELOG.md`](CHANGELOG.md).

## Installieren

Die Paketquelle einmalig einbinden und die gewünschten Programme installieren:

```bash
sudo install -d -m 755 /etc/apt/keyrings
curl -fsSL https://eurelordschaft008.github.io/VPRepo/vprepo.gpg | sudo tee /etc/apt/keyrings/vprepo.gpg >/dev/null
echo 'deb [signed-by=/etc/apt/keyrings/vprepo.gpg] https://eurelordschaft008.github.io/VPRepo stable main' | sudo tee /etc/apt/sources.list.d/vprepo.list
sudo apt update
sudo apt install av1-converter intro-cutter duplikat-finder mediathek-downloader
```

Neue Versionen erscheinen danach automatisch in der Aktualisierungsverwaltung von Linux Mint
(unter Ubuntu in der Aktualisierungsverwaltung bzw. bei `sudo apt upgrade`). Die Programme stehen
im Anwendungsmenü unter „Sound & Video“.

## AV1-Converter

Wandelt alle Videos eines Ordners samt Unterordnern nach AV1 um, mit den Einstellungen des
HandBrake-Profils „AV1 MKV 2160p60 4K“. Gestartet wird `av1-converter` (Oberfläche) oder
`av1-converter-cli` (Terminal, z. B. für lange Läufe in tmux).

**Kodieren per GPU, CPU oder beidem**

- **GPU:** ffmpeg mit VAAPI (`av1_vaapi`), ein Vielfaches schneller als die CPU und deshalb
  der Standard. Nötig ist eine Grafikkarte, die AV1 per VAAPI kodieren kann.
- **CPU:** HandBrake selbst mit genau diesem Profil (Flatpak `fr.handbrake.ghb` oder
  `HandBrakeCLI`). Langsamer, die Dateien werden aber meist kleiner. Ohne HandBrake nutzt das
  Programm ffmpeg mit SVT-AV1 und denselben Werten.
- **GPU + CPU parallel:** für möglichst kleine Dateien. Bringt die GPU bei einer Datei keinen
  Gewinn, versucht es HandBrake.

**Sicher für die Originale**

- Kodiert wird in eine versteckte Temp-Datei. Erst nach der Prüfung (AV1-Spur vorhanden, Dauer
  stimmt) wird das Original ersetzt oder die Datei in den Ausgabeordner gelegt.
- Ist die neue Datei nicht mindestens um die Mindestersparnis kleiner (Standard 10 %), bleibt
  das Original. GPU-Jobs erkennen das schon früh und brechen ab.
- Originale lassen sich sofort oder später ersetzen, auch nach einem Neustart.
- Scheitert ein GPU-Job, wird er mit CPU-Dekoder und danach mit HandBrake wiederholt.
- Disc-Ordner (`BDMV`, `VIDEO_TS`), versteckte Ordner und Symlinks bleiben unberührt.

**Für große Sammlungen**

- Bearbeitete Dateien werden gemerkt. Ein neuer Lauf macht auch nach Abbruch, Absturz oder
  Neustart dort weiter, schon vorhandene AV1-Videos werden übersprungen.
- Getestet mit 90 000 Videos: Die erste Suche prüft jede Datei, jede weitere kommt aus dem Cache
  und dauert 1,6 s.
- Die Restdauer samt Fertig-Zeitpunkt lernt das tatsächliche Tempo. Pause, Abbruch und
  Herunterfahren nach dem Ende sind möglich.
- Encoder laufen mit niedriger Priorität, Jobs warten bei knappem Speicher oder Plattenplatz,
  und Standby ist während der Arbeit gesperrt.

**Bild, Ton und HDR**

- Ergebnis ist 10-Bit-AV1 in MKV mit Auto-Crop, höchstens 3840×2160 und 60 Bildern/s, Kapitel
  bleiben erhalten.
- Standardmäßig wird die erste Tonspur zu Opus 160 kbit/s Stereo. Wahlweise bleiben alle
  Tonspuren als Opus oder unverändert, auch alle Untertitel lassen sich übernehmen.
- HDR10 per GPU behält die Helligkeitsangaben, weil das Programm sie danach per `mkvpropedit` in
  den MKV-Container schreibt. Dolby Vision und HDR10+ gehen an HandBrake.

## Intro-Cutter

Erkennt ein wiederkehrendes Intro oder Outro in vielen Videos samt Unterordnern und schneidet es
heraus. Gestartet wird `intro-cutter` (Oberfläche) oder `intro-cutter-cli` (Terminal).

**Drei Arten der Erkennung**

- **Intro am Anfang:** Mit einem Referenz-Video und einer Länge wird alles bis zu dieser Stelle
  vorne abgeschnitten.
- **Outro am Ende:** Die angegebene Länge wird am Ende jedes Videos entfernt. Erkannt wird das
  Outro an einem Vergleichsbild in seiner Mitte.
- **Automatisch:** Ganz ohne Referenz-Video findet das Programm alle wiederkehrenden Intros im
  Ordner, auch mehrere verschiedene, etwa je Serie ein eigenes. Jedes Video wird bildgenau an
  seinem eigenen Intro-Ende geschnitten.

**Lieber nicht schneiden als Inhalt entfernen**

Im automatischen Modus gilt ein Video nur als sicher, wenn das Intro ganz am Anfang beginnt,
innerhalb der Suchlänge endet, danach eindeutig Neues kommt und es mindestens 1,5 s lang ist und
in mindestens 5 Videos vorkommt. Alles andere wird mit Begründung angezeigt, aber nicht
ausgewählt. Bildgenaue Korrekturen sind für eine ganze Gruppe oder ein einzelnes Video möglich.

**Vier Schnittmodi**

- **Copy:** verlustfrei und sehr schnell, im automatischen Modus am letzten Keyframe im Intro.
  Es nimmt nie Inhalt weg, lässt aber eventuell einen Rest des Intros stehen.
- **CPU:** framegenau mit x264, AV1-Quellen und WebM bleiben AV1 per SVT-AV1.
- **GPU:** framegenau per VAAPI im Codec der Quelle.
- **GPU + CPU parallel:** Die GPU nimmt die größten Dateien, die CPU gleichzeitig die kleinsten.

**Bedienung**

- Referenz-Video und Ordner lassen sich per Drag & Drop aus dem Dateimanager setzen.
- Die Trefferliste zeigt Vorschaubilder, auch von knapp nicht erkannten Videos, damit sich leicht
  entscheiden lässt, ob dort doch das Intro steckt.
- Getestet mit 100 000 Videos. Die Suche aus dem Cache dauert wenige Sekunden, und der
  Speicherbedarf bleibt in der normalen Suche unter 1 GB.
- Die GPU wird höchstens zu 95 % ausgelastet, damit nebenher z. B. Videos flüssig laufen.

## Duplikat-Finder

Findet doppelte Videos und auf Wunsch Bilder anhand ihres Inhalts, nicht nur bei gleichem Namen
oder gleicher Größe. Gestartet wird `duplikat-finder` (Oberfläche) oder `duplikat-finder-cli`
(Terminal). Das Programm ist ein Neubau nach dem Vorbild des „Video Duplicate Finder“.

**Was erkannt wird**

- Kopien, die anders kodiert (H.264, HEVC, VP9, AV1), verkleinert, beschnitten oder gespiegelt
  sind oder schwarze Balken haben.
- Gekürzte Fassungen (Teilclips) über einen Ton-Fingerabdruck.
- Bilder wie JPEG, PNG, WebP, GIF, AVIF und HEIC, wenn die Option aktiviert ist.

**Robust und schnell**

- Dekodiert wird in eigenen Hilfsprozessen mit Zeit- und Speichergrenze. Stürzt einer ab, wird er
  neu gestartet und nur diese Datei als „nicht lesbar“ gemerkt.
- CPU und GPU (VAAPI) arbeiten gleichzeitig aus einer Warteschlange. Was die GPU nicht kann,
  übernimmt die CPU.
- Die Ergebnisse liegen laufend in einer SQLite-Datenbank mit etwa 1 KB je Datei, ein Abbruch
  verliert also nichts.
- 100 000 Videos werden in wenigen Sekunden verglichen.
- Das Programm läuft mit niedriger Priorität, und CPU-Prozesse pausieren automatisch, wenn andere
  Programme die CPU brauchen oder der Speicher knapp wird.

**Trefferliste**

- Gruppen ähnlicher Dateien mit Vorschaubildern und allen Eckdaten, die besten Werte sind grün
  markiert.
- Auswahlregeln markieren z. B. die niedrigere Qualität, ältere Dateien, 100 % gleiche oder
  Teilclips. In jeder Gruppe bleibt mindestens eine Datei unmarkiert.
- Markierte Dateien lassen sich in den Papierkorb legen, löschen, verschieben, kopieren oder
  durch Hardlinks bzw. symbolische Links auf die beste Datei ersetzen.
- „Vergleichen“ zeigt alle Vergleichsbilder nebeneinander, „Kein Duplikat“ merkt sich das Paar
  dauerhaft.
- Filter und Sortierung, außerdem Export als JSON, CSV und HTML.

**Weitere Funktionen**

- Täglicher geplanter Suchlauf, auch ohne geöffnetes Fenster.
- Vergleich zweier Dateien als Diagnose.
- Datenbankpflege, z. B. Pfade nach einem umbenannten Laufwerk ändern.

## Mediathek Downloader

Lädt Filme und Serien aus den Mediatheken von ZDF, ZDFneo, ARD, ONE, WDR, SWR, NDR, rbb, 3sat und
Netzkino, von der einzelnen Folge bis zur ganzen Serie mit allen Staffeln. Gestartet wird `mediathek-downloader` (Oberfläche) oder
`mediathek-downloader-cli` (Terminal).

Bis Version 1.2 hieß das Programm ZDF-Downloader. Bestehende Installationen wechseln beim nächsten
Update von selbst, Einstellungen, Verlauf und Warteschlange bleiben erhalten.

**Auswählen**

- Die Vorschau läuft über eine, mehrere oder alle Mediatheken. Gleiche Titel erscheinen nur
  einmal.
- Die Vorschau zeigt neue Filme und Serien als Kacheln mit Titelbild, gefiltert nach Kategorien wie
  Komödie, Drama oder Krimi und nach Sprache. Die Kategorien stammen auch von IMDb.
  Häkchen sammeln eine Auswahl, „Herunterladen“ startet alles auf einmal.
- Ruht die Maus auf einer Kachel, zeigt ein Popup FSK, Länge, Datum, IMDb-Bewertung und Beschreibung.
- Die Detailansicht zeigt zusätzlich Staffeln und Folgen, der IMDb-Knopf öffnet die passende Seite
  im Browser.
- Bilder und Informationen bleiben im Zwischenspeicher und werden nicht neu geladen.
- Jede Folge zeigt, ob sie geladen, angefangen oder noch offen ist. Serien wählen nur aus, was fehlt.
- Die Bibliothek listet alles Geladene nach Sendung.
- Die Seite „Neue Folgen“ zeigt, was bei geladenen Serien seit dem letzten Download dazukam.

**Laden**

- Mehrere Sprachen und Untertitel sind wählbar, Standard ist alles, zusammen in einer MKV-Datei.
- Downloads lassen sich pausieren und auch nach einem Neustart fortsetzen.
- Das Tempolimit gilt für alle Downloads zusammen und sofort.
- Auf Wunsch wandelt das Programm die Videos nach AV1 um, per GPU (VAAPI) oder CPU. Das Ergebnis
  wird geprüft, und die Datei bleibt erhalten, wenn AV1 nicht kleiner wäre.

**Gut zu wissen**

- Die Qualität reicht bis 1080p. Geladen werden kann nur, was gerade in der Mediathek steht, und
  manche Titel sind nur aus Deutschland erreichbar.
- Das Programm umgeht keinen Kopierschutz. Die Dateien sind für den privaten Gebrauch gedacht und
  dürfen nicht weitergegeben werden.
- Die Suche läuft über MediathekViewWeb, das Programm steht in keiner Verbindung zu den Sendern.
- Information courtesy of IMDb (https://www.imdb.com). Used with permission.

## Wie die Paketquelle funktioniert

Die Projekte bauen ihre Pakete bei jedem Versions-Tag per GitHub Actions unter Ubuntu 24.04 und
legen sie nach `pool/`. Daraus baut `scripts/build-index.sh` die signierten Indizes, die über
GitHub Pages ausgeliefert werden. Der öffentliche Schlüssel ist [`vprepo.gpg`](vprepo.gpg).
Mit jedem Paket kommt der dazugehörige Stand des Quellcodes nach `quellcode/`.

## Entfernen

```bash
sudo apt remove av1-converter intro-cutter duplikat-finder mediathek-downloader
```

```bash
sudo rm /etc/apt/sources.list.d/vprepo.list /etc/apt/keyrings/vprepo.gpg
```

## Lizenz

Copyright (C) 2026 EureLordschaft008

Alle vier Programme und die Skripte dieses Repositorys stehen unter der GNU General Public License,
Version 3 oder neuer (GPL-3.0-or-later), siehe [LICENSE](LICENSE). Sie werden ohne jede Gewähr
bereitgestellt.
