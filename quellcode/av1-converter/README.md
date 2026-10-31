# AV1-Converter 1.4.0

Wandelt alle Videos eines Ordners (inkl. Unterordner) nach AV1 um, um Speicherplatz zu sparen –
mit den Einstellungen des HandBrake-Profils **„AV1 MKV 2160p60 4K“**. Als Programm mit
grafischer Oberfläche (`av1-converter`, Aufbau und Optik wie beim Intro-Cutter) und als
Terminal-Version (`av1-converter-cli`, z. B. für lange Läufe in tmux).

## Was genau passiert

| | CPU | GPU |
|---|---|---|
| Werkzeug | HandBrake selbst mit dem Profil (Flatpak `fr.handbrake.ghb` bevorzugt, sonst `HandBrakeCLI`) | ffmpeg mit VAAPI (`av1_vaapi`) |
| Video | SVT-AV1 10 Bit, Preset und RF laut Profil (HandBrake 1.11: Preset 4, RF 35, Tune PSNR) | AV1 10 Bit, Qualität 64 (CQP) |
| Bild | Auto-Crop, Decomb, höchstens 3840×2160, Peak 60 Bilder/s | dieselben Werte: Zuschnitt aus HandBrakes eigenem Scan, Deinterlacing nur bei Interlaced-Material, höchstens 2160p, Peak 60 Bilder/s, Keyframe alle 10 s |
| Ton | erste Tonspur → Opus 160 kbit/s Stereo | ebenso |
| Untertitel | keine (erzwungene fremdsprachige werden eingebrannt) | keine |
| Sonst | Kapitel, MKV | Kapitel, Metadaten, MKV |

Ton und Untertitel lassen sich abweichend vom Profil einstellen: *alle Tonspuren als Opus*,
*alle Tonspuren unverändert (verlustfrei)* und *alle Untertitel übernehmen*.

**Zu „verlustfrei“:** Das Profil ist – wie jede AV1-Kodierung, die Platz spart – nicht
mathematisch verlustfrei, sondern auf eine Qualität ausgelegt, bei der man keinen Unterschied
sieht. Echt verlustfreies AV1 wäre um ein Vielfaches *größer* als die Originale. Wer nichts am
Ton verlieren will, wählt „Alle Tonspuren unverändert übernehmen“.

### Gemessen (8-Kern-CPU, Grafikkarte mit AV1-Encoder, System ohne andere Last)

Testmaterial: ein Spielfilm (1080p H.264, 2,8 Mbit/s, schwarze Balken) und daraus erzeugte
4K-Clips (H.264 bzw. HEVC 10 Bit).

| | Größe (1080p) | Bildqualität | Tempo 1080p | Tempo 4K | CPU-Last | RAM je Job |
|---|---|---|---|---|---|---|
| **GPU** (Standard) | −51 % | SSIM 0,9929 | 1 Job 575, **2–3 Jobs ~850 Bilder/s** | 150–180 (H.264), ~100 (HEVC) | 1080p 6 Kerne, 4K-HEVC 3 Kerne | ~0,5 GB |
| **CPU** (HandBrake-Profil) | **−65 %** | SSIM 0,9929 – gleich | 1 Job 21,5, 3 Jobs nur 25,6 Bilder/s | ~8 Bilder/s | alle Kerne | 3,2 GB (4K: 6,2 GB) |
| GPU + CPU parallel | – | – | insgesamt nur ~310–400 Bilder/s | – | – | – |

- Die GPU ist **25- bis 40-mal so schnell** bei gleicher Bildqualität; HandBrake macht die
  Dateien noch einmal 25–40 % kleiner. Für große Sammlungen ist deshalb **GPU der Standard**;
  HandBrake springt nur ein, wenn die GPU an einer Datei scheitert.
- **GPU + CPU parallel lohnt sich nicht:** HandBrake nimmt den GPU-Jobs die CPU weg (die
  Bilder werden auf der CPU dekodiert bzw. kopiert) – die GPU fällt von ~850 auf ~300–400
  Bilder/s, HandBrake steuert nur ~12 Bilder/s bei. Der Modus bleibt wählbar für den Fall, dass
  möglichst kleine Dateien wichtiger sind als Tempo.
- **Mehrere HandBrake-Jobs lohnen sich kaum** (+16 % bei 2 Jobs), kosten aber je 3–6 GB RAM.
  Standard ist 1 CPU-Job.
- **Wer dekodiert?** HEVC und VP9 dekodiert die GPU (4K-HEVC: 3 statt 9 CPU-Kerne bei gleichem
  Tempo), H.264 die CPU (bei 1080p schneller als die GPU, weil die Bilder für den Zuschnitt
  zurückkopiert werden müssten). Scheitert der GPU-Dekoder, wird mit CPU-Dekoder wiederholt.
- Die Balkenerkennung (HandBrakes eigener Scan, damit der Zuschnitt dem Profil entspricht)
  kostet ~1,2–1,6 s je Datei und läuft parallel zu den anderen GPU-Jobs.

**Zeitbedarf je Terabyte** (grobe Rechnung, nur GPU): 1080p-H.264-Material wie oben sind rund
800 Stunden Video → gut **1 Tag**; 4K-HEVC mit ~25 Mbit/s → etwa **1 Tag**; dazu die
Balkenerkennung aller Dateien. Nur mit der CPU wären es über fünf Wochen. Die Restzeit zeigt das
Programm während des Laufs an. Pausieren, Abbrechen und später weitermachen ist jederzeit
möglich.

### Getestet mit 90 000 Videos

300 Ordner mit je 300 Videos (90 000 echte, kleine MP4-Dateien):

- Erste Suche (jede Datei per ffprobe, 16 parallel): ~9 min lokal; Zwischenstände werden
  minütlich gesichert. **Jede weitere Suche: 1,6 s** (Prüf-Cache), Speicherbedarf ~110 MB.
- Oberfläche mit 90 000 Einträgen: Liste nach 1 s da, „Alle“/„Keine“ sofort (Zeilen werden in
  Portionen von 200 angelegt; ausgewählt bleiben trotzdem alle).
- Umwandlung über mehrere Minuten: Speicher (137 MB), Threads und Dateihandles der Oberfläche
  bleiben konstant, keine Zombie-Prozesse, Protokoll im Fenster auf 5 000 Zeilen begrenzt.
- Abbruch mittendrin und neue Suche: die fertigen Videos werden als „Früher umgewandelt“
  übersprungen, keine Temp-Dateien bleiben zurück.

## Sicherheit und Platz

- **Originale werden nie vorab angefasst.** Kodiert wird in eine versteckte Temp-Datei
  (`.<Name>.av1-tmp-….mkv`) neben dem Ziel. Danach wird geprüft (AV1-Spur vorhanden, Dauer
  stimmt); erst dann wird das Original ersetzt bzw. die Datei in den Ausgabeordner gelegt.
- **Kein Gewinn → Original bleibt.** Ist die AV1-Datei nicht mindestens *Mindestersparnis*
  % (Standard 10 %) kleiner, wird sie verworfen. Bei GPU-Jobs wird das schon ab 20 %
  Fortschritt hochgerechnet und der Job früh abgebrochen – das spart viel Zeit bei bereits gut
  komprimiertem HEVC. (HandBrake schreibt seine Datei erst am Ende; dort geht das nicht.)
- **Weitermachen statt neu anfangen:** Jede bearbeitete Datei steht in
  `~/.av1-converter/erledigt.tsv` (Ergebnis, Größe, Änderungszeit). Die nächste Suche
  überspringt sie – auch nach Abbruch, Absturz oder Neustart. Schon vorhandene AV1-Videos werden
  immer übersprungen. Zurücksetzen: Einstellungen → „Bearbeitete Dateien“ bzw. Menü.
- **Schnelle Suche über viele TB:** ffprobe-Ergebnisse werden in
  `~/.av1-converter/probe_cache.tsv` gemerkt (Schlüssel: Pfad, Größe, Änderungszeit).
- Beim Ersetzen wird aus `Film.mp4` → `Film.mkv`. Gibt es `Film.mkv` schon (ein anderes Video),
  heißt das Ergebnis `Film.av1.mkv`. Änderungsdatum und Dateirechte des Originals werden
  übernommen.
- Scheitert ein GPU-Job, wird er mit CPU-Dekoder und danach mit HandBrake wiederholt. Hängt
  ein Encoder 15 min ohne Fortschritt, wird er beendet.
- Disc-Ordner (`BDMV`, `VIDEO_TS`), versteckte Ordner (z. B. `.zfs`) und Symlinks werden nicht
  angefasst. Bei weniger als 10 GB freiem Platz am Ziel warten neue Jobs, bis wieder Platz ist.
- Encoder laufen mit niedriger CPU- und I/O-Priorität (HandBrake am niedrigsten, damit die
  GPU-Jobs nicht warten); neue CPU-Jobs starten nur bei genug freiem Arbeitsspeicher (3,5 GB,
  über 1440p 7 GB). Standby ist während der Arbeit gesperrt.
- Ausgelegt auf große Sammlungen: Bilder, Untertitel und `.nfo`-Dateien werden beim Durchsuchen
  ohne Zugriff übergangen; Einträge gelöschter Videos fliegen aus dem Prüf-Cache; die Liste
  bearbeiteter Dateien wird verdichtet, wenn sie durch Wiederholungen anwächst.

## HDR-Videos

Gemessen mit einem HDR10-Testclip (BT.2020, PQ, Mastering Display 1000 cd/m², MaxCLL 1000):
Die GPU (VAAPI) behält Farbraum und Kennlinie – das Video bleibt HDR –, schreibt aber **keine
Helligkeitsangaben** (Mastering Display, MaxCLL/MaxFALL). HandBrake überträgt alles. Auswahl
unter Einstellungen → Qualität → „HDR-Videos“ (Terminal: `--hdr container|handbrake|ohne`):

| Auswahl | Weg | HDR-Angaben | Tempo |
|---|---|---|---|
| **Per GPU – HDR-Angaben in den MKV-Container** (Standard) | GPU; danach liest das Programm die Angaben aus dem Original und schreibt sie per `mkvpropedit` in den Container. Dolby Vision und HDR10+ gehen an HandBrake | HDR10 vollständig (im Container) | schnell |
| Vollständig per HandBrake | HandBrake | alles, auch Dolby Vision/HDR10+ | langsam (4K ~8 Bilder/s) |
| Per GPU ohne Helligkeitsangaben | GPU | nur Farbraum/Kennlinie | schnell |

`mkvtoolnix` wird mit dem Paket installiert. Fehlt `mkvpropedit` oder schlägt es bei einer Datei
fehl, übernimmt HandBrake – HDR-Angaben gehen nie stillschweigend verloren. In der Videoliste
steht bei jedem HDR-Video, welcher Weg genommen wird.

## Stark komprimierte Videos (z. B. YouTube-Downloads)

Bei Videos mit sehr niedriger Bitrate macht der GPU-Encoder die Datei **größer** (gemessen an
YouTube-Downloads mit 0,4–1,7 Mbit/s: +21 bis +50 %). HandBrake schafft bei gleicher
Bildqualität (SSIM 0,988–0,993) noch **13–27 % weniger**.

- **Modus „GPU“:** Bringt die GPU keinen Gewinn, bleibt das Original (schnell, aber diese
  Videos werden nicht kleiner). HandBrake springt nur bei GPU-Fehlern ein.
- **Modus „GPU + CPU parallel“** (für möglichst kleine Dateien): Bringt die GPU keinen Gewinn
  (auch per Hochrechnung früh erkannt), versucht es **HandBrake automatisch noch einmal**. Erst
  wenn auch HandBrake nichts spart, bleibt das Original. Videos, die früher per GPU ohne Gewinn
  blieben, nimmt die Suche in diesem Modus wieder auf und gibt sie direkt an HandBrake („per
  HandBrake“ in der Liste). Dauert deutlich länger (1080p ~21 Bilder/s); die Restdauer rechnet
  das mit ein.

## Später entscheiden: Originale ersetzen

Wer ohne „Originale ersetzen“ umwandelt, findet die Ergebnisse im Ausgabeordner und kann sie
erst einmal ansehen. Ersetzen lassen sich die Originale danach jederzeit – wie beim
Intro-Cutter, aber auch Tage später oder nach einem Neustart:

- **Fertig-Seite:** Knopf „N fertige Dateien an den Platz der Originale verschieben“.
- **Einstellungen → Speicherplatz:** Zeile „Noch nicht an den Platz der Originale verschoben“
  mit Anzahl und freiwerdendem Platz (erscheint nur, wenn etwas ansteht).
- **Menü:** „Fertige Dateien an den Platz der Originale verschieben …“
- **Terminal:** `av1-converter-cli --verschieben`

Vor dem Verschieben fragt das Programm nach und zeigt, wie viel Platz frei wird. Dann:

- Aus `Film.mp4` wird `Film.mkv` im selben Ordner, das Original wird gelöscht; ein Original
  `Film.mkv` wird direkt ersetzt. Gibt es den Namen schon (anderes Video): `Film.av1.mkv`.
- **Übersprungen** wird, wenn sich das Original seit der Umwandlung geändert hat (Größe oder
  Datum) oder nicht mehr da ist – das Ergebnis bleibt dann im Ausgabeordner.
- Liegen Ausgabeordner und Originale auf verschiedenen Laufwerken, wird erst vollständig
  kopiert, die Größe geprüft und dann atomar ausgetauscht; vorher wird der freie Platz geprüft.
- Rechte und Änderungsdatum des Originals bleiben erhalten.
- Fortschritt nach Dateien und GB; „Nach der aktuellen Datei anhalten“ ist jederzeit möglich,
  der Rest bleibt für später in der Liste.
- Danach werden die leer gewordenen Ausgabeordner entfernt – nur die, die das Programm selbst
  angelegt hat, und nur, wenn sie wirklich leer sind.

Die Liste steht in `~/.av1-converter/ausgabe.tsv`, die angelegten Ordner in
`~/.av1-converter/angelegte_ordner.txt`.

Ergebnisse früherer Läufe (auch aus Versionen vor 1.2.0) nimmt die nächste Suche mit demselben
Quell- und Ausgabeordner automatisch in die Liste auf, wenn sie laut Liste der bearbeiteten
Dateien erfolgreich umgewandelt wurden und das Original unverändert ist. Ausgabeordner, die eine
ältere Version angelegt hat, kennt das Programm nicht – die bleiben nach dem Verschieben stehen
(leer) und lassen sich von Hand löschen.

## Restdauer

- **Vor dem Start** zeigt die Videoliste die geschätzte Gesamtdauer der Auswahl (unten neben
  der Ersparnis), die Terminal-Version nach der Suche.
- **Während der Umwandlung** steht oben groß: *Restdauer: ca. 3 Tage 4 h · fertig voraussichtlich
  Mittwoch, 15.10., 14:20*.
- Geschätzt wird nach Arbeit statt nach Datenmenge: Länge × Bildrate × Auflösung jedes Videos,
  bewertet mit den gemessenen Werten (GPU, CPU, Anzahl der Jobs). Ab der ersten Minute rechnet
  das Programm mit dem tatsächlich gemessenen Tempo; die letzten 30 Minuten zählen am stärksten,
  damit sich die Anzeige an Wechsel anpasst (z. B. eine Strecke mit 4K-Videos, ein langsameres
  NAS oder andere Programme nebenher). Pausen zählen nicht mit.
- Am Ende jedes Laufs merkt sich das Programm das tatsächliche Tempo im Verhältnis zur Schätzung
  (`~/.av1-converter/tempo.ini`, je Modus). So stimmt auch die Schätzung vor dem nächsten Start.
  Getestet: unter Last (Intro-Cutter lief nebenher) sagte die Anzeige nach gut einer Minute das
  Ende auf die Minute genau voraus.

## Bedienung

1. **Quellordner** wählen (oder aus dem Dateimanager auf die Zeile ziehen).
2. **Originale ersetzen** einschalten, wenn sofort Platz frei werden soll – sonst landen die
   Ergebnisse gespiegelt im Ausgabeordner (`<Quelle>_av1`).
3. **Hardware** wählen (Standard: GPU), ggf. Anzahl der Jobs und Qualität.
4. **Videos suchen** → Liste prüfen (größte zuerst, Häkchen abwählbar) → **Umwandeln**.
5. Während der Umwandlung: GPU- und CPU-Jobs mit Bildern/s und Restzeit, Gesamtfortschritt,
   bisher gesparter Platz, Protokoll. **Pausieren** hält alle Encoder sofort an (auch den
   Flatpak-Sandkasten von HandBrake). „Wenn fertig“: nichts tun, herunterfahren oder beenden.

Einstellungen (inkl. Ordner und Fenstergröße) werden sofort gespeichert und beim nächsten
Start wiederhergestellt.

### Terminal-Version

```bash
av1-converter-cli --ersetzen ~/Videos
```

```bash
av1-converter-cli --nur-anzeigen ~/Videos
```

Optionen: `--modus cpu|gpu|hybrid`, `--ersetzen`, `--ausgabe ORDNER`, `--cpu-jobs N`,
`--gpu-jobs N`, `--rf ZAHL`, `--gpu-qualitaet ZAHL`, `--ton profil|alle|kopie`,
`--untertitel profil|alle`, `--mindestersparnis P`, `--datum-neu`, `--nur-anzeigen`,
`--verschieben` (fertige Ergebnisse an den Platz der Originale, ohne Quellordner).
Strg+C bricht ab (fertige Dateien bleiben), Strg+Z pausiert (weiter mit `fg`). Exit-Codes:
0 = Erfolg, 1 = Fehler bei mindestens einer Datei, 2 = Aufruffehler, 130 = abgebrochen.

## Installieren

Einmalig die Paketquelle VPRepo einbinden und den AV1-Converter installieren:

```bash
sudo install -d -m 755 /etc/apt/keyrings
curl -fsSL https://eurelordschaft008.github.io/VPRepo/vprepo.gpg | sudo tee /etc/apt/keyrings/vprepo.gpg >/dev/null
echo 'deb [signed-by=/etc/apt/keyrings/vprepo.gpg] https://eurelordschaft008.github.io/VPRepo stable main' | sudo tee /etc/apt/sources.list.d/vprepo.list
sudo apt update
sudo apt install av1-converter
```

Neue Versionen erscheinen danach automatisch in der Aktualisierungsverwaltung von Linux Mint.

Danach im Menü unter „Sound & Video“ → „AV1-Converter“. Für die CPU-Jobs wird HandBrake
empfohlen (am besten als Flatpak `fr.handbrake.ghb`); ohne HandBrake nutzt das Programm ffmpeg mit
SVT-AV1 und denselben Werten (die SVT-AV1-Version von Ubuntu 24.04 ist allerdings älter und
langsamer).

Deinstallieren:

```bash
sudo apt remove av1-converter
```

## Neue Version veröffentlichen

`VERSION` (bei reinen Paketänderungen `DEBREV`) im `Makefile` erhöhen, committen und einen
passenden Tag pushen, z. B. `v1.4.1` bzw. `v1.4.1-2`. GitHub Actions baut dann das Paket,
legt es als Release an und stellt es über VPRepo bereit. Eine Versionsnummer lässt sich nur
einmal veröffentlichen.

## Selbst bauen

Benötigt `build-essential`, `libgtk-3-dev` und `ffmpeg`.

```bash
make        # baut av1-converter und av1-converter-cli
make deb    # baut das .deb-Paket
```

## Aufbau

- `src/ac_core.c`, `src/ac_core.h` – Engine: Suche, Analyse (ffprobe), HandBrake-/GPU-Erkennung,
  Befehle, Job-Verteilung auf CPU/GPU, Pause, Watchdog, Prüfung, Ablage
- `src/ac_gui.c` – grafische Oberfläche (GTK 3, im Stil des Intro-Cutters)
- `src/ac_cli.c` – Terminal-Version
- `data/` – Menüeintrag, Icon, Paketbeschreibung

Namenskonventionen wie beim Intro-Cutter: `ac_*` = öffentliche Schnittstelle der Engine
(`ac_core.h`), `g_*`/`run.*` = globaler Zustand der Engine, `ui` = Zustand der Oberfläche,
GROSSBUCHSTABEN = Konstanten. Lauf-Protokoll mit allen Befehlszeilen:
`~/.av1-converter/last_run.log`.

## Lizenz

Copyright (C) 2026 EureLordschaft008

Dieses Programm steht unter der GNU General Public License, Version 3 oder neuer
(GPL-3.0-or-later), siehe [LICENSE](LICENSE). Es wird ohne jede Gewähr bereitgestellt.
